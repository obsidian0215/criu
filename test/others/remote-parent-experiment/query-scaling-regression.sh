#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
PROXY="$SCRIPT_DIR/query_delay_proxy.py"
WORK_ROOT="$SCRIPT_DIR/query-scaling-work"
RESULT_DIR="$SCRIPT_DIR/results/query-scaling"
WORKLOAD="$WORK_ROOT/query-fragment-workload"
RESULTS="$RESULT_DIR/results.tsv"
EXPECTATION=${REMOTE_PARENT_QUERY_EXPECTATION:-characterize}
PID=""
LAUNCHER_PID=""
PAGE_SERVER_PID=""
PROXY_PID=""
SERVER_PORT=""
STATE_FILE=""
RESULT_FILE=""

case "$EXPECTATION" in
	characterize|per-range|batched) ;;
	*) echo "invalid REMOTE_PARENT_QUERY_EXPECTATION: $EXPECTATION" >&2; exit 2 ;;
esac

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
	if [ -n "$LAUNCHER_PID" ]; then
		wait "$LAUNCHER_PID" 2>/dev/null || true
	fi
	LAUNCHER_PID=""
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	find "$WORK_ROOT" -type f \( -name '*.log' -o -name '*.json' -o -name '*.state' -o -name '*.tsv' \) \
		-print -exec sh -c 'echo "===== $1 ====="; tail -n 160 "$1"' _ {} \; 2>/dev/null >&2 || true
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
	for _ in $(seq 1 200); do
		kill -0 "$pid" 2>/dev/null || return 1
		ss -H -ltn "sport = :$port" | grep -q . && return 0
		sleep 0.02
	done
	return 1
}

wait_ready() {
	local pid=$1 file=$2
	for _ in $(seq 1 200); do
		[ -s "$file" ] && return 0
		kill -0 "$pid" 2>/dev/null || return 1
		sleep 0.02
	done
	return 1
}

wait_phase() {
	local phase=$1
	for _ in $(seq 1 300); do
		grep -q "^phase=$phase$" "$STATE_FILE" 2>/dev/null && return 0
		[ -z "$PID" ] || kill -0 "$PID" 2>/dev/null || return 1
		sleep 0.02
	done
	return 1
}

state_value() {
	local key=$1
	awk -F= -v key="$key" '$1 == key { print $2; exit }' "$STATE_FILE"
}

start_server() {
	local directory=$1 parent=${2:-}
	local args=()
	SERVER_PORT=$(free_port)
	[ -z "$parent" ] || args+=(--prev-images-dir "../$(basename "$parent")")
	"${CRIU_CMD[@]}" page-server -D "$directory" -o page-server.log -v4 \
		--port "$SERVER_PORT" "${args[@]}" >"$directory/server-command.log" 2>&1 &
	PAGE_SERVER_PID=$!
	wait_listen "$PAGE_SERVER_PID" "$SERVER_PORT" || return 1
}

finish_server() {
	local status=0
	wait "$PAGE_SERVER_PID" || status=$?
	PAGE_SERVER_PID=""
	return "$status"
}

start_workload() {
	local base=$1
	STATE_FILE="$base/workload.state"
	RESULT_FILE="$base/result"
	"$WORKLOAD" "$STATE_FILE" "$RESULT_FILE" >"$base/workload.log" 2>&1 &
	LAUNCHER_PID=$!
	PID=$LAUNCHER_PID
	wait_phase ready || fail "workload did not become ready"
	[ "$(state_value pid)" = "$PID" ] || fail "workload pid mismatch"
}

remote_predump() {
	local source=$1 target=$2
	mkdir -p "$source" "$target"
	start_server "$target" || fail "pre-dump page server did not start"
	"${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log \
		-t "$PID" -v4 --track-mem --shell-job --page-server \
		--address 127.0.0.1 --port "$SERVER_PORT" || fail "remote pre-dump failed"
	finish_server || fail "pre-dump page server failed"
}

sum_pages() {
	local directory=$1 total=0 file
	for file in "$directory"/pages-*.img; do
		[ -e "$file" ] || continue
		total=$((total + $(wc -c <"$file")))
	done
	printf '%d\n' "$total"
}

restore_and_verify() {
	local image=$1 base=$2 root_pid
	rm -f "$base/restore.pid" "$RESULT_FILE"
	"${CRIU_CMD[@]}" restore -D "$image" -o restore.log -v4 -d --shell-job \
		--pidfile "$base/restore.pid" || return 1
	for _ in $(seq 1 200); do
		[ -s "$base/restore.pid" ] && break
		sleep 0.02
	done
	[ -s "$base/restore.pid" ] || return 1
	root_pid=$(cat "$base/restore.pid")
	kill -TERM "$root_pid" || return 1
	for _ in $(seq 1 300); do
		[ -s "$RESULT_FILE" ] && break
		kill -0 "$root_pid" 2>/dev/null || true
		sleep 0.02
	done
	grep -qx PASS "$RESULT_FILE"
}

