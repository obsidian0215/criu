#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
WORK_DIR="$SCRIPT_DIR/relocation-work-$ROUTE"
RESULT_DIR="$SCRIPT_DIR/results/relocation"
HELPER="$WORK_DIR/topology-workload"
ORACLE="$SCRIPT_DIR/topology_oracle.py"
PAGE_SIZE=$(getconf PAGESIZE)
PID=""
LAUNCHER_PID=""
PAGE_SERVER_PID=""
SERVER_PORT=""

case "$ROUTE" in
	final-page-server|local-no-parent|local-pagemap|custom-coverage) ;;
	*) echo "invalid route: $ROUTE" >&2; exit 2 ;;
esac

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
		-print -exec sh -c 'echo "===== $1 ====="; tail -n 160 "$1"' _ {} \; 2>/dev/null || true
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
	local directory=$1 parent=${2:-}
	local args=()
	SERVER_PORT=$(free_port)
	[ -z "$parent" ] || args+=(--prev-images-dir "../$(basename "$parent")")
	"${CRIU_CMD[@]}" page-server -D "$directory" -o page-server.log -v4 \
		--port "$SERVER_PORT" "${args[@]}" >"$directory/server-command.log" 2>&1 &
	PAGE_SERVER_PID=$!
	wait_listen "$SERVER_PORT" || fail "page server did not start"
}

