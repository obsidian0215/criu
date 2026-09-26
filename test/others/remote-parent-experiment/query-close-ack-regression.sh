#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
PROXY="$SCRIPT_DIR/query_close_ack_proxy.py"
WORK_DIR="$SCRIPT_DIR/query-close-ack-work"
RESULT_DIR="$SCRIPT_DIR/results/query-close-ack"
WORKLOAD="$WORK_DIR/parent-identity-workload"
STATE_FILE="$WORK_DIR/workload.state"
EXPECTED_FILE="$WORK_DIR/expected.hex"
PID=""
PAGE_SERVER_PID=""
PROXY_PID=""
SERVER_PORT=""

cleanup() {
	if [ -n "$PROXY_PID" ] && kill -0 "$PROXY_PID" 2>/dev/null; then
		kill -KILL "$PROXY_PID" 2>/dev/null || true
		wait "$PROXY_PID" 2>/dev/null || true
	fi
	if [ -n "$PAGE_SERVER_PID" ] && kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
		wait "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		kill -KILL "$PID" 2>/dev/null || true
		wait "$PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	find "$WORK_DIR" -type f \( -name '*.log' -o -name '*.state' -o -name '*.json' \) -print -exec sh -c '
		for file do
			echo "===== $file ====="
			tail -n 160 "$file"
		done
	' sh {} + 2>/dev/null >&2 || true
	exit 1
}

free_port() {
	python3 - <<'PY'
import socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(('127.0.0.1', 0))
    print(sock.getsockname()[1])
PY
}

wait_listen() {
	local pid=$1 port=$2
	for _ in $(seq 1 150); do
		kill -0 "$pid" 2>/dev/null || return 1
		ss -H -ltn "sport = :$port" | grep -q . && return 0
		sleep 0.02
	done
	return 1
}

wait_ready() {
	local pid=$1 ready=$2
	for _ in $(seq 1 150); do
		[ -s "$ready" ] && return 0
		kill -0 "$pid" 2>/dev/null || return 1
		sleep 0.02
	done
	return 1
}

start_server() {
	local directory=$1 parent=${2:-}
	local args=()
	SERVER_PORT=$(free_port)
	[ -z "$parent" ] || args+=(--prev-images-dir "../$(basename "$parent")")
	"${CRIU_CMD[@]}" page-server -D "$directory" -o page-server.log -v4 --port "$SERVER_PORT" "${args[@]}" \
		>"$directory/server-command.log" 2>&1 &
	PAGE_SERVER_PID=$!
	wait_listen "$PAGE_SERVER_PID" "$SERVER_PORT" || fail "page server did not start"
}

finish_server() {
	local status=0
	wait "$PAGE_SERVER_PID" || status=$?
	PAGE_SERVER_PID=""
	return "$status"
}

field() {
	local name=$1
	sed -n "s/.* $name=\\([^ ]*\\).*/\\1/p" "$STATE_FILE"
}

wait_phase() {
	local phase=$1
	for _ in $(seq 1 200); do
		grep -q "^$phase " "$STATE_FILE" 2>/dev/null && return 0
		[ -z "$PID" ] || kill -0 "$PID" 2>/dev/null || return 1
		sleep 0.05
	done
	return 1
}

rm -rf "$WORK_DIR" "$RESULT_DIR"
mkdir -p "$WORK_DIR" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/parent_identity_workload.c" -o "$WORKLOAD"
printf '31\n' >"$EXPECTED_FILE"
"$WORKLOAD" "$STATE_FILE" "$EXPECTED_FILE" >"$WORK_DIR/workload.log" 2>&1 &
PID=$!
ROOT_PID=$PID
wait_phase READY || fail "workload did not become ready"

SOURCE_PRE="$WORK_DIR/source-pre"
TARGET_PRE="$WORK_DIR/target-pre"
SOURCE_FINAL="$WORK_DIR/source-final"
TARGET_FINAL="$WORK_DIR/target-final"
QUERY_DIR="$WORK_DIR/query-final"
mkdir -p "$SOURCE_PRE" "$TARGET_PRE" "$SOURCE_FINAL" "$TARGET_FINAL" "$QUERY_DIR"
start_server "$TARGET_PRE"
"${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$SOURCE_PRE" -o dump.log \
	-t "$PID" -v4 --track-mem --page-server --address 127.0.0.1 --port "$SERVER_PORT" ||
	fail "remote pre-dump failed"
finish_server || fail "pre-dump page server failed"

start_server "$QUERY_DIR" "$TARGET_PRE"
TARGET_PORT=$SERVER_PORT
PROXY_PORT=$(free_port)
READY="$WORK_DIR/proxy.ready"
STATS="$WORK_DIR/proxy-stats.json"
python3 "$PROXY" --listen-port "$PROXY_PORT" --target-port "$TARGET_PORT" \
	--stats "$STATS" --ready "$READY" >"$WORK_DIR/proxy.log" 2>&1 &
PROXY_PID=$!
wait_ready "$PROXY_PID" "$READY" || fail "close-ack proxy did not start"

"${CRIU_CMD[@]}" dump -D "$SOURCE_FINAL" -o dump.log -t "$PID" -v4 --track-mem \
	--prev-images-dir "../$(basename "$SOURCE_PRE")" \
	--page-server-parent --address 127.0.0.1 --port "$PROXY_PORT" ||
	fail "lost query close acknowledgement failed an otherwise complete final dump"
PID=""
wait "$PROXY_PID" || true
PROXY_PID=""
finish_server || true
[ -s "$STATS" ] || fail "proxy produced no statistics"
python3 - "$STATS" <<'PY' || fail "proxy did not drop the close acknowledgement"
import json
import sys
value = json.load(open(sys.argv[1]))
if not value['close_response_dropped']:
    raise SystemExit(1)
if value['source_to_server'] <= 0 or value['server_to_source'] <= 0:
    raise SystemExit(1)
PY

cp -a "$SOURCE_FINAL/." "$TARGET_FINAL/"
rm -f "$TARGET_FINAL/parent"
ln -s ../target-pre "$TARGET_FINAL/parent"
"${CRIU_CMD[@]}" restore -D "$TARGET_FINAL" -o restore.log -v4 -d || fail "restore failed"
PID=$ROOT_PID
kill -0 "$PID" 2>/dev/null || fail "restored workload is missing"
kill -USR2 "$PID" || fail "unable to run restored workload oracle"
wait_phase PASS || fail "restored memory is incorrect"
PID=""

cat >"$RESULT_DIR/summary.json" <<EOF_JSON
{
  "route": "local-no-parent",
  "case": "lost-query-close-ack",
  "status": "PASS",
  "detail": "all range replies were received and the final image remained usable"
}
EOF_JSON
printf 'QUERY-CLOSE-ACK PASS\n'
rm -rf "$WORK_DIR"
