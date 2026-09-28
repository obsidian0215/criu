#!/bin/bash
set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
WORK_ROOT="$SCRIPT_DIR/contract-work-$ROUTE-post-final-swap"
RESULT_DIR="$SCRIPT_DIR/results/parent-contract-post-final"
WORKLOAD="$WORK_ROOT/parent-identity-workload"
PROBE="$SCRIPT_DIR/pagemap_probe.py"
STATE_FILE=""
EXPECTED_FILE=""
PID=""
PAGE_SERVER_PID=""
SERVER_PORT=""

if [ "$ROUTE" != local-pagemap ]; then
	printf 'POST-FINAL-PARENT-SWAP SKIP route=%s\n' "$ROUTE"
	exit 0
fi

cleanup_processes() {
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
trap cleanup_processes EXIT

failure_logs() {
	find "$WORK_ROOT" -type f \( -name '*.log' -o -name '*.state' -o -name '*.hex' \) -print -exec sh -c '
		for file do
			echo "===== $file ====="
			tail -n 160 "$file"
		done
	' sh {} + 2>/dev/null || true
}

fail() {
	echo "POST-FINAL-PARENT-SWAP FAIL: $*" >&2
	failure_logs >&2
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
	"${CRIU_CMD[@]}" page-server -D "$directory" -o page-server.log -v4 \
		--port "$SERVER_PORT" "${args[@]}" >"$directory/server-command.log" 2>&1 &
	PAGE_SERVER_PID=$!
	wait_listen "$PAGE_SERVER_PID" "$SERVER_PORT"
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

wait_oracle() {
	local phase
	for _ in $(seq 1 200); do
		phase=$(awk 'NR == 1 { print $1 }' "$STATE_FILE" 2>/dev/null || true)
		case "$phase" in
			PASS|FAIL) printf '%s\n' "$phase"; return 0 ;;
		esac
		sleep 0.05
	done
	return 1
}

start_workload() {
	local base=$1
	STATE_FILE="$base/workload.state"
	EXPECTED_FILE="$base/expected.hex"
	printf '31\n' > "$EXPECTED_FILE"
	"$WORKLOAD" "$STATE_FILE" "$EXPECTED_FILE" >"$base/workload.log" 2>&1 &
	PID=$!
	wait_phase READY || fail "workload did not become ready"
	[ "$(field pid)" = "$PID" ] || fail "workload PID mismatch"
}

remote_predump() {
	local source=$1 target=$2 source_parent=${3:-} target_parent=${4:-}
	local args=(--track-mem --page-server --address 127.0.0.1)
	mkdir -p "$source" "$target"
	start_server "$target" "$target_parent" || fail "page server did not start"
	args+=(--port "$SERVER_PORT")
	[ -z "$source_parent" ] || args+=(--prev-images-dir "../$(basename "$source_parent")")
	if ! "${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log \
		-t "$PID" -v4 "${args[@]}"; then
		fail "remote pre-dump failed"
	fi
	finish_server || fail "page server failed"
}

local_final_dump() {
	local source=$1 target=$2 source_parent=$3 target_parent=$4
	mkdir -p "$source" "$target"
	if ! "${CRIU_CMD[@]}" dump -D "$source" -o dump.log -t "$PID" -v4 --track-mem \
		--prev-images-dir "../$(basename "$source_parent")"; then
		fail "local final dump failed"
	fi
	PID=""
	cp -a "$source/." "$target/"
	rm -f "$target/parent"
	ln -s "../$(basename "$target_parent")" "$target/parent"
}

probe_tracked() {
	local directory=$1 image_id=$2 address=$3 expected=$4
	PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" \
		python3 "$PROBE" "$directory" "$image_id" "$address" "$expected"
}

pagemap_generation() {
	local directory=$1 image_id=$2 field_name=$3
	PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" \
		python3 - "$directory" "$image_id" "$field_name" <<'PY'
from pathlib import Path
import sys
import pycriu.images

directory, image_id, field = sys.argv[1:]
with (Path(directory) / f'pagemap-{image_id}.img').open('rb') as image:
    entries = pycriu.images.load(image)['entries']
if not entries:
    raise SystemExit('empty pagemap image')
value = entries[0].get(field)
if not value:
    raise SystemExit(f'missing {field}')
print(value)
PY
}

write_summary() {
	local old_generation=$1 new_generation=$2 child_parent=$3 final_class=$4
	mkdir -p "$RESULT_DIR"
	python3 - "$RESULT_DIR/summary.json" "$ROUTE" "$old_generation" \
		"$new_generation" "$child_parent" "$final_class" <<'PY'
import json
import sys

path, route, old_generation, new_generation, child_parent, final_class = sys.argv[1:]
with open(path, 'w') as output:
    json.dump({
        'route': route,
        'harness_status': 'PASS',
        'case': 'post-final-parent-swap',
        'outcome': 'DETECTED_AT_RESTORE',
        'old_parent_generation': old_generation,
        'committed_parent_generation': new_generation,
        'child_expected_parent_generation': child_parent,
        'final_page_class': final_class,
    }, output, indent=2)
    output.write('\n')
PY
}

rm -rf "$WORK_ROOT" "$RESULT_DIR"
mkdir -p "$WORK_ROOT"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/parent_identity_workload.c" \
	-o "$WORKLOAD" || fail "identity workload compilation failed"

SOURCE_PRE1="$WORK_ROOT/source-pre1"
SOURCE_PRE2="$WORK_ROOT/source-pre2"
SOURCE_FINAL="$WORK_ROOT/source-final"
TARGET_PRE1="$WORK_ROOT/target-pre1"
TARGET_PRE2="$WORK_ROOT/target-pre2"
TARGET_PRE2_CORRECT="$WORK_ROOT/target-pre2-correct"
TARGET_FINAL="$WORK_ROOT/target-final"

start_workload "$WORK_ROOT"
ROOT_PID=$PID
TRACKED=$(field tracked)

remote_predump "$SOURCE_PRE1" "$TARGET_PRE1"
probe_tracked "$TARGET_PRE1" "$ROOT_PID" "$TRACKED" present >/dev/null ||
	fail "first parent does not cover the tracked page"

kill -USR1 "$PID" || fail "unable to create the second generation"
wait_phase GEN2 || fail "workload did not create the second generation"
printf '62\n' > "$EXPECTED_FILE"

remote_predump "$SOURCE_PRE2" "$TARGET_PRE2" "$SOURCE_PRE1" "$TARGET_PRE1"
probe_tracked "$TARGET_PRE2" "$ROOT_PID" "$TRACKED" present >/dev/null ||
	fail "second parent does not cover the tracked page"

OLD_GENERATION=$(pagemap_generation "$TARGET_PRE1" "$ROOT_PID" dump_criu_run_id) ||
	fail "first parent has no generation id"
NEW_GENERATION=$(pagemap_generation "$TARGET_PRE2" "$ROOT_PID" dump_criu_run_id) ||
	fail "second parent has no generation id"
[ "$OLD_GENERATION" != "$NEW_GENERATION" ] ||
	fail "two distinct pre-dumps reused one generation id"

local_final_dump "$SOURCE_FINAL" "$TARGET_FINAL" "$SOURCE_PRE2" "$TARGET_PRE2"
FINAL_CLASS=$(PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" \
	python3 "$PROBE" "$SOURCE_FINAL" "$ROOT_PID" "$TRACKED") ||
	fail "unable to classify the final tracked page"
[ "$FINAL_CLASS" = parent ] ||
	fail "final image is not incremental for the tracked page ($FINAL_CLASS)"

CHILD_PARENT=$(pagemap_generation "$SOURCE_FINAL" "$ROOT_PID" parent_criu_run_id) ||
	fail "final image has no expected parent generation"
[ "$CHILD_PARENT" = "$NEW_GENERATION" ] ||
	fail "final image is bound to an unexpected parent generation"

mv "$TARGET_PRE2" "$TARGET_PRE2_CORRECT"
cp -a "$TARGET_PRE1" "$TARGET_PRE2"
SWAPPED_GENERATION=$(pagemap_generation "$TARGET_PRE2" "$ROOT_PID" dump_criu_run_id) ||
	fail "swapped parent has no generation id"
[ "$SWAPPED_GENERATION" = "$OLD_GENERATION" ] ||
	fail "parent swap did not install the old generation"
probe_tracked "$TARGET_PRE2" "$ROOT_PID" "$TRACKED" present >/dev/null ||
	fail "swapped parent no longer has matching address coverage"

RESTORE_STATUS=0
"${CRIU_CMD[@]}" restore -D "$TARGET_FINAL" -o restore.log -v4 -d || RESTORE_STATUS=$?
if [ "$RESTORE_STATUS" -eq 0 ]; then
	PID=$ROOT_PID
	if kill -0 "$PID" 2>/dev/null; then
		kill -USR2 "$PID" || true
		ORACLE=$(wait_oracle || printf 'NO_RESULT\n')
	else
		ORACLE=NO_PROCESS
	fi
	fail "restore accepted a parent replaced after final commit (oracle=$ORACLE)"
fi

grep -q 'Parent pagemap generation does not match the expected dump' \
	"$TARGET_FINAL/restore.log" ||
	fail "restore failed for a reason other than parent generation mismatch"

write_summary "$OLD_GENERATION" "$NEW_GENERATION" "$CHILD_PARENT" "$FINAL_CLASS"
printf 'POST-FINAL-PARENT-SWAP PASS route=%s outcome=DETECTED_AT_RESTORE\n' "$ROUTE"
rm -rf "$WORK_ROOT"
