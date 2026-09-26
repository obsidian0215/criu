#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
FAULT_PROXY="$SCRIPT_DIR/query_fault_proxy.py"
WORK_ROOT="$SCRIPT_DIR/query-disconnect-work"
RESULT_DIR="$SCRIPT_DIR/results/query-disconnect"
WORKLOAD="$WORK_ROOT/parent-identity-workload"
RESULTS="$RESULT_DIR/results.tsv"
PID=""
PAGE_SERVER_PID=""
PROXY_PID=""
SERVER_PORT=""
STATE_FILE=""
EXPECTED_FILE=""

cleanup() {
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
trap cleanup EXIT

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

wait_ready() {
	local pid=$1 ready=$2
	for _ in $(seq 1 150); do
		[ -s "$ready" ] && return 0
		kill -0 "$pid" 2>/dev/null || return 1
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
	local source=$1 target=$2
	mkdir -p "$source" "$target"
	start_server "$target" || fail "pre-dump page server did not start"
	"${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log \
		-t "$PID" -v4 --track-mem --page-server --address 127.0.0.1 --port "$SERVER_PORT" ||
		fail "remote pre-dump failed"
	finish_server || fail "pre-dump page server failed"
}

run_case() {
	local label=$1 server_bytes=$2
	local base="$WORK_ROOT/$label"
	local source_pre="$base/source-pre" target_pre="$base/target-pre"
	local source_final="$base/source-final" query_dir="$base/query"
	local server_port proxy_port ready stats status=0 up down
	cleanup
	rm -rf "$base"
	mkdir -p "$base" "$source_final" "$query_dir"
	start_workload "$base"
	remote_predump "$source_pre" "$target_pre"

	start_server "$query_dir" "$target_pre" || fail "$label query server did not start"
	server_port=$SERVER_PORT
	proxy_port=$(free_port)
	ready="$base/proxy.ready"
	stats="$base/proxy-stats.json"
	python3 "$FAULT_PROXY" --listen-port "$proxy_port" --target-port "$server_port" \
		--server-bytes "$server_bytes" --stats "$stats" --ready "$ready" \
		>"$base/proxy.log" 2>&1 &
	PROXY_PID=$!
	wait_ready "$PROXY_PID" "$ready" || fail "$label query proxy did not start"

	set +e
	"${CRIU_CMD[@]}" dump -D "$source_final" -o dump.log -t "$PID" -v4 --track-mem \
		--prev-images-dir "../$(basename "$source_pre")" \
		--page-server-parent --address 127.0.0.1 --port "$proxy_port"
	status=$?
	set -e
	[ "$status" -ne 0 ] || fail "$label query disconnect was accepted"

	wait "$PROXY_PID" || true
	PROXY_PID=""
	stop_server
	[ -s "$stats" ] || fail "$label proxy produced no traffic record"
	read -r up down < <(python3 - "$stats" <<'PY'
import json
import sys
value = json.load(open(sys.argv[1]))
print(value['source_to_server'], value['server_to_source'])
PY
)
	[ "$up" -gt 0 ] || fail "$label source sent no query traffic"
	[ "$down" -eq "$server_bytes" ] ||
		fail "$label forwarded $down server bytes, expected $server_bytes"
	[ ! -e "$source_final/inventory.img" ] || fail "$label left a successful inventory"
	kill -0 "$PID" 2>/dev/null || fail "$label failed dump did not resume the workload"
	kill -USR2 "$PID" || fail "$label could not run the workload oracle"
	wait_phase PASS || fail "$label damaged the workload"
	PID=""
	printf '%s\tPASS\t%s\t%s\n' "$label" "$up" "$down" >>"$RESULTS"
}

rm -rf "$WORK_ROOT" "$RESULT_DIR"
mkdir -p "$WORK_ROOT" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/parent_identity_workload.c" -o "$WORKLOAD"
printf 'case\tstatus\tsource_to_server\tserver_to_source\n' >"$RESULTS"
run_case before-parent-response 0
run_case after-parent-before-range-response 4
python3 - "$RESULTS" "$RESULT_DIR/summary.json" <<'PY'
import csv
import json
import sys
source, target = sys.argv[1:]
with open(source) as stream:
    cases = list(csv.DictReader(stream, delimiter='\t'))
json.dump({'route': 'local-no-parent', 'status': 'PASS', 'cases': cases}, open(target, 'w'), indent=2)
open(target, 'a').write('\n')
PY
printf 'QUERY-DISCONNECT PASS\n'
rm -rf "$WORK_ROOT"
