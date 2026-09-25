#!/bin/bash
set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
WORK_ROOT="$SCRIPT_DIR/contract-work-$ROUTE"
RESULT_DIR="$SCRIPT_DIR/results/parent-contract"
WORKLOAD="$WORK_ROOT/parent-identity-workload"
PROBE="$SCRIPT_DIR/pagemap_probe.py"
RESULTS="$RESULT_DIR/results.tsv"
PID=""
PAGE_SERVER_PID=""
STATE_FILE=""
SERVER_PORT=""
FINAL_IMAGE=""
ORACLE_RESULT=""

case "$ROUTE" in
	final-page-server|local-no-parent|local-pagemap|custom-coverage) ;;
	*) echo "invalid route: $ROUTE" >&2; exit 2 ;;
esac

cleanup_processes() {
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
trap cleanup_processes EXIT

failure_logs() {
	local base=${1:-$WORK_ROOT}
	find "$base" -type f \( -name '*.log' -o -name '*.state' \) -print -exec sh -c '
		for file do
			echo "===== $file ====="
			tail -n 120 "$file"
		done
	' sh {} + 2>/dev/null || true
}

fail() {
	echo "HARNESS-FAIL: $*" >&2
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

wait_oracle() {
	local phase
	for _ in $(seq 1 200); do
		phase=$(awk 'NR == 1 { print $1 }' "$STATE_FILE" 2>/dev/null || true)
		case "$phase" in
			PASS|FAIL) ORACLE_RESULT=$phase; return 0 ;;
		esac
		[ -z "$PID" ] || kill -0 "$PID" 2>/dev/null || true
		sleep 0.05
	done
	return 1
}

start_workload() {
	local base=$1
	STATE_FILE="$base/workload.state"
	"$WORKLOAD" "$STATE_FILE" >"$base/workload.log" 2>&1 &
	PID=$!
	wait_phase READY || fail "workload did not become ready in $base"
	[ "$(field pid)" = "$PID" ] || fail "workload PID mismatch in $base"
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

remote_predump() {
	local source=$1 target=$2 source_parent=${3:-} target_parent=${4:-}
	local args=(--track-mem --page-server --address 127.0.0.1)
	mkdir -p "$source" "$target"
	start_server "$target" "$target_parent" || {
		stop_server
		fail "pre-dump page server did not start"
	}
	args+=(--port "$SERVER_PORT")
	[ -z "$source_parent" ] || args+=(--prev-images-dir "../$(basename "$source_parent")")
	if ! "${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log \
		-t "$PID" -v4 "${args[@]}"; then
		stop_server
		fail "remote pre-dump failed"
	fi
	finish_server || fail "pre-dump page server failed"
}

final_dump() {
	local source=$1 target=$2 source_parent=$3 target_parent=$4
	local source_status=0 server_status=0
	FINAL_IMAGE=""
	mkdir -p "$source" "$target"
	if [ "$ROUTE" = final-page-server ]; then
		if ! start_server "$target" "$target_parent"; then
			stop_server
			return 1
		fi
		"${CRIU_CMD[@]}" dump -D "$source" -o dump.log -t "$PID" -v4 --track-mem \
			--page-server --address 127.0.0.1 --port "$SERVER_PORT" \
			--prev-images-dir "../$(basename "$source_parent")" || source_status=$?
		if [ "$source_status" -eq 0 ]; then
			finish_server || server_status=$?
		else
			stop_server
		fi
		if [ "$source_status" -ne 0 ] || [ "$server_status" -ne 0 ]; then
			return 1
		fi
		copy_non_memory "$source" "$target"
		FINAL_IMAGE="$target"
	else
		if ! "${CRIU_CMD[@]}" dump -D "$source" -o dump.log -t "$PID" -v4 --track-mem \
			--prev-images-dir "../$(basename "$source_parent")"; then
			return 1
		fi
		assemble_local_final "$source" "$target" "$target_parent"
		FINAL_IMAGE="$source"
	fi
	PID=""
	return 0
}

probe_tracked() {
	local directory=$1 image_id=$2 address=$3
	PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" \
		python3 "$PROBE" "$directory" "$image_id" "$address"
}

restore_and_check() {
	local target=$1 root_pid=$2
	ORACLE_RESULT=""
	if ! "${CRIU_CMD[@]}" restore -D "$target" -o restore.log -v4 -d; then
		return 10
	fi
	PID=$root_pid
	kill -0 "$PID" 2>/dev/null || fail "restore reported success but task $PID is missing"
	kill -USR2 "$PID" || fail "unable to trigger restored workload oracle"
	wait_oracle || fail "restored workload oracle did not finish"
	if [ "$ORACLE_RESULT" = PASS ]; then
		PID=""
		return 0
	fi
	PID=""
	return 20
}

record() {
	local name=$1 outcome=$2 final_class=$3 detail=$4
	printf '%s\t%s\t%s\t%s\n' "$name" "$outcome" "$final_class" "$detail" >> "$RESULTS"
	printf 'PARENT-CONTRACT route=%s case=%s outcome=%s class=%s detail=%s\n' \
		"$ROUTE" "$name" "$outcome" "$final_class" "$detail"
}

run_parent_loss() {
	local base="$WORK_ROOT/parent-loss"
	local source_pre="$base/source-pre"
	local target_pre="$base/target-pre"
	local source_final="$base/source-final"
	local target_final="$base/target-final"
	local lost="$base/lost-parent"
	local root_pid tracked final_class="not-produced" rc
	mkdir -p "$base" "$lost"
	start_workload "$base"
	root_pid=$PID
	tracked=$(field tracked)
	remote_predump "$source_pre" "$target_pre"
	probe_tracked "$target_pre" "$root_pid" "$tracked" present >/dev/null ||
		fail "pre-dump did not capture tracked page"
	shopt -s nullglob
	local images=("$target_pre"/pagemap-*.img "$target_pre"/pages-*.img)
	((${#images[@]})) || fail "target parent has no memory images to remove"
	mv "${images[@]}" "$lost/"
	shopt -u nullglob
	if ! final_dump "$source_final" "$target_final" "$source_pre" "$target_pre"; then
		if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
			record parent-loss SAFE_REJECT "$final_class" "final dump rejected an unavailable destination parent"
			cleanup_processes
			return
		fi
		fail "parent-loss final dump failed without resuming workload"
	fi
	final_class=$(probe_tracked "$FINAL_IMAGE" "$root_pid" "$tracked") ||
		fail "unable to classify parent-loss final image"
	restore_and_check "$target_final" "$root_pid"
	rc=$?
	case "$rc:$ORACLE_RESULT:$final_class" in
		0:PASS:present)
			record parent-loss SAFE_FULL "$final_class" "final dump became self-contained after destination parent loss"
			;;
		0:PASS:*)
			record parent-loss RESTORED_WITH_PARENT "$final_class" "restore succeeded despite an unavailable parent; inspect image references"
			;;
		10::*|10:*:*)
			record parent-loss DEFERRED_FAILURE "$final_class" "final dump succeeded but missing destination parent was detected only at restore"
			;;
		20:FAIL:*)
			record parent-loss UNSAFE_ACCEPT "$final_class" "restore succeeded but application memory was incorrect"
			;;
		*)
			fail "unexpected parent-loss result rc=$rc oracle=$ORACLE_RESULT class=$final_class"
			;;
	esac
	cleanup_processes
}

