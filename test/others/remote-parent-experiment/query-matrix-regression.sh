#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
SEQUENCE=${PRE_DUMP_SEQUENCE:-splice}
COMPRESSION=${REMOTE_PARENT_COMPRESSION:-plain}

case "$SEQUENCE" in
	read) ROUND_MODES=(read read read) ;;
	splice) ROUND_MODES=(splice splice splice) ;;
	mixed) ROUND_MODES=(read splice read) ;;
	*) echo "invalid pre-dump sequence: $SEQUENCE" >&2; exit 2 ;;
esac
case "$COMPRESSION" in
	plain) MEMORY_OPTS=() ;;
	lz4) MEMORY_OPTS=(--compress) ;;
	*) echo "invalid compression variant: $COMPRESSION" >&2; exit 2 ;;
esac
[ "$ROUTE" = local-no-parent ] || {
	echo "remote parent query matrix requires the local-no-parent route" >&2
	exit 2
}

CRIU_CMD=("$TOP/criu/criu" --no-default-config)
ZDTM_DIR="$TOP/test/zdtm/static"
CASE_ID="$SEQUENCE-$COMPRESSION"
WORK_DIR="$SCRIPT_DIR/query-matrix-work-$CASE_ID"
RESULT_DIR="$SCRIPT_DIR/results/query-matrix-$CASE_ID"
BASE_PAGE_LAUNCHER="$WORK_DIR/base-page-launcher"
ORACLE="$SCRIPT_DIR/generation_oracle.py"
PROXY="$SCRIPT_DIR/tcp_proxy.py"
ORACLE_STATE="$WORK_DIR/generation.json"
PID=""
PAGE_SERVER_PID=""
PROXY_PID=""
LAST_WIRE_UP=0
LAST_WIRE_DOWN=0

fail() {
	echo "FAIL: $*" >&2
	find "$WORK_DIR" -type f \( -name '*.log' -o -name '*.json' -o -name '*.out' \) -print -exec sh -c '
		for file do
			echo "===== $file ====="
			tail -n 160 "$file"
		done
	' sh {} + 2>/dev/null >&2 || true
	exit 1
}

skip() {
	mkdir -p "$RESULT_DIR"
	printf '{"route":"%s","case":"%s","status":"SKIP","reason":"%s"}\n' \
		"$ROUTE" "$CASE_ID" "$1" > "$RESULT_DIR/summary.json"
	printf 'SKIP route=%s case=%s reason=%s\n' "$ROUTE" "$CASE_ID" "$1"
	exit 0
}

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
	local directory=$1 port=$2 parent=${3:-}
	local args=()
	[ -z "$parent" ] || args+=(--prev-images-dir "../$(basename "$parent")")
	"${CRIU_CMD[@]}" page-server -D "$directory" -o page-server.log -v4 --port "$port" "${args[@]}" \
		>"$directory/server-command.log" 2>&1 &
	PAGE_SERVER_PID=$!
	wait_listen "$PAGE_SERVER_PID" "$port" || fail "page server did not start"
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
	fail "wire proxy did not start"
}

finish_transfer() {
	local stats=$1 server_status=0 proxy_status=0
	wait "$PAGE_SERVER_PID" || server_status=$?
	PAGE_SERVER_PID=""
	wait "$PROXY_PID" || proxy_status=$?
	PROXY_PID=""
	[ "$server_status" -eq 0 ] || fail "page server failed"
	[ "$proxy_status" -eq 0 ] || fail "wire proxy failed"
	read -r LAST_WIRE_UP LAST_WIRE_DOWN < <(python3 - "$stats" <<'PY'
import json
import sys
value = json.load(open(sys.argv[1]))
print(value['source_to_server'], value['server_to_source'])
PY
)
}

run_predump() {
	local source=$1 target=$2 source_parent=$3 target_parent=$4 label=$5 mode=$6
	local server_port proxy_port stats ready
	local args=(--track-mem --page-server --address 127.0.0.1)
	mkdir -p "$source" "$target"
	server_port=$(free_port)
	proxy_port=$(free_port)
	stats="$WORK_DIR/$label-wire.json"
	ready="$WORK_DIR/$label-proxy.ready"
	start_server "$target" "$server_port" "$target_parent"
	start_proxy "$proxy_port" "$server_port" "$stats" "$ready"
	args+=(--port "$proxy_port")
	args+=("${MEMORY_OPTS[@]}")
	[ -z "$source_parent" ] || args+=(--prev-images-dir "../$(basename "$source_parent")")
	"${CRIU_CMD[@]}" pre-dump --pre-dump-mode "$mode" -D "$source" -o dump.log \
		-t "$PID" -v4 "${args[@]}" || fail "$label pre-dump failed"
	finish_transfer "$stats"
}

