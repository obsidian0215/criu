#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
QUERY_FINAL="$SCRIPT_DIR/query-final-dump.sh"
PROBE="$SCRIPT_DIR/pagemap_probe.py"
WORK_ROOT="$SCRIPT_DIR/query-contract-work"
RESULT_DIR="$SCRIPT_DIR/results/query-parent-contract"
WORKLOAD="$WORK_ROOT/parent-identity-workload"
RESULTS="$RESULT_DIR/results.tsv"
PID=""
PAGE_SERVER_PID=""
SERVER_PORT=""
STATE_FILE=""
EXPECTED_FILE=""
ORACLE_RESULT=""

[ "$ROUTE" = local-no-parent ] || {
	echo "query parent contract test requires the local-no-parent route" >&2
	exit 2
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

fail() {
	echo "FAIL: $*" >&2
	find "$WORK_ROOT" -type f \( -name '*.log' -o -name '*.state' -o -name '*.hex' \) -print -exec sh -c '
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
	wait_listen "$PAGE_SERVER_PID" "$SERVER_PORT" || return 1
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

start_workload() {
	local base=$1
	STATE_FILE="$base/workload.state"
	EXPECTED_FILE="$base/expected.hex"
	printf '31\n' >"$EXPECTED_FILE"
	"$WORKLOAD" "$STATE_FILE" "$EXPECTED_FILE" >"$base/workload.log" 2>&1 &
	PID=$!
	wait_phase READY || fail "workload did not become ready"
	[ "$(field pid)" = "$PID" ] || fail "workload pid mismatch"
}

remote_predump() {
	local source=$1 target=$2 source_parent=${3:-} target_parent=${4:-}
	local args=(--track-mem --page-server --address 127.0.0.1)
	mkdir -p "$source" "$target"
	start_server "$target" "$target_parent" || fail "page server did not start"
	args+=(--port "$SERVER_PORT")
	[ -z "$source_parent" ] || args+=(--prev-images-dir "../$(basename "$source_parent")")
	"${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log \
		-t "$PID" -v4 "${args[@]}" || fail "remote pre-dump failed"
	finish_server || fail "pre-dump page server failed"
}

probe() {
	local directory=$1 image_id=$2 address=$3 expected=${4:-}
	local args=("$directory" "$image_id" "$address")
	[ -z "$expected" ] || args+=("$expected")
	PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" python3 "$PROBE" "${args[@]}"
}

assemble_with_parent() {
	local source=$1 target=$2 parent=$3
	mkdir -p "$target"
	cp -a "$source/." "$target/"
	rm -f "$target/parent"
	ln -s "../$(basename "$parent")" "$target/parent"
}

assemble_standalone() {
	local source=$1 target=$2
	mkdir -p "$target"
	cp -a "$source/." "$target/"
	rm -f "$target/parent"
}

restore_and_check() {
	local target=$1 root_pid=$2
	ORACLE_RESULT=""
	if ! "${CRIU_CMD[@]}" restore -D "$target" -o restore.log -v4 -d; then
		return 10
	fi
	PID=$root_pid
	kill -0 "$PID" 2>/dev/null || return 11
	kill -USR2 "$PID" || return 12
	if wait_phase PASS; then
		ORACLE_RESULT=PASS
		PID=""
		return 0
	fi
	if grep -q '^FAIL ' "$STATE_FILE" 2>/dev/null; then
		ORACLE_RESULT=FAIL
		PID=""
		return 20
	fi
	return 13
}

record() {
	printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >>"$RESULTS"
	printf 'QUERY-PARENT-CONTRACT case=%s outcome=%s class=%s detail=%s\n' "$1" "$2" "$3" "$4"
}

run_parent_loss() {
	local base="$WORK_ROOT/parent-loss"
	local source_pre="$base/source-pre" target_pre="$base/target-pre"
	local source_final="$base/source-final" target_final="$base/target-final"
	local query_dir="$base/query" lost="$base/lost"
	local root_pid tracked final_class rc
	mkdir -p "$base" "$lost"
	start_workload "$base"
	root_pid=$PID
	tracked=$(field tracked)
	remote_predump "$source_pre" "$target_pre"
	shopt -s nullglob
	local images=("$target_pre"/pagemap-*.img "$target_pre"/pages-*.img)
	((${#images[@]})) || fail "remote parent has no memory images"
	mv "${images[@]}" "$lost/"
	shopt -u nullglob
	bash "$QUERY_FINAL" "$PID" "$source_final" "$source_pre" "$query_dir" "$target_pre" ||
		fail "final dump did not handle a missing remote parent"
	PID=""
	final_class=$(probe "$source_final" "$root_pid" "$tracked") || fail "unable to classify final image"
	[ "$final_class" = present ] || fail "missing parent did not produce a self-contained final image"
	assemble_standalone "$source_final" "$target_final"
	rm -rf "$target_pre" "$lost"
	set +e
	restore_and_check "$target_final" "$root_pid"
	rc=$?
	set -e
	[ "$rc" -eq 0 ] && [ "$ORACLE_RESULT" = PASS ] || fail "self-contained parent-loss restore failed"
	record parent-loss SAFE_FULL "$final_class" "final dump became self-contained"
	cleanup
}

run_generation_skew() {
	local base="$WORK_ROOT/generation-skew"
	local source_pre1="$base/source-pre1" source_pre2="$base/source-pre2"
	local target_pre1="$base/target-pre1" target_pre2="$base/target-pre2"
	local source_final="$base/source-final" target_final="$base/target-final"
	local query_dir="$base/query"
	local root_pid tracked final_class rc
	mkdir -p "$base"
	start_workload "$base"
	root_pid=$PID
	tracked=$(field tracked)
	remote_predump "$source_pre1" "$target_pre1"
	kill -USR1 "$PID" || fail "unable to create a second generation"
	wait_phase GEN2 || fail "workload did not reach generation two"
	printf '62\n' >"$EXPECTED_FILE"
	remote_predump "$source_pre2" "$target_pre2" "$source_pre1" "$target_pre1"
	probe "$target_pre2" "$root_pid" "$tracked" present >/dev/null || fail "generation two is absent"
	bash "$QUERY_FINAL" "$PID" "$source_final" "$source_pre2" "$query_dir" "$target_pre1" ||
		fail "range-compatible generation skew was rejected"
	PID=""
	final_class=$(probe "$source_final" "$root_pid" "$tracked") || fail "unable to classify skewed final image"
	assemble_with_parent "$source_final" "$target_final" "$target_pre1"
	set +e
	restore_and_check "$target_final" "$root_pid"
	rc=$?
	set -e
	case "$rc:$ORACLE_RESULT:$final_class" in
		20:FAIL:parent)
			record generation-skew UNBOUND_PARENT_ACCEPTED "$final_class" "range validation does not identify the intended generation"
			;;
		0:PASS:present)
			record generation-skew SAFE_FULL "$final_class" "final dump did not depend on the mismatched generation"
			;;
		0:PASS:*)
			record generation-skew RESTORED_CORRECTLY "$final_class" "restore remained correct"
			;;
		*)
			fail "unexpected generation-skew result rc=$rc oracle=$ORACLE_RESULT class=$final_class"
			;;
	esac
	cleanup
}

rm -rf "$WORK_ROOT" "$RESULT_DIR"
mkdir -p "$WORK_ROOT" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/parent_identity_workload.c" -o "$WORKLOAD"
printf 'case\toutcome\tfinal_class\tdetail\n' >"$RESULTS"
run_parent_loss
run_generation_skew
python3 - "$RESULTS" "$RESULT_DIR/summary.json" <<'PY'
import csv
import json
import sys
source, target = sys.argv[1:]
with open(source) as stream:
    rows = list(csv.DictReader(stream, delimiter='\t'))
json.dump({'route': 'local-no-parent', 'status': 'CHARACTERIZED', 'cases': rows}, open(target, 'w'), indent=2)
open(target, 'a').write('\n')
PY
printf 'QUERY-PARENT-CONTRACT COMPLETE\n'
rm -rf "$WORK_ROOT"