run_generation_skew() {
	local base="$WORK_ROOT/generation-skew"
	local source_pre1="$base/source-pre1"
	local source_pre2="$base/source-pre2"
	local target_pre1="$base/target-pre1"
	local target_pre2="$base/target-pre2"
	local source_final="$base/source-final"
	local target_final="$base/target-final"
	local root_pid tracked final_class="not-produced" rc
	mkdir -p "$base"
	start_workload "$base"
	root_pid=$PID
	tracked=$(field tracked)
	remote_predump "$source_pre1" "$target_pre1"
	kill -USR1 "$PID" || fail "unable to create second memory generation"
	wait_phase GEN2 || fail "workload did not create second generation"
	remote_predump "$source_pre2" "$target_pre2" "$source_pre1" "$target_pre1"
	probe_tracked "$target_pre2" "$root_pid" "$tracked" present >/dev/null ||
		fail "second pre-dump did not capture changed tracked page"

	# Deliberately pair the source's second-generation parent metadata with
	# the destination's first-generation image. Address coverage is identical,
	# but the payload generation is not.
	if ! final_dump "$source_final" "$target_final" "$source_pre2" "$target_pre1"; then
		if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
			record generation-skew SAFE_REJECT "$final_class" "final dump rejected a mismatched destination parent generation"
			cleanup_processes
			return
		fi
		fail "generation-skew final dump failed without resuming workload"
	fi
	final_class=$(probe_tracked "$FINAL_IMAGE" "$root_pid" "$tracked") ||
		fail "unable to classify generation-skew final image"
	restore_and_check "$target_final" "$root_pid"
	rc=$?
	case "$rc:$ORACLE_RESULT:$final_class" in
		0:PASS:present)
			record generation-skew SAFE_FULL "$final_class" "final dump did not depend on the mismatched parent generation"
			;;
		0:PASS:*)
			record generation-skew RESTORED_CORRECTLY "$final_class" "restore remained correct despite the intentional parent mismatch"
			;;
		10::*|10:*:*)
			record generation-skew DETECTED_AT_RESTORE "$final_class" "parent generation mismatch was detected during restore"
			;;
		20:FAIL:*)
			record generation-skew UNBOUND_PARENT_ACCEPTED "$final_class" "restore accepted same-range data from the wrong parent generation"
			;;
		*)
			fail "unexpected generation-skew result rc=$rc oracle=$ORACLE_RESULT class=$final_class"
			;;
	esac
	cleanup_processes
}

rm -rf "$WORK_ROOT" "$RESULT_DIR"
mkdir -p "$WORK_ROOT" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/parent_identity_workload.c" -o "$WORKLOAD" ||
	fail "identity workload compilation failed"
printf 'case\toutcome\tfinal_class\tdetail\n' > "$RESULTS"

run_parent_loss
run_generation_skew

python3 - "$ROUTE" "$RESULTS" "$RESULT_DIR/summary.json" <<'PY'
import csv
import json
import sys
route, source, target = sys.argv[1:]
with open(source) as stream:
    rows = list(csv.DictReader(stream, delimiter='\t'))
json.dump({'route': route, 'status': 'PASS', 'cases': rows}, open(target, 'w'), indent=2)
open(target, 'a').write('\n')
PY
printf 'PARENT-CONTRACT PASS route=%s\n' "$ROUTE"
rm -rf "$WORK_ROOT"
