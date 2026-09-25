#!/bin/bash
set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
WORK_DIR="$SCRIPT_DIR/boundary-work-$ROUTE"
RESULT_DIR="$SCRIPT_DIR/results/parentless-boundary"
WORKLOAD="$WORK_DIR/parentless-workload"
STATE_FILE="$WORK_DIR/workload.state"
PROBE="$SCRIPT_DIR/pagemap_probe.py"
PID=""
PAGE_SERVER_PID=""
FINAL_CLASS="not-produced"

case "$ROUTE" in
	final-page-server|local-no-parent|local-pagemap|custom-coverage) ;;
	*) echo "invalid route: $ROUTE" >&2; exit 2 ;;
esac

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

fail_harness() {
	printf 'HARNESS-FAIL: %s\n' "$*" >&2
	write_summary HARNESS_FAIL "$*"
	exit 1
}

json_escape() {
	python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$1"
}

write_summary() {
	local status=$1 detail=$2
	local escaped
	mkdir -p "$RESULT_DIR"
	escaped=$(json_escape "$detail")
	cat > "$RESULT_DIR/summary.json" <<EOF_JSON
{
  "route": "$ROUTE",
  "case": "read-protection-transition",
  "status": "$status",
  "detail": $escaped,
  "final_hidden_class": "$FINAL_CLASS"
}
EOF_JSON
	printf '%s\t%s\t%s\t%s\n' "$ROUTE" "$status" "$FINAL_CLASS" "$detail" > "$RESULT_DIR/result.tsv"
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
	wait_listen "$PAGE_SERVER_PID" "$SERVER_PORT" || fail_harness "page server did not listen"
}

stop_server() {
	if [ -n "$PAGE_SERVER_PID" ] && kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	wait "$PAGE_SERVER_PID" 2>/dev/null || true
	PAGE_SERVER_PID=""
}

finish_server() {
	local status=0
	wait "$PAGE_SERVER_PID" || status=$?
	PAGE_SERVER_PID=""
	return "$status"
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

field() {
	local name=$1
	sed -n "s/.* $name=\\([^ ]*\\).*/\\1/p" "$STATE_FILE"
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
	local source=$1 target=$2 parent=$3
	mkdir -p "$target"
	cp -a "$source/." "$target/"
	rm -f "$target/parent"
	ln -s "../$(basename "$parent")" "$target/parent"
}

rm -rf "$WORK_DIR" "$RESULT_DIR"
mkdir -p "$WORK_DIR" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/parentless_workload.c" -o "$WORKLOAD" ||
	fail_harness "workload compilation failed"

"$WORKLOAD" "$STATE_FILE" >"$WORK_DIR/workload.log" 2>&1 &
PID=$!
wait_phase READY || fail_harness "workload did not become ready"
[ "$(field pid)" = "$PID" ] || fail_harness "workload pid file mismatch"
HIDDEN=$(field hidden)
STABLE=$(field stable)

SOURCE_PRE="$WORK_DIR/source-pre"
TARGET_PRE="$WORK_DIR/target-pre"
SOURCE_FINAL="$WORK_DIR/source-final"
TARGET_FINAL="$WORK_DIR/target-final"
mkdir -p "$SOURCE_PRE" "$TARGET_PRE" "$SOURCE_FINAL" "$TARGET_FINAL"

start_server "$TARGET_PRE"
if ! "${CRIU_CMD[@]}" pre-dump --pre-dump-mode read -D "$SOURCE_PRE" -o dump.log \
	-t "$PID" -v4 --track-mem --page-server --address 127.0.0.1 --port "$SERVER_PORT"; then
	stop_server
	fail_harness "read-mode remote pre-dump failed"
fi
finish_server || fail_harness "pre-dump page server failed"

PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" python3 "$PROBE" "$TARGET_PRE" "$PID" "$HIDDEN" absent ||
	fail_harness "hidden non-readable page unexpectedly exists in remote parent"
PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" python3 "$PROBE" "$TARGET_PRE" "$PID" "$STABLE" present ||
	fail_harness "readable control page is absent from remote parent"

kill -USR1 "$PID" || fail_harness "unable to request post-pre-dump mutation"
wait_phase MUTATED || fail_harness "workload did not finish post-pre-dump mutation"

FINAL_OK=0
case "$ROUTE" in
	final-page-server)
		start_server "$TARGET_FINAL" "$TARGET_PRE"
		if "${CRIU_CMD[@]}" dump -D "$SOURCE_FINAL" -o dump.log -t "$PID" -v4 --track-mem \
			--page-server --address 127.0.0.1 --port "$SERVER_PORT" \
			--prev-images-dir "../$(basename "$SOURCE_PRE")"; then
			if finish_server; then
				FINAL_OK=1
			else
				FINAL_OK=0
			fi
		else
			stop_server
		fi
		if [ "$FINAL_OK" -eq 1 ]; then
			copy_non_memory "$SOURCE_FINAL" "$TARGET_FINAL"
			FINAL_IMAGE="$TARGET_FINAL"
		fi
		;;
	*)
		"${CRIU_CMD[@]}" dump -D "$SOURCE_FINAL" -o dump.log -t "$PID" -v4 --track-mem \
			--prev-images-dir "../$(basename "$SOURCE_PRE")" && FINAL_OK=1
		if [ "$FINAL_OK" -eq 1 ]; then
			assemble_local_final "$SOURCE_FINAL" "$TARGET_FINAL" "$TARGET_PRE"
			FINAL_IMAGE="$SOURCE_FINAL"
		fi
		;;
esac

if [ "$FINAL_OK" -ne 1 ]; then
	if kill -0 "$PID" 2>/dev/null; then
		write_summary SAFE_REJECT "final dump rejected a parent hole absent from the remote pre-dump and resumed the workload"
		printf 'BOUNDARY SAFE_REJECT route=%s\n' "$ROUTE"
		exit 0
	fi
	write_summary HARNESS_FAIL "final dump failed and workload was not resumed"
	exit 1
fi
PID=""
FINAL_CLASS=$(PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" python3 "$PROBE" "$FINAL_IMAGE" "$(field pid)" "$HIDDEN") ||
	fail_harness "unable to classify hidden page in final image"

if ! "${CRIU_CMD[@]}" restore -D "$TARGET_FINAL" -o restore.log -v4 -d; then
	write_summary UNSAFE "final dump succeeded but restore could not resolve a parent range absent from the remote pre-dump"
	printf 'BOUNDARY UNSAFE route=%s final-class=%s restore=failed\n' "$ROUTE" "$FINAL_CLASS" >&2
	exit 1
fi
PID=$(field pid)
if ! kill -0 "$PID" 2>/dev/null; then
	write_summary UNSAFE "restore reported success but the root task is missing"
	exit 1
fi
kill -USR2 "$PID" || {
	write_summary UNSAFE "restored task could not be asked to verify memory"
	exit 1
}
if ! wait_phase PASS; then
	DETAIL=$(cat "$STATE_FILE" 2>/dev/null || true)
	write_summary UNSAFE "restored workload oracle failed: $DETAIL"
	exit 1
fi
PID=""
write_summary PASS "final dump and restore preserved skipped, dirty, replaced, new-VMA, and new-process memory"
printf 'BOUNDARY PASS route=%s final-class=%s\n' "$ROUTE" "$FINAL_CLASS"
rm -rf "$WORK_DIR"
