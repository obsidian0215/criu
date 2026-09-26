#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
QUERY_FINAL="$SCRIPT_DIR/query-final-dump.sh"
WORK_DIR="$SCRIPT_DIR/query-relocation-work"
RESULT_DIR="$SCRIPT_DIR/results/query-relocation"
HELPER="$WORK_DIR/topology-workload"
ORACLE="$SCRIPT_DIR/topology_oracle.py"
PID=""
LAUNCHER_PID=""
PAGE_SERVER_PID=""
SERVER_PORT=""

cleanup() {
	if [ -n "$PAGE_SERVER_PID" ] && kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
		wait "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		kill -KILL -- "-$PID" 2>/dev/null || kill -KILL "$PID" 2>/dev/null || true
	fi
	if [ -n "$LAUNCHER_PID" ]; then
		wait "$LAUNCHER_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	find "$WORK_DIR" -type f \( -name '*.log' -o -name '*.json' -o -name '*.state' \) \
		-print -exec sh -c 'echo "===== $1 ====="; tail -n 160 "$1"' _ {} \; 2>/dev/null >&2 || true
	exit 1
}

free_port() {
	python3 - <<'PY'
import socket
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    print(sock.getsockname()[1])
PY
}

wait_listen() {
	local port=$1
	for _ in $(seq 1 200); do
		kill -0 "$PAGE_SERVER_PID" 2>/dev/null || return 1
		ss -H -ltn "sport = :$port" | grep -q . && return 0
		sleep 0.02
	done
	return 1
}

start_server() {
	local directory=$1
	SERVER_PORT=$(free_port)
	"${CRIU_CMD[@]}" page-server -D "$directory" -o page-server.log -v4 \
		--port "$SERVER_PORT" >"$directory/server-command.log" 2>&1 &
	PAGE_SERVER_PID=$!
	wait_listen "$SERVER_PORT" || fail "page server did not start"
}

finish_server() {
	local status=0
	wait "$PAGE_SERVER_PID" || status=$?
	PAGE_SERVER_PID=""
	return "$status"
}

wait_state() {
	local file=$1 phase=$2
	for _ in $(seq 1 300); do
		[ -f "$file" ] && grep -qx "phase=$phase" "$file" && return 0
		[ -n "$PID" ] && ! kill -0 "$PID" 2>/dev/null && return 1
		sleep 0.02
	done
	return 1
}

state_value() {
	local file=$1 key=$2
	awk -F= -v key="$key" '$1 == key { print $2; exit }' "$file"
}

json_value() {
	local file=$1 key=$2
	python3 - "$file" "$key" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1]))[sys.argv[2]])
PY
}

restore_and_verify() {
	local image=$1 base=$2 restore_pid
	rm -f "$base/restore.pid" "$base/result"
	"${CRIU_CMD[@]}" restore -D "$image" -o restore.log -v4 -d --shell-job \
		--pidfile "$base/restore.pid" || return 1
	for _ in $(seq 1 200); do
		[ -s "$base/restore.pid" ] && break
		sleep 0.02
	done
	[ -s "$base/restore.pid" ] || return 1
	restore_pid=$(cat "$base/restore.pid")
	kill -TERM "$restore_pid" || return 1
	for _ in $(seq 1 300); do
		[ -s "$base/result" ] && break
		kill -0 "$restore_pid" 2>/dev/null || true
		sleep 0.02
	done
	grep -qx PASS "$base/result"
}

rm -rf "$WORK_DIR" "$RESULT_DIR"
mkdir -p "$WORK_DIR" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/topology_workload.c" -o "$HELPER"

BASE="$WORK_DIR/case"
mkdir -p "$BASE"
PIDFILE="$BASE/source.pid"
STATE="$BASE/workload.state"
RESULT="$BASE/result"
setsid "$HELPER" relocate "$PIDFILE" "$STATE" "$RESULT" >"$BASE/workload.log" 2>&1 &
LAUNCHER_PID=$!
PID=$LAUNCHER_PID
wait_state "$STATE" ready || fail "workload did not become ready"
[ "$(cat "$PIDFILE")" = "$PID" ] || fail "workload pidfile mismatch"

