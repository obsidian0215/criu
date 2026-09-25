#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
ZDTM_DIR="$TOP/test/zdtm/static"
WORK_ROOT="$SCRIPT_DIR/lifecycle-work-$ROUTE"
RESULT_DIR="$SCRIPT_DIR/results/lifecycle"
BASE_PAGE_LAUNCHER="$WORK_ROOT/base-page-launcher"
PAGE_SERVER_PID=""
SERVER_PORT=""
PID=""

case "$ROUTE" in
	final-page-server|local-no-parent|local-pagemap|custom-coverage) ;;
	*) echo "invalid route: $ROUTE" >&2; exit 2 ;;
esac

cleanup_case() {
	if [ -n "$PAGE_SERVER_PID" ] && kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
		wait "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	PAGE_SERVER_PID=""
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		kill -KILL "$PID" 2>/dev/null || true
	fi
	PID=""
}
trap cleanup_case EXIT

free_port() {
	python3 - <<'PY'
import socket
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    print(sock.getsockname()[1])
PY
}

wait_server() {
	local port=$1
	for _ in $(seq 1 150); do
		kill -0 "$PAGE_SERVER_PID" 2>/dev/null || return 1
		ss -H -ltn "sport = :$port" | grep -q . && return 0
		sleep 0.02
	done
	return 1
}

start_server() {
	local directory=$1 parent=${2:-} args=()
	SERVER_PORT=$(free_port)
	[ -z "$parent" ] || args+=(--prev-images-dir "../$(basename "$parent")")
	"${CRIU_CMD[@]}" page-server -D "$directory" -o page-server.log -v4 --port "$SERVER_PORT" "${args[@]}" \
		>"$directory/server-command.log" 2>&1 &
	PAGE_SERVER_PID=$!
	wait_server "$SERVER_PORT" || return 1
}

remote_round() {
	local operation=$1 source=$2 target=$3 source_parent=${4:-} target_parent=${5:-}
	local args=(--track-mem --page-server --address 127.0.0.1)
	mkdir -p "$source" "$target"
	start_server "$target" "$target_parent" || return 1
	args+=(--port "$SERVER_PORT")
	[ -z "$source_parent" ] || args+=(--prev-images-dir "../$(basename "$source_parent")")
	if [ "$operation" = pre-dump ]; then
		"${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log -t "$PID" -v4 "${args[@]}" || return 1
	else
		"${CRIU_CMD[@]}" dump -D "$source" -o dump.log -t "$PID" -v4 "${args[@]}" || return 1
	fi
	wait "$PAGE_SERVER_PID" || return 1
	PAGE_SERVER_PID=""
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

record() {
	printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$RESULT_DIR/results.tsv"
}

start_workload() {
	local test=$1
	if [ "$test" = shm ]; then
		unshare --ipc --fork "$BASE_PAGE_LAUNCHER" make "$test.pid"
	else
		"$BASE_PAGE_LAUNCHER" make "$test.pid"
	fi
}

run_test() {
	local test=$1
	local base="$WORK_ROOT/$test"
	local source1="$base/source1"
	local source2="$base/source2"
	local sourcef="$base/source-final"
	local target1="$base/target1"
	local target2="$base/target2"
	local targetf="$base/target-final"
	cleanup_case
	rm -rf "$base"
	mkdir -p "$base"
	make -C "$ZDTM_DIR" "$test.cleanout" >/dev/null 2>&1 || true
	if ! make -C "$ZDTM_DIR" "$test" >"$base/build.log" 2>&1; then
		record "$test" SKIP "workload unavailable"
		return
	fi
	if ! (cd "$ZDTM_DIR" && start_workload "$test") >"$base/start.log" 2>&1; then
		if [ "$test" = shm ]; then
			record "$test" FAIL "IPC namespace workload start failed"
			return 1
		fi
		record "$test" SKIP "workload start unsupported"
		return
	fi
	PID=$(cat "$ZDTM_DIR/$test.pid")
	if ! kill -0 "$PID" 2>/dev/null; then
		record "$test" SKIP "workload exited during startup"
		PID=""
		return
	fi
	if [ "$test" = shm ] && [ "$(readlink "/proc/$PID/ns/ipc")" = "$(readlink /proc/self/ns/ipc)" ]; then
		record "$test" FAIL "workload did not enter an isolated IPC namespace"
		return 1
	fi
	remote_round pre-dump "$source1" "$target1" || { record "$test" FAIL "first remote pre-dump"; return 1; }
	remote_round pre-dump "$source2" "$target2" "$source1" "$target1" || { record "$test" FAIL "second remote pre-dump"; return 1; }
	if [ "$ROUTE" = final-page-server ]; then
		remote_round dump "$sourcef" "$targetf" "$source2" "$target2" || { record "$test" FAIL "remote final dump"; return 1; }
		copy_non_memory "$sourcef" "$targetf"
	else
		mkdir -p "$sourcef" "$targetf"
		"${CRIU_CMD[@]}" dump -D "$sourcef" -o dump.log -t "$PID" -v4 --track-mem \
			--prev-images-dir ../source2 || { record "$test" FAIL "local final dump"; return 1; }
		cp -a "$sourcef/." "$targetf/"
		rm -f "$targetf/parent"
		ln -s ../target2 "$targetf/parent"
	fi
	PID=""
	"${CRIU_CMD[@]}" restore -D "$targetf" -o restore.log -v4 -d || { record "$test" FAIL "restore"; return 1; }
	PID=$(cat "$ZDTM_DIR/$test.pid")
	if ! (cd "$ZDTM_DIR" && make "$test.stop" && grep PASS "$test.out") >"$base/stop.log" 2>&1; then
		record "$test" FAIL "post-restore oracle"
		return 1
	fi
	PID=""
	record "$test" PASS "remote chain restored"
}

rm -rf "$WORK_ROOT" "$RESULT_DIR"
mkdir -p "$WORK_ROOT" "$RESULT_DIR"
"${CC:-cc}" -Wall -Wextra -Werror "$SCRIPT_DIR/base_page_launcher.c" -o "$BASE_PAGE_LAUNCHER"
printf 'test\tstatus\tdetail\n' > "$RESULT_DIR/results.tsv"

for test in cow00 maps00 shm vfork00; do
	run_test "$test"
done
record pid-reuse SKIP "deterministic PID reuse not established in this runner"

python3 - "$ROUTE" "$RESULT_DIR/results.tsv" "$RESULT_DIR/summary.json" <<'PY'
import csv, json, sys
route, source, target = sys.argv[1:]
with open(source) as f:
    rows = list(csv.DictReader(f, delimiter='\t'))
status = 'FAIL' if any(r['status'] == 'FAIL' for r in rows) else 'PASS'
json.dump({'route': route, 'status': status, 'cases': rows}, open(target, 'w'), indent=2)
open(target, 'a').write('\n')
if status == 'FAIL':
    raise SystemExit(1)
PY
printf 'LIFECYCLE-REGRESSION PASS route=%s\n' "$ROUTE"
rm -rf "$WORK_ROOT"
