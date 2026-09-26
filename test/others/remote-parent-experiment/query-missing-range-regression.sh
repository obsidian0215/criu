#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
QUERY_FINAL="$SCRIPT_DIR/query-final-dump.sh"
PROBE="$SCRIPT_DIR/pagemap_probe.py"
WORK_DIR="$SCRIPT_DIR/query-missing-range-work"
RESULT_DIR="$SCRIPT_DIR/results/query-missing-range"
WORKLOAD="$WORK_DIR/parentless-workload"
STATE_FILE="$WORK_DIR/workload.state"
PID=""
PAGE_SERVER_PID=""
SERVER_PORT=""

cleanup() {
	local child=""
	if [ -s "$STATE_FILE" ]; then
		child=$(sed -n 's/.* child=\([^ ]*\).*/\1/p' "$STATE_FILE")
	fi
	if [ -n "$PAGE_SERVER_PID" ] && kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
		wait "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	if [ -n "$child" ] && [ "$child" != -1 ] && kill -0 "$child" 2>/dev/null; then
		kill -KILL "$child" 2>/dev/null || true
	fi
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		kill -KILL "$PID" 2>/dev/null || true
		wait "$PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	find "$WORK_DIR" -type f \( -name '*.log' -o -name '*.state' \) -print -exec sh -c '
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

remote_predump() {
	local source=$1 target=$2 source_parent=${3:-} target_parent=${4:-}
	local args=(--track-mem --page-server --address 127.0.0.1)
	mkdir -p "$source" "$target"
	start_server "$target" "$target_parent"
	args+=(--port "$SERVER_PORT")
	[ -z "$source_parent" ] || args+=(--prev-images-dir "../$(basename "$source_parent")")
	"${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log \
		-t "$PID" -v4 "${args[@]}" || fail "remote pre-dump failed"
	finish_server || fail "page server failed"
}

probe() {
	PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" python3 "$PROBE" "$@"
}

rm -rf "$WORK_DIR" "$RESULT_DIR"
mkdir -p "$WORK_DIR" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/parentless_workload.c" -o "$WORKLOAD"
"$WORKLOAD" "$STATE_FILE" >"$WORK_DIR/workload.log" 2>&1 &
PID=$!
wait_phase READY || fail "workload did not become ready"
ROOT_PID=$PID

SOURCE_PRE1="$WORK_DIR/source-pre1"
TARGET_PRE1="$WORK_DIR/target-pre1"
SOURCE_PRE2="$WORK_DIR/source-pre2"
TARGET_PRE2="$WORK_DIR/target-pre2"
SOURCE_FINAL="$WORK_DIR/source-final"
QUERY_DIR="$WORK_DIR/query-final"

remote_predump "$SOURCE_PRE1" "$TARGET_PRE1"
kill -USR1 "$PID" || fail "unable to create post-parent mappings"
wait_phase MUTATED || fail "workload did not mutate"
NEW_MAPPING=$(field new)
[ -n "$NEW_MAPPING" ] && [ "$NEW_MAPPING" != '(nil)' ] || fail "workload did not report the new mapping"
remote_predump "$SOURCE_PRE2" "$TARGET_PRE2" "$SOURCE_PRE1" "$TARGET_PRE1"
probe "$TARGET_PRE1" "$ROOT_PID" "$NEW_MAPPING" absent >/dev/null ||
	fail "older parent unexpectedly covers the new mapping"
probe "$TARGET_PRE2" "$ROOT_PID" "$NEW_MAPPING" present >/dev/null ||
	fail "newer parent does not cover the new mapping"

set +e
bash "$QUERY_FINAL" "$PID" "$SOURCE_FINAL" "$SOURCE_PRE2" "$QUERY_DIR" "$TARGET_PRE1"
status=$?
set -e
[ "$status" -ne 0 ] || fail "final dump accepted a range absent from the queried parent"
kill -0 "$PID" 2>/dev/null || fail "failed final dump did not resume the workload"
grep -Eq 'Hole .* not found in parent|Missing [0-9a-f]+ in parent pagemap' \
	"$SOURCE_FINAL/dump.log" "$QUERY_DIR/page-server.log" ||
	fail "range rejection was not reported"
kill -USR2 "$PID" || fail "unable to run the post-failure workload oracle"
wait_phase PASS || fail "range rejection damaged the workload"
PID=""

cat >"$RESULT_DIR/summary.json" <<EOF_JSON
{
  "route": "local-no-parent",
  "case": "missing-remote-parent-range",
  "status": "PASS",
  "detail": "final dump rejected an uncovered range and resumed the workload"
}
EOF_JSON
printf 'QUERY-MISSING-RANGE PASS\n'
rm -rf "$WORK_DIR"
