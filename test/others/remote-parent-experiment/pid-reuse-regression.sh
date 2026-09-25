#!/bin/bash
set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
WORK_ROOT="$SCRIPT_DIR/pid-reuse-work-$ROUTE"
RESULT_DIR="$SCRIPT_DIR/results/pid-reuse"
WORKLOAD="$WORK_ROOT/parent-identity-workload"
STATE_FILE="$WORK_ROOT/workload.state"
EXPECTED_FILE="$WORK_ROOT/expected.hex"
PROBE="$SCRIPT_DIR/pagemap_probe.py"
PID=""
PAGE_SERVER_PID=""
SERVER_PORT=""
FINAL_IMAGE=""
DESIRED_PID=200

case "$ROUTE" in
	final-page-server|local-no-parent|local-pagemap|custom-coverage) ;;
	*) echo "invalid route: $ROUTE" >&2; exit 2 ;;
esac

write_summary() {
	local status=$1 detail=$2 final_class=${3:-not-produced}
	mkdir -p "$RESULT_DIR"
	python3 - "$ROUTE" "$status" "$detail" "$final_class" "$RESULT_DIR/summary.json" <<'PY'
import json
import sys
route, status, detail, final_class, target = sys.argv[1:]
json.dump({
    'route': route,
    'case': 'deterministic-pid-reuse',
    'status': status,
    'detail': detail,
    'final_tracked_class': final_class,
}, open(target, 'w'), indent=2)
open(target, 'a').write('\n')
PY
	printf '%s\t%s\t%s\t%s\n' "$ROUTE" "$status" "$final_class" "$detail" > "$RESULT_DIR/result.tsv"
}

skip() {
	write_summary SKIP "$1"
	printf 'PID-REUSE SKIP route=%s reason=%s\n' "$ROUTE" "$1"
	exit 77
}

fail() {
	write_summary FAIL "$1"
	echo "FAIL: $*" >&2
	find "$WORK_ROOT" -type f \( -name '*.log' -o -name '*.state' -o -name '*.hex' \) -print -exec sh -c '
		for file do
			echo "===== $file =====" >&2
			tail -n 160 "$file" >&2
		done
	' sh {} + 2>/dev/null || true
	exit 1
}

cleanup() {
	if [ -n "$PAGE_SERVER_PID" ] && kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
		wait "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	PAGE_SERVER_PID=""
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		kill -KILL "$PID" 2>/dev/null || true
		wait "$PID" 2>/dev/null || true
	fi
	PID=""
}
trap cleanup EXIT

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
	wait_listen "$PAGE_SERVER_PID" "$SERVER_PORT"
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

set_next_pid() {
	local previous=$((DESIRED_PID - 1))
	printf '%s\n' "$previous" > /proc/sys/kernel/ns_last_pid 2>/dev/null || return 1
}