finish_server() {
	local status
	for _ in $(seq 1 400); do
		kill -0 "$PAGE_SERVER_PID" 2>/dev/null || break
		sleep 0.025
	done
	if kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	set +e
	wait "$PAGE_SERVER_PID"
	status=$?
	set -e
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

sum_pages() {
	local directory=$1 total=0 file
	for file in "$directory"/pages-*.img; do
		[ -e "$file" ] || continue
		total=$((total + $(wc -c < "$file")))
	done
	printf '%d\n' "$total"
}

copy_non_memory() {
	local source=$1 target=$2 path base
	for path in "$source"/*; do
		[ -e "$path" ] || continue
		base=$(basename "$path")
		case "$base" in pages-*.img|pagemap-*.img|parent) continue ;; esac
		cp -a "$path" "$target/"
	done
}

assemble_local_final() {
	local source=$1 target=$2 parent=${3:-}
	mkdir -p "$target"
	cp -a "$source/." "$target/"
	rm -f "$target/parent"
	[ -z "$parent" ] || ln -s "../$(basename "$parent")" "$target/parent"
}

run_pre_dump() {
	local source=$1 target=$2
	mkdir -p "$source" "$target"
	start_server "$target"
	if ! "${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log \
		-t "$PID" -v4 --track-mem --shell-job --page-server \
		--address 127.0.0.1 --port "$SERVER_PORT"; then
		fail "remote pre-dump failed"
	fi
	finish_server || fail "pre-dump page server failed"
}

run_parent_final() {
	local source=$1 target=$2 source_parent=$3 target_parent=$4
	mkdir -p "$source" "$target"
	if [ "$ROUTE" = final-page-server ]; then
		local source_status=0 server_status=0
		start_server "$target" "$target_parent"
		"${CRIU_CMD[@]}" dump -D "$source" -o dump.log -t "$PID" -v4 \
			--track-mem --shell-job --prev-images-dir "../$(basename "$source_parent")" \
			--page-server --address 127.0.0.1 --port "$SERVER_PORT" || source_status=$?
		finish_server || server_status=$?
		if [ "$source_status" -eq 0 ] && [ "$server_status" -eq 0 ]; then
			copy_non_memory "$source" "$target"
			return 0
		fi
		return 1
	fi

	if "${CRIU_CMD[@]}" dump -D "$source" -o dump.log -t "$PID" -v4 \
		--track-mem --shell-job --prev-images-dir "../$(basename "$source_parent")"; then
		assemble_local_final "$source" "$target" "$target_parent"
		return 0
	fi
	return 1
}

run_full_final() {
	local source=$1 target=$2
	mkdir -p "$source" "$target"
	if [ "$ROUTE" = final-page-server ]; then
		start_server "$target"
		"${CRIU_CMD[@]}" dump -D "$source" -o dump.log -t "$PID" -v4 \
			--track-mem --shell-job --page-server --address 127.0.0.1 \
			--port "$SERVER_PORT" || return 1
		finish_server || return 1
		copy_non_memory "$source" "$target"
	else
		"${CRIU_CMD[@]}" dump -D "$source" -o dump.log -t "$PID" -v4 \
			--track-mem --shell-job || return 1
		assemble_local_final "$source" "$target"
	fi
	return 0
}

reap_source() {
	if [ -n "$LAUNCHER_PID" ]; then
		wait "$LAUNCHER_PID" 2>/dev/null || true
		LAUNCHER_PID=""
	fi
	PID=""
}

restore_and_verify() {
	local image=$1 base=$2
	local restore_pid
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

json_value() {
	local file=$1 key=$2
	python3 - "$file" "$key" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))[sys.argv[2]])
PY
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
SOURCE_FINAL="$BASE/source-parent-final"
TARGET_FINAL="$BASE/target-parent-final"
SOURCE_RETRY="$BASE/source-full-retry"
TARGET_RETRY="$BASE/target-full-retry"

run_pre_dump "$SOURCE_PRE" "$TARGET_PRE"
kill -USR1 "$PID"
wait_state "$STATE" mutated || fail "workload did not relocate its mapping"
ADDRESS=$(state_value "$STATE" address)
ORIGINAL=$(state_value "$STATE" original_address)
SIZE=$(state_value "$STATE" size)
[ "$ADDRESS" != "$ORIGINAL" ] || fail "mremap did not move the mapping"

python3 "$ORACLE" proc-stats "$PID" "$ADDRESS" "$SIZE" > "$RESULT_DIR/proc-after-relocate.json"
python3 "$ORACLE" image-stats "$TARGET_PRE" "$PID" "$ADDRESS" "$SIZE" > "$RESULT_DIR/parent-at-relocated-address.json"
SOFTDIRTY=$(json_value "$RESULT_DIR/proc-after-relocate.json" softdirty_pages)
PARENT_COVERED=$(json_value "$RESULT_DIR/parent-at-relocated-address.json" covered_pages)
[ "$SOFTDIRTY" -eq 0 ] || fail "relocated clean pages unexpectedly became soft-dirty ($SOFTDIRTY)"
[ "$PARENT_COVERED" -eq 0 ] || fail "remote parent unexpectedly covers relocated address ($PARENT_COVERED pages)"

if run_parent_final "$SOURCE_FINAL" "$TARGET_FINAL" "$SOURCE_PRE" "$TARGET_PRE"; then
	reap_source
	python3 "$ORACLE" image-stats "$TARGET_FINAL" "$(cat "$PIDFILE")" "$ADDRESS" "$SIZE" \
		> "$RESULT_DIR/final-at-relocated-address.json"
	FINAL_PRESENT=$(json_value "$RESULT_DIR/final-at-relocated-address.json" present_pages)
	FINAL_PARENT=$(json_value "$RESULT_DIR/final-at-relocated-address.json" parent_pages)
	FINAL_UNCOVERED=$(json_value "$RESULT_DIR/final-at-relocated-address.json" uncovered_pages)
	if [ "$FINAL_PRESENT" -eq $((SIZE / PAGE_SIZE)) ] && [ "$FINAL_PARENT" -eq 0 ] && [ "$FINAL_UNCOVERED" -eq 0 ]; then
		restore_and_verify "$TARGET_FINAL" "$BASE" || fail "safe copied final image did not restore"
		cat > "$RESULT_DIR/summary.json" <<EOF_JSON
{"route":"$ROUTE","status":"PASS","outcome":"safe-copy","softdirty_pages":$SOFTDIRTY,"parent_covered_pages":$PARENT_COVERED,"final_present_pages":$FINAL_PRESENT,"final_parent_pages":$FINAL_PARENT}
EOF_JSON
		echo "RELOCATION-REGRESSION PASS route=$ROUTE outcome=safe-copy"
		exit 0
	fi

	RESTORE_RESULT="failed"
	if restore_and_verify "$TARGET_FINAL" "$BASE"; then
		RESTORE_RESULT="unexpected-success"
	fi
	cat > "$RESULT_DIR/summary.json" <<EOF_JSON
{"route":"$ROUTE","status":"FAIL","outcome":"unsafe-parent-accepted","softdirty_pages":$SOFTDIRTY,"parent_covered_pages":$PARENT_COVERED,"final_present_pages":$FINAL_PRESENT,"final_parent_pages":$FINAL_PARENT,"final_uncovered_pages":$FINAL_UNCOVERED,"restore":"$RESTORE_RESULT"}
EOF_JSON
	fail "final dump accepted parent references for a relocated range absent from the remote parent"
fi

kill -0 "$PID" 2>/dev/null || fail "failed final dump killed the workload"
if ! grep -Eiq 'not found in parent|Missing .*parent pagemap|No parent image found' \
	"$SOURCE_FINAL/dump.log" "$TARGET_FINAL/page-server.log" 2>/dev/null; then
	fail "parent-backed final dump failed for an unrelated reason"
fi

run_full_final "$SOURCE_RETRY" "$TARGET_RETRY" || fail "full retry after safe rejection failed"
reap_source
restore_and_verify "$TARGET_RETRY" "$BASE" || fail "full retry did not restore relocated data"
RETRY_PAYLOAD=$(sum_pages "$TARGET_RETRY")
cat > "$RESULT_DIR/summary.json" <<EOF_JSON
{"route":"$ROUTE","status":"PASS","outcome":"safe-reject-and-full-retry","softdirty_pages":$SOFTDIRTY,"parent_covered_pages":$PARENT_COVERED,"retry_payload_bytes":$RETRY_PAYLOAD}
EOF_JSON
echo "RELOCATION-REGRESSION PASS route=$ROUTE outcome=safe-reject-and-full-retry"
