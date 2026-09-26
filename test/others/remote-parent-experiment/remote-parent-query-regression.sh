#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
WORK_ROOT="$SCRIPT_DIR/query-work-$ROUTE"
RESULT_DIR="$SCRIPT_DIR/results/remote-parent-query"
WORKLOAD="$WORK_ROOT/parent-identity-workload"
PROBE="$SCRIPT_DIR/pagemap_probe.py"
PROXY="$SCRIPT_DIR/tcp_proxy.py"
RESULTS="$RESULT_DIR/results.tsv"
PID=""
PAGE_SERVER_PID=""
PROXY_PID=""
SERVER_PORT=""
STATE_FILE=""
EXPECTED_FILE=""
LAST_QUERY_UP=0
LAST_QUERY_DOWN=0

[ "$ROUTE" = local-no-parent ] || {
	echo "remote parent query test requires the local-no-parent route" >&2
	exit 2
}

cleanup_processes() {
	if [ -n "$PROXY_PID" ] && kill -0 "$PROXY_PID" 2>/dev/null; then
		kill -KILL "$PROXY_PID" 2>/dev/null || true
		wait "$PROXY_PID" 2>/dev/null || true
	fi
	PROXY_PID=""
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

fail() {
	echo "FAIL: $*" >&2
	find "$WORK_ROOT" -type f \( -name '*.log' -o -name '*.state' -o -name '*.json' \) -print -exec sh -c '
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

stop_server() {
	if [ -n "$PAGE_SERVER_PID" ] && kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	wait "$PAGE_SERVER_PID" 2>/dev/null || true
	PAGE_SERVER_PID=""
}

start_proxy() {
	local listen_port=$1 target_port=$2 stats=$3 ready=$4
	rm -f "$stats" "$ready"
	python3 "$PROXY" --listen-port "$listen_port" --target-port "$target_port" \
		--stats "$stats" --ready "$ready" >"${stats%.json}.log" 2>&1 &
	PROXY_PID=$!
	for _ in $(seq 1 100); do
		[ -s "$ready" ] && return 0
		kill -0 "$PROXY_PID" 2>/dev/null || break
		sleep 0.02
	done
	return 1
}

finish_proxy() {
	local stats=$1 status=0
	wait "$PROXY_PID" || status=$?
	PROXY_PID=""
	[ "$status" -eq 0 ] || return "$status"
	read -r LAST_QUERY_UP LAST_QUERY_DOWN < <(python3 - "$stats" <<'PY'
import json
import sys
value = json.load(open(sys.argv[1]))
print(value['source_to_server'], value['server_to_source'])
PY
)
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
	printf '31\n' > "$EXPECTED_FILE"
	"$WORKLOAD" "$STATE_FILE" "$EXPECTED_FILE" >"$base/workload.log" 2>&1 &
	PID=$!
	wait_phase READY || fail "workload did not become ready"
	[ "$(field pid)" = "$PID" ] || fail "workload PID mismatch"
}

remote_predump() {
	local source=$1 target=$2
	mkdir -p "$source" "$target"
	start_server "$target" || fail "pre-dump page server did not start"
	if ! "${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log \
		-t "$PID" -v4 --track-mem --page-server --address 127.0.0.1 --port "$SERVER_PORT"; then
		stop_server
		fail "remote pre-dump failed"
	fi
	finish_server || fail "pre-dump page server failed"
}

query_final() {
	local source=$1 source_parent=$2 query_dir=$3 target_parent=$4
	local target_port proxy_port stats ready source_status=0 server_status=0 proxy_status=0
	mkdir -p "$source" "$query_dir"
	start_server "$query_dir" "$target_parent" || fail "query page server did not start"
	target_port=$SERVER_PORT
	proxy_port=$(free_port)
	stats="$query_dir/query-wire.json"
	ready="$query_dir/query-proxy.ready"
	start_proxy "$proxy_port" "$target_port" "$stats" "$ready" || fail "query proxy did not start"
	"${CRIU_CMD[@]}" dump -D "$source" -o dump.log -t "$PID" -v4 --track-mem \
		--prev-images-dir "../$(basename "$source_parent")" \
		--page-server-parent --address 127.0.0.1 --port "$proxy_port" || source_status=$?
	if [ "$source_status" -eq 0 ]; then
		finish_server || server_status=$?
	else
		stop_server
	fi
	finish_proxy "$stats" || proxy_status=$?
	[ "$source_status" -eq 0 ] && [ "$server_status" -eq 0 ] && [ "$proxy_status" -eq 0 ]
}

probe() {
	local directory=$1 image_id=$2 address=$3 expected=$4
	PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" \
		python3 "$PROBE" "$directory" "$image_id" "$address" "$expected"
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
	"${CRIU_CMD[@]}" restore -D "$target" -o restore.log -v4 -d || return 1
	PID=$root_pid
	kill -0 "$PID" 2>/dev/null || return 1
	kill -USR2 "$PID" || return 1
	wait_phase PASS || return 1
	PID=""
}

assert_source_has_no_memory_parent() {
	local source=$1
	if compgen -G "$source/pagemap-*.img" >/dev/null ||
	   compgen -G "$source/pages-*.img" >/dev/null ||
	   compgen -G "$source/remote-parent-*.img" >/dev/null; then
		fail "source pre-dump retained parent memory images"
	fi
}

record() {
	printf '%s\tPASS\t%s\t%s\t%s\n' "$1" "$2" "$LAST_QUERY_UP" "$LAST_QUERY_DOWN" >> "$RESULTS"
}

run_available_parent() {
	local base="$WORK_ROOT/available-parent"
	local source_pre="$base/source-pre" target_pre="$base/target-pre"
	local source_final="$base/source-final" target_final="$base/target-final"
	local query_dir="$base/query" root_pid tracked control
	mkdir -p "$base"
	start_workload "$base"
	root_pid=$PID
	tracked=$(field tracked)
	control=$(field control)
	remote_predump "$source_pre" "$target_pre"
	assert_source_has_no_memory_parent "$source_pre"
	query_final "$source_final" "$source_pre" "$query_dir" "$target_pre" ||
		fail "local final dump could not validate the remote parent"
	PID=""
	[ "$LAST_QUERY_UP" -gt 0 ] && [ "$LAST_QUERY_DOWN" -gt 0 ] ||
		fail "local final dump did not exchange parent query traffic"
	probe "$source_final" "$root_pid" "$tracked" parent >/dev/null ||
		fail "unchanged tracked page was not emitted as a parent reference"
	probe "$source_final" "$root_pid" "$control" parent >/dev/null ||
		fail "unchanged control page was not emitted as a parent reference"
	assemble_with_parent "$source_final" "$target_final" "$target_pre"
	restore_and_check "$target_final" "$root_pid" || fail "remote-parent query restore failed"
	record available-parent "remote parent ranges validated"
}

run_missing_parent() {
	local base="$WORK_ROOT/missing-parent"
	local source_pre="$base/source-pre" target_pre="$base/target-pre"
	local source_final="$base/source-final" target_final="$base/target-final"
	local query_dir="$base/query" lost="$base/lost" root_pid tracked control
	mkdir -p "$base" "$lost"
	start_workload "$base"
	root_pid=$PID
	tracked=$(field tracked)
	control=$(field control)
	remote_predump "$source_pre" "$target_pre"
	assert_source_has_no_memory_parent "$source_pre"
	shopt -s nullglob
	local memory_images=("$target_pre"/pagemap-*.img "$target_pre"/pages-*.img)
	((${#memory_images[@]})) || fail "remote parent has no memory images"
	mv "${memory_images[@]}" "$lost/"
	shopt -u nullglob
	query_final "$source_final" "$source_pre" "$query_dir" "$target_pre" ||
		fail "local final dump did not fall back after remote parent loss"
	PID=""
	probe "$source_final" "$root_pid" "$tracked" present >/dev/null ||
		fail "missing parent did not produce a self-contained tracked page"
	probe "$source_final" "$root_pid" "$control" present >/dev/null ||
		fail "missing parent did not produce a self-contained control page"
	assemble_standalone "$source_final" "$target_final"
	rm -rf "$target_pre" "$lost"
	restore_and_check "$target_final" "$root_pid" || fail "self-contained fallback restore failed"
	record missing-parent "final dump became self-contained"
}

rm -rf "$WORK_ROOT" "$RESULT_DIR"
mkdir -p "$WORK_ROOT" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/parent_identity_workload.c" -o "$WORKLOAD"
printf 'case\tstatus\tdetail\tquery_up\tquery_down\n' > "$RESULTS"

run_available_parent
cleanup_processes
run_missing_parent
cleanup_processes

python3 - "$ROUTE" "$RESULTS" "$RESULT_DIR/summary.json" <<'PY'
import csv
import json
import sys
route, source, target = sys.argv[1:]
with open(source) as stream:
    cases = list(csv.DictReader(stream, delimiter='\t'))
json.dump({'route': route, 'status': 'PASS', 'cases': cases}, open(target, 'w'), indent=2)
open(target, 'a').write('\n')
PY
printf 'REMOTE-PARENT-QUERY PASS route=%s\n' "$ROUTE"
rm -rf "$WORK_ROOT"