run_query_final() {
	local source=$1 source_parent=$2 query_dir=$3 target_parent=$4
	local server_port proxy_port stats ready source_status=0
	mkdir -p "$source" "$query_dir"
	server_port=$(free_port)
	proxy_port=$(free_port)
	stats="$WORK_DIR/final-query-wire.json"
	ready="$WORK_DIR/final-query-proxy.ready"
	start_server "$query_dir" "$server_port" "$target_parent"
	start_proxy "$proxy_port" "$server_port" "$stats" "$ready"
	"${CRIU_CMD[@]}" dump -D "$source" -o dump.log -t "$PID" -v4 --track-mem \
		"${MEMORY_OPTS[@]}" --prev-images-dir "../$(basename "$source_parent")" \
		--page-server-parent --address 127.0.0.1 --port "$proxy_port" || source_status=$?
	if [ "$source_status" -ne 0 ]; then
		cleanup
		fail "local final dump could not validate the remote parent"
	fi
	finish_transfer "$stats"
}

sum_glob_bytes() {
	local directory=$1 pattern=$2 total=0 file
	for file in "$directory"/$pattern; do
		[ -e "$file" ] || continue
		total=$((total + $(wc -c < "$file")))
	done
	printf '%d\n' "$total"
}

run_oracle() {
	PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" python3 "$ORACLE" "$@"
}

start_workload() {
	make -C "$ZDTM_DIR" compress_pages00.cleanout >/dev/null
	make -C "$ZDTM_DIR" compress_pages00 >/dev/null
	(cd "$ZDTM_DIR" && "$BASE_PAGE_LAUNCHER" make compress_pages00.pid)
	PID=$(cat "$ZDTM_DIR/compress_pages00.pid")
	kill -0 "$PID" 2>/dev/null || fail "workload did not start"
}

stop_workload() {
	run_oracle verify-and-reset "$PID" "$ORACLE_STATE" || fail "restored data mismatch"
	(cd "$ZDTM_DIR" && make compress_pages00.stop && grep PASS compress_pages00.out) \
		>"$WORK_DIR/stop.log" 2>&1 || fail "workload verification failed"
	PID=""
}

assemble_final() {
	local source=$1 target=$2 parent=$3
	mkdir -p "$target"
	cp -a "$source/." "$target/"
	rm -f "$target/parent"
	ln -s "../$(basename "$parent")" "$target/parent"
}

rm -rf "$WORK_DIR" "$RESULT_DIR"
mkdir -p "$WORK_DIR" "$RESULT_DIR"
"${CC:-cc}" -Wall -Wextra -Werror "$SCRIPT_DIR/base_page_launcher.c" -o "$BASE_PAGE_LAUNCHER"

if [ "$COMPRESSION" = lz4 ] && ! "${CRIU_CMD[@]}" check --feature compress >"$WORK_DIR/compress-check.log" 2>&1; then
	skip "compression feature unavailable"
fi

SOURCE_PRE1="$WORK_DIR/source-pre1"
SOURCE_PRE2="$WORK_DIR/source-pre2"
SOURCE_PRE3="$WORK_DIR/source-pre3"
TARGET_PRE1="$WORK_DIR/target-pre1"
TARGET_PRE2="$WORK_DIR/target-pre2"
TARGET_PRE3="$WORK_DIR/target-pre3"
SOURCE_FINAL="$WORK_DIR/source-final"
TARGET_FINAL="$WORK_DIR/target-final"
QUERY_DIR="$WORK_DIR/query-final"

start_workload
run_predump "$SOURCE_PRE1" "$TARGET_PRE1" "" "" pre1 "${ROUND_MODES[0]}"
PRE1_WIRE=$LAST_WIRE_UP
PRE1_PAYLOAD=$(sum_glob_bytes "$TARGET_PRE1" 'pages-*.img')
if [ "$COMPRESSION" = plain ]; then
	[ "$PRE1_PAYLOAD" -gt $((8 * 1024 * 1024)) ] || fail "first pre-dump payload is too small"
else
	[ "$PRE1_PAYLOAD" -gt 0 ] || fail "first compressed pre-dump has no payload"
	grep -q "Adding compressed" "$TARGET_PRE1/page-server.log" ||
		fail "first pre-dump did not record compressed page ranges"
fi
run_oracle mutate "$TARGET_PRE1" "$PID" "$ORACLE_STATE" || fail "first mutation failed"