run_case() {
	local delay=$1
	local base="$WORK_ROOT/delay-$delay"
	local source_pre="$base/source-pre" target_pre="$base/target-pre"
	local source_final="$base/source-final" target_final="$base/target-final"
	local query_dir="$base/query-final"
	local target_port proxy_port ready stats start_ns end_ns elapsed
	local server_status=0 proxy_status=0 messages up down partial payload pre_payload
	cleanup
	rm -rf "$base"
	mkdir -p "$base" "$source_final" "$target_final" "$query_dir"
	start_workload "$base"
	remote_predump "$source_pre" "$target_pre"
	pre_payload=$(sum_pages "$target_pre")
	[ "$pre_payload" -gt $((4 * 1024 * 1024)) ] || fail "pre-dump payload is too small"
	kill -USR1 "$PID" || fail "unable to fragment the dirty set"
	wait_phase mutated || fail "workload did not mutate alternating pages"

	start_server "$query_dir" "$target_pre" || fail "query page server did not start"
	target_port=$SERVER_PORT
	proxy_port=$(free_port)
	ready="$base/proxy.ready"
	stats="$base/proxy-stats.json"
	python3 "$PROXY" --listen-port "$proxy_port" --target-port "$target_port" \
		--response-delay-ms "$delay" --stats "$stats" --ready "$ready" \
		>"$base/proxy.log" 2>&1 &
	PROXY_PID=$!
	wait_ready "$PROXY_PID" "$ready" || fail "query delay proxy did not start"

	start_ns=$(date +%s%N)
	"${CRIU_CMD[@]}" dump -D "$source_final" -o dump.log -t "$PID" -v4 \
		--track-mem --shell-job --prev-images-dir "../$(basename "$source_pre")" \
		--page-server-parent --address 127.0.0.1 --port "$proxy_port" ||
		fail "query-backed final dump failed"
	end_ns=$(date +%s%N)
	elapsed=$(((end_ns - start_ns) / 1000000))
	PID=""
	if [ -n "$LAUNCHER_PID" ]; then
		wait "$LAUNCHER_PID" 2>/dev/null || true
		LAUNCHER_PID=""
	fi
	wait "$PROXY_PID" || proxy_status=$?
	PROXY_PID=""
	wait "$PAGE_SERVER_PID" || server_status=$?
	PAGE_SERVER_PID=""
	[ "$proxy_status" -eq 0 ] || fail "query delay proxy failed"
	[ "$server_status" -eq 0 ] || fail "query page server failed"
	[ -s "$stats" ] || fail "query proxy produced no statistics"
	read -r up down messages partial < <(python3 - "$stats" <<'PY'
import json
import sys
value = json.load(open(sys.argv[1]))
print(value['source_to_server'], value['server_to_source'],
      value['response_messages'], value['partial_response_bytes'])
PY
)
	[ "$up" -gt 0 ] && [ "$down" -gt 0 ] || fail "query path exchanged no traffic"
	[ "$partial" -eq 0 ] || fail "query proxy saw a partial response"
	payload=$(sum_pages "$source_final")
	[ "$payload" -gt $((2 * 1024 * 1024)) ] || fail "final dirty payload is too small"
	[ "$payload" -lt "$pre_payload" ] || fail "final dump was not incremental"
	cp -a "$source_final/." "$target_final/"
	rm -f "$target_final/parent"
	ln -s ../target-pre "$target_final/parent"
	restore_and_verify "$target_final" "$base" || fail "fragmented dirty-set restore failed"
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$delay" "$elapsed" "$messages" "$up" "$down" "$payload" >>"$RESULTS"
}

rm -rf "$WORK_ROOT" "$RESULT_DIR"
mkdir -p "$WORK_ROOT" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/query_fragment_workload.c" -o "$WORKLOAD"
printf 'delay_ms\tdump_ms\tresponse_messages\tsource_to_server\tserver_to_source\tfinal_payload\n' >"$RESULTS"
run_case 0
run_case 5

python3 - "$EXPECTATION" "$RESULTS" "$RESULT_DIR/summary.json" <<'PY'
import csv
import json
import sys
expectation, source, target = sys.argv[1:]
with open(source) as stream:
    rows = list(csv.DictReader(stream, delimiter='\t'))
if len(rows) != 2:
    raise SystemExit('expected two scaling cases')
for row in rows:
    for key in row:
        row[key] = int(row[key])
base, delayed = rows
if base['response_messages'] != delayed['response_messages']:
    raise SystemExit('query response count changed between delay cases')
messages = base['response_messages']
penalty = delayed['dump_ms'] - base['dump_ms']
if expectation == 'per-range':
    if messages < 900:
        raise SystemExit(f'expected fragmented per-range queries, got {messages}')
    if penalty < 2500:
        raise SystemExit(f'5ms response delay added only {penalty}ms')
elif expectation == 'batched':
    if messages > 16:
        raise SystemExit(f'expected batched queries, got {messages} responses')
    if penalty > 1500:
        raise SystemExit(f'batched query delay penalty is too high: {penalty}ms')
result = {
    'route': 'local-no-parent',
    'case': 'fragmented-query-scaling',
    'status': 'PASS',
    'expectation': expectation,
    'response_messages': messages,
    'delay_penalty_ms': penalty,
    'runs': rows,
}
json.dump(result, open(target, 'w'), indent=2)
open(target, 'a').write('\n')
print(json.dumps(result, sort_keys=True))
PY
printf 'QUERY-SCALING PASS expectation=%s\n' "$EXPECTATION"
rm -rf "$WORK_ROOT"