SOURCE_PRE="$BASE/source-pre"
TARGET_PRE="$BASE/target-pre"
SOURCE_FINAL="$BASE/source-final"
QUERY_DIR="$BASE/query-final"
SOURCE_RETRY="$BASE/source-full-retry"
TARGET_RETRY="$BASE/target-full-retry"
mkdir -p "$SOURCE_PRE" "$TARGET_PRE"
start_server "$TARGET_PRE"
"${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$SOURCE_PRE" -o dump.log \
	-t "$PID" -v4 --track-mem --shell-job --page-server \
	--address 127.0.0.1 --port "$SERVER_PORT" || fail "remote pre-dump failed"
finish_server || fail "pre-dump page server failed"

kill -USR1 "$PID"
wait_state "$STATE" mutated || fail "workload did not relocate its mapping"
ADDRESS=$(state_value "$STATE" address)
ORIGINAL=$(state_value "$STATE" original_address)
SIZE=$(state_value "$STATE" size)
[ "$ADDRESS" != "$ORIGINAL" ] || fail "mremap did not move the mapping"
python3 "$ORACLE" proc-stats "$PID" "$ADDRESS" "$SIZE" >"$RESULT_DIR/proc-after-relocate.json"
python3 "$ORACLE" image-stats "$TARGET_PRE" "$PID" "$ADDRESS" "$SIZE" >"$RESULT_DIR/parent-at-relocated-address.json"
SOFTDIRTY=$(json_value "$RESULT_DIR/proc-after-relocate.json" softdirty_pages)
PARENT_COVERED=$(json_value "$RESULT_DIR/parent-at-relocated-address.json" covered_pages)
[ "$SOFTDIRTY" -eq 0 ] || fail "relocated pages unexpectedly became soft-dirty ($SOFTDIRTY)"
[ "$PARENT_COVERED" -eq 0 ] || fail "remote parent unexpectedly covers the relocated address"

set +e
bash "$QUERY_FINAL" "$PID" "$SOURCE_FINAL" "$SOURCE_PRE" "$QUERY_DIR" "$TARGET_PRE" --shell-job
status=$?
set -e
[ "$status" -ne 0 ] || fail "query-backed final accepted an uncovered relocated range"
kill -0 "$PID" 2>/dev/null || fail "rejected final dump did not resume the workload"
grep -Eiq 'not found in parent|Missing .*parent pagemap' \
	"$SOURCE_FINAL/dump.log" "$QUERY_DIR/page-server.log" 2>/dev/null ||
	fail "relocated range rejection was not reported"

mkdir -p "$SOURCE_RETRY" "$TARGET_RETRY"
"${CRIU_CMD[@]}" dump -D "$SOURCE_RETRY" -o dump.log -t "$PID" -v4 \
	--track-mem --shell-job || fail "full retry after rejection failed"
PID=""
cp -a "$SOURCE_RETRY/." "$TARGET_RETRY/"
rm -f "$TARGET_RETRY/parent"
if [ -n "$LAUNCHER_PID" ]; then
	wait "$LAUNCHER_PID" 2>/dev/null || true
	LAUNCHER_PID=""
fi
restore_and_verify "$TARGET_RETRY" "$BASE" || fail "full retry did not restore relocated data"

cat >"$RESULT_DIR/summary.json" <<EOF_JSON
{
  "route": "local-no-parent",
  "case": "relocated-clean-range",
  "status": "PASS",
  "outcome": "safe-reject-and-full-retry",
  "softdirty_pages": $SOFTDIRTY,
  "parent_covered_pages": $PARENT_COVERED
}
EOF_JSON
printf 'QUERY-RELOCATION PASS\n'
rm -rf "$WORK_DIR"