start_generation() {
	local expected=$1 mutate=${2:-0}
	rm -f "$STATE_FILE"
	printf '%s\n' "$expected" > "$EXPECTED_FILE"
	set_next_pid || skip "kernel does not permit deterministic ns_last_pid control"
	PARENT_IDENTITY_FIXED=1 "$WORKLOAD" "$STATE_FILE" "$EXPECTED_FILE" \
		>"$WORK_ROOT/workload-$expected.log" 2>&1 &
	PID=$!
	[ "$PID" -eq "$DESIRED_PID" ] || skip "requested pid $DESIRED_PID but kernel allocated $PID"
	wait_phase READY || fail "workload generation $expected did not become ready"
	[ "$(field pid)" = "$PID" ] || fail "workload generation $expected reported a different pid"
	if [ "$mutate" -eq 1 ]; then
		kill -USR1 "$PID" || fail "unable to mutate reused-pid workload"
		wait_phase GEN2 || fail "reused-pid workload did not reach generation two"
		printf '62\n' > "$EXPECTED_FILE"
	fi
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

probe() {
	local directory=$1 address=$2 expected=$3
	PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" \
		python3 "$PROBE" "$directory" "$DESIRED_PID" "$address" "$expected"
}

run_inner() {
	local source_pre="$WORK_ROOT/source-pre"
	local target_pre="$WORK_ROOT/target-pre"
	local reset_pre="$WORK_ROOT/reset-pre"
	local source_final="$WORK_ROOT/source-final"
	local target_final="$WORK_ROOT/target-final"
	local tracked control final_class phase

	[ "$$" -eq 1 ] || skip "PID namespace init is not the test driver"
	[ -w /proc/sys/kernel/ns_last_pid ] || skip "ns_last_pid is not writable in the test namespace"

	start_generation 31 0
	tracked=$(field tracked)
	control=$(field control)
	mkdir -p "$source_pre" "$target_pre"
	start_server "$target_pre" || {
		stop_server
		fail "pre-dump page server did not start"
	}
	if ! "${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source_pre" -o dump.log \
		-t "$PID" -v4 --track-mem --page-server --address 127.0.0.1 --port "$SERVER_PORT"; then
		stop_server
		fail "first-generation remote pre-dump failed"
	fi
	finish_server || fail "first-generation page server failed"
	probe "$target_pre" "$tracked" present >/dev/null || fail "first generation tracked page is absent"
	probe "$target_pre" "$control" present >/dev/null || fail "first generation control page is absent"

	kill -KILL "$PID" || fail "unable to terminate first process instance"
	wait "$PID" 2>/dev/null || true
	PID=""

	start_generation 31 1
	[ "$(field tracked)" = "$tracked" ] || fail "reused process did not map tracked page at the same address"
	[ "$(field control)" = "$control" ] || fail "reused process did not map control page at the same address"
	mkdir -p "$reset_pre"
	if ! "${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$reset_pre" -o dump.log \
		-t "$PID" -v4 --track-mem; then
		fail "throwaway pre-dump could not reset reused-task dirty tracking"
	fi
	kill -0 "$PID" 2>/dev/null || fail "throwaway pre-dump stopped the reused task"

	mkdir -p "$source_final" "$target_final"
	if [ "$ROUTE" = final-page-server ]; then
		start_server "$target_final" "$target_pre" || {
			stop_server
			fail "final page server did not start"
		}
		if ! "${CRIU_CMD[@]}" dump -D "$source_final" -o dump.log -t "$PID" -v4 --track-mem \
			--page-server --address 127.0.0.1 --port "$SERVER_PORT" \
			--prev-images-dir "../$(basename "$source_pre")"; then
			stop_server
			fail "final dump rejected the reused pid"
		fi
		PID=""
		finish_server || fail "final page server failed"
		copy_non_memory "$source_final" "$target_final"
		FINAL_IMAGE="$target_final"
	else
		if ! "${CRIU_CMD[@]}" dump -D "$source_final" -o dump.log -t "$PID" -v4 --track-mem \
			--prev-images-dir "../$(basename "$source_pre")"; then
			fail "local final dump rejected the reused pid"
		fi
		PID=""
		assemble_local_final "$source_final" "$target_final" "$target_pre"
		FINAL_IMAGE="$source_final"
	fi

	grep -Eq 'Pid reuse\*? detected for pid 200' "$source_final/dump.log" ||
		fail "final dump did not report pid reuse"
	probe "$FINAL_IMAGE" "$tracked" present >/dev/null || fail "reused-pid tracked page inherited old data"
	probe "$FINAL_IMAGE" "$control" present >/dev/null || fail "reused-pid control page inherited old data"
	final_class=$(PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" \
		python3 "$PROBE" "$FINAL_IMAGE" "$DESIRED_PID" "$tracked") ||
		fail "unable to classify final tracked page"

	if ! "${CRIU_CMD[@]}" restore -D "$target_final" -o restore.log -v4 -d; then
		fail "reused-pid restore failed"
	fi
	PID=$DESIRED_PID
	kill -0 "$PID" 2>/dev/null || fail "restored reused-pid task is missing"
	kill -USR2 "$PID" || fail "unable to trigger reused-pid oracle"
	wait_phase PASS || {
		phase=$(awk 'NR == 1 {print $1}' "$STATE_FILE" 2>/dev/null || true)
		fail "reused-pid oracle did not pass (phase=$phase)"
	}
	PID=""
	write_summary PASS "pid reuse forced a self-contained final memory image" "$final_class"
	printf 'PID-REUSE PASS route=%s final-class=%s\n' "$ROUTE" "$final_class"
	rm -rf "$WORK_ROOT"
}

if [ "${1:-}" = inner ]; then
	run_inner
	exit 0
fi

rm -rf "$WORK_ROOT" "$RESULT_DIR"
mkdir -p "$WORK_ROOT" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/parent_identity_workload.c" -o "$WORKLOAD" ||
	fail "identity workload compilation failed"

set +e
unshare --pid --fork --mount-proc bash "$0" inner
status=$?
set -e
case "$status" in
	0) exit 0 ;;
	77) exit 0 ;;
	*) exit "$status" ;;
esac