run_predump "$SOURCE_PRE2" "$TARGET_PRE2" "$SOURCE_PRE1" "$TARGET_PRE1" pre2 "${ROUND_MODES[1]}"
PRE2_WIRE=$LAST_WIRE_UP
PRE2_PAYLOAD=$(sum_glob_bytes "$TARGET_PRE2" 'pages-*.img')
run_oracle check-image "$TARGET_PRE2" "$PID" "$ORACLE_STATE" || fail "second generation placement is wrong"
[ "$PRE2_PAYLOAD" -gt 0 ] || fail "second pre-dump has no payload"
[ $((PRE2_WIRE * 4)) -lt "$PRE1_WIRE" ] || fail "second pre-dump wire traffic is not incremental"
run_oracle mutate "$TARGET_PRE2" "$PID" "$ORACLE_STATE" || fail "second mutation failed"

run_predump "$SOURCE_PRE3" "$TARGET_PRE3" "$SOURCE_PRE2" "$TARGET_PRE2" pre3 "${ROUND_MODES[2]}"
PRE3_WIRE=$LAST_WIRE_UP
PRE3_PAYLOAD=$(sum_glob_bytes "$TARGET_PRE3" 'pages-*.img')
run_oracle check-image "$TARGET_PRE3" "$PID" "$ORACLE_STATE" || fail "third generation placement is wrong"
[ "$PRE3_PAYLOAD" -gt 0 ] || fail "third pre-dump has no payload"
[ $((PRE3_WIRE * 4)) -lt "$PRE1_WIRE" ] || fail "third pre-dump wire traffic is not incremental"
run_oracle mutate "$TARGET_PRE3" "$PID" "$ORACLE_STATE" || fail "third mutation failed"

[ "$(sum_glob_bytes "$SOURCE_PRE3" 'pages-*.img')" -eq 0 ] || fail "source retained pre-dump page payload"
[ "$(sum_glob_bytes "$SOURCE_PRE3" 'pagemap-*.img')" -eq 0 ] || fail "source retained pre-dump pagemap"
[ "$(sum_glob_bytes "$SOURCE_PRE3" 'remote-parent-*.img')" -eq 0 ] || fail "source retained side metadata"

run_query_final "$SOURCE_FINAL" "$SOURCE_PRE3" "$QUERY_DIR" "$TARGET_PRE3"
QUERY_UP=$LAST_WIRE_UP
QUERY_DOWN=$LAST_WIRE_DOWN
[ "$QUERY_UP" -gt 0 ] && [ "$QUERY_DOWN" -gt 0 ] || fail "final dump exchanged no parent query traffic"
[ "$(sum_glob_bytes "$QUERY_DIR" 'pages-*.img')" -eq 0 ] || fail "query server wrote page payload"
[ "$(sum_glob_bytes "$QUERY_DIR" 'pagemap-*.img')" -eq 0 ] || fail "query server wrote a pagemap"

run_oracle check-image "$SOURCE_FINAL" "$PID" "$ORACLE_STATE" || fail "final generation placement is wrong"
FINAL_PAYLOAD=$(sum_glob_bytes "$SOURCE_FINAL" 'pages-*.img')
[ "$FINAL_PAYLOAD" -gt 0 ] || fail "final dump has no dirty payload"
[ $((FINAL_PAYLOAD * 4)) -lt "$PRE1_PAYLOAD" ] || fail "final dump fell back toward a full image"

assemble_final "$SOURCE_FINAL" "$TARGET_FINAL" "$TARGET_PRE3"
PID=""
"${CRIU_CMD[@]}" restore -D "$TARGET_FINAL" -o restore.log -v4 -d || fail "restore failed"
PID=$(cat "$ZDTM_DIR/compress_pages00.pid")
stop_workload

cat >"$RESULT_DIR/summary.json" <<EOF_JSON
{
  "route": "$ROUTE",
  "case": "$CASE_ID",
  "status": "PASS",
  "round_modes": ["${ROUND_MODES[0]}", "${ROUND_MODES[1]}", "${ROUND_MODES[2]}"],
  "compression": "$COMPRESSION",
  "pre1_wire_source_to_server": $PRE1_WIRE,
  "pre2_wire_source_to_server": $PRE2_WIRE,
  "pre3_wire_source_to_server": $PRE3_WIRE,
  "query_wire_source_to_server": $QUERY_UP,
  "query_wire_server_to_source": $QUERY_DOWN,
  "pre1_page_payload": $PRE1_PAYLOAD,
  "pre2_page_payload": $PRE2_PAYLOAD,
  "pre3_page_payload": $PRE3_PAYLOAD,
  "final_page_payload": $FINAL_PAYLOAD
}
EOF_JSON
cp "$WORK_DIR"/*-wire.json "$RESULT_DIR/" 2>/dev/null || true
printf 'QUERY-MATRIX PASS case=%s query-up=%s query-down=%s final-pages=%s\n' \
	"$CASE_ID" "$QUERY_UP" "$QUERY_DOWN" "$FINAL_PAYLOAD"
rm -rf "$WORK_DIR"
