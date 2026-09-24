#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
SEQUENCE=${PRE_DUMP_SEQUENCE:-${PRE_DUMP_MODE:-splice}}
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
case "$ROUTE" in
	final-page-server|local-no-parent|local-pagemap|custom-coverage) ;;
	*) echo "invalid route: $ROUTE" >&2; exit 2 ;;
esac

CRIU_CMD=("$TOP/criu/criu" --no-default-config)
ZDTM_DIR="$TOP/test/zdtm/static"
CASE_ID="$SEQUENCE-$COMPRESSION"
WORK_DIR="$SCRIPT_DIR/work-$ROUTE-$CASE_ID"
RESULT_DIR="$SCRIPT_DIR/results/$CASE_ID"
BASE_PAGE_LAUNCHER="$WORK_DIR/base-page-launcher"
ORACLE="$SCRIPT_DIR/generation_oracle.py"
PROXY="$SCRIPT_DIR/tcp_proxy.py"
PID=""
PAGE_SERVER_PID=""
PROXY_PID=""
ORACLE_STATE="$WORK_DIR/generation.json"
LAST_WIRE_UP=0
LAST_WIRE_DOWN=0

fail() {
	echo "FAIL: $*" >&2
	if [ -d "$WORK_DIR" ]; then
		find "$WORK_DIR" -type f -name '*.log' -print -exec sh -c 'echo "--- $1"; tail -n 120 "$1"' _ {} \; || true
	fi
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
		kill -TERM "$PROXY_PID" 2>/dev/null || true
		wait "$PROXY_PID" 2>/dev/null || true
	fi
	if [ -n "$PAGE_SERVER_PID" ] && kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -TERM "$PAGE_SERVER_PID" 2>/dev/null || true
		wait "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		kill -TERM "$PID" 2>/dev/null || true
		for _ in $(seq 1 50); do
			kill -0 "$PID" 2>/dev/null || break
			sleep 0.1
		done
		kill -KILL "$PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

sum_glob_bytes() {
	local directory=$1
	local pattern=$2
	local total=0
	local file
	for file in "$directory"/$pattern; do
		[ -e "$file" ] || continue
		total=$((total + $(wc -c < "$file")))
	done
	printf '%d\n' "$total"
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
	local pid=$1
	local port=$2
	for _ in $(seq 1 150); do
		kill -0 "$pid" 2>/dev/null || return 1
		if ss -H -ltn "sport = :$port" | grep -q .; then
			return 0
		fi
		sleep 0.02
	done
	return 1
}

start_page_server() {
	local directory=$1
	local target_port=$2
	local parent=${3:-}
	local args=()
	if [ -n "$parent" ]; then
		args+=(--prev-images-dir "../$(basename "$parent")")
	fi
	"${CRIU_CMD[@]}" page-server -D "$directory" -o page-server.log -v4 --port "$target_port" "${args[@]}" &
	PAGE_SERVER_PID=$!
	wait_listen "$PAGE_SERVER_PID" "$target_port" || fail "page server did not start"
}

start_proxy() {
	local proxy_port=$1
	local target_port=$2
	local stats=$3
	local ready=$4
	rm -f "$stats" "$ready"
	python3 "$PROXY" --listen-port "$proxy_port" --target-port "$target_port" --stats "$stats" --ready "$ready" &
	PROXY_PID=$!
	for _ in $(seq 1 100); do
		[ -s "$ready" ] && return 0
		kill -0 "$PROXY_PID" 2>/dev/null || break
		sleep 0.02
	done
	fail "wire proxy did not start"
}

finish_round() {
	local stats=$1
	if ! wait "$PAGE_SERVER_PID"; then
		PAGE_SERVER_PID=""
		fail "page server failed"
	fi
	PAGE_SERVER_PID=""
	if ! wait "$PROXY_PID"; then
		PROXY_PID=""
		fail "wire proxy failed"
	fi
	PROXY_PID=""
	read -r LAST_WIRE_UP LAST_WIRE_DOWN < <(python3 - "$stats" <<'PY'
import json
import sys
value = json.load(open(sys.argv[1]))
print(value['source_to_server'], value['server_to_source'])
PY
)
}

run_remote_round() {
	local operation=$1
	local source_dir=$2
	local target_dir=$3
	local source_parent=${4:-}
	local target_parent=${5:-}
	local label=$6
	local mode=$7
	local target_port proxy_port stats ready
	local args=(--track-mem --page-server --address 127.0.0.1)

	mkdir -p "$source_dir" "$target_dir"
	target_port=$(free_port)
	proxy_port=$(free_port)
	stats="$WORK_DIR/$label-wire.json"
	ready="$WORK_DIR/$label-proxy.ready"
	start_page_server "$target_dir" "$target_port" "$target_parent"
	start_proxy "$proxy_port" "$target_port" "$stats" "$ready"
	args+=(--port "$proxy_port")
	args+=("${MEMORY_OPTS[@]}")
	if [ -n "$source_parent" ]; then
		args+=(--prev-images-dir "../$(basename "$source_parent")")
	fi
	if [ "$operation" = pre-dump ]; then
		"${CRIU_CMD[@]}" pre-dump --pre-dump-mode "$mode" -D "$source_dir" -o dump.log -t "$PID" -v4 "${args[@]}" ||
			fail "$label source pre-dump failed"
	else
		"${CRIU_CMD[@]}" dump -D "$source_dir" -o dump.log -t "$PID" -v4 "${args[@]}" ||
			fail "$label source final dump failed"
	fi
	finish_round "$stats"
}

run_oracle() {
	PYTHONPATH="$TOP/lib${PYTHONPATH:+:$PYTHONPATH}" python3 "$ORACLE" "$@"
}

start_workload() {
	make -C "$ZDTM_DIR" compress_pages00.cleanout
	make -C "$ZDTM_DIR" compress_pages00
	(
		cd "$ZDTM_DIR"
		"$BASE_PAGE_LAUNCHER" make compress_pages00.pid
	)
	PID=$(cat "$ZDTM_DIR/compress_pages00.pid")
	kill -0 "$PID" 2>/dev/null || fail "workload did not start"
}

stop_workload() {
	run_oracle verify-and-reset "$PID" "$ORACLE_STATE" || fail "restored data mismatch"
	(
		cd "$ZDTM_DIR"
		make compress_pages00.stop
		grep PASS compress_pages00.out
	) || fail "workload verification failed"
	PID=""
}

copy_non_memory_images() {
	local source=$1
	local target=$2
	local path base
	for path in "$source"/*; do
		[ -e "$path" ] || continue
		base=$(basename "$path")
		case "$base" in
			pages-*.img|pagemap-*.img|parent) continue ;;
		esac
		cp -a "$path" "$target/"
	done
}

assemble_local_final() {
	local source=$1
	local target=$2
	local parent=$3
	mkdir -p "$target"
	cp -a "$source/." "$target/"
	rm -f "$target/parent"
	ln -s "../$(basename "$parent")" "$target/parent"
}

rm -rf "$WORK_DIR" "$RESULT_DIR"
mkdir -p "$WORK_DIR" "$RESULT_DIR"
"${CC:-cc}" -Wall -Wextra -Werror "$SCRIPT_DIR/base_page_launcher.c" -o "$BASE_PAGE_LAUNCHER"

if [ "$COMPRESSION" = lz4 ]; then
	if ! "${CRIU_CMD[@]}" check --feature compress >"$WORK_DIR/compress-check.log" 2>&1; then
		skip "compression feature unavailable"
	fi
fi

SOURCE_PRE1="$WORK_DIR/source-pre1"
SOURCE_PRE2="$WORK_DIR/source-pre2"
SOURCE_PRE3="$WORK_DIR/source-pre3"
TARGET_PRE1="$WORK_DIR/target-pre1"
TARGET_PRE2="$WORK_DIR/target-pre2"
TARGET_PRE3="$WORK_DIR/target-pre3"
SOURCE_FINAL="$WORK_DIR/source-final"
TARGET_FINAL="$WORK_DIR/target-final"

start_workload
run_remote_round pre-dump "$SOURCE_PRE1" "$TARGET_PRE1" "" "" pre1 "${ROUND_MODES[0]}"
PRE1_WIRE=$LAST_WIRE_UP
PRE1_PAYLOAD=$(sum_glob_bytes "$TARGET_PRE1" 'pages-*.img')
[ "$PRE1_PAYLOAD" -gt $((8 * 1024 * 1024)) ] || fail "first pre-dump payload is too small"
run_oracle mutate "$TARGET_PRE1" "$PID" "$ORACLE_STATE" || fail "first mutation failed"

run_remote_round pre-dump "$SOURCE_PRE2" "$TARGET_PRE2" "$SOURCE_PRE1" "$TARGET_PRE1" pre2 "${ROUND_MODES[1]}"
PRE2_WIRE=$LAST_WIRE_UP
PRE2_PAYLOAD=$(sum_glob_bytes "$TARGET_PRE2" 'pages-*.img')
run_oracle check-image "$TARGET_PRE2" "$PID" "$ORACLE_STATE" || fail "second pre-dump generation placement is wrong"
[ "$PRE2_PAYLOAD" -gt 0 ] || fail "second pre-dump has no payload"
[ $((PRE2_WIRE * 4)) -lt "$PRE1_WIRE" ] || fail "second pre-dump wire traffic is not incremental"
run_oracle mutate "$TARGET_PRE2" "$PID" "$ORACLE_STATE" || fail "second mutation failed"

run_remote_round pre-dump "$SOURCE_PRE3" "$TARGET_PRE3" "$SOURCE_PRE2" "$TARGET_PRE2" pre3 "${ROUND_MODES[2]}"
PRE3_WIRE=$LAST_WIRE_UP
PRE3_PAYLOAD=$(sum_glob_bytes "$TARGET_PRE3" 'pages-*.img')
run_oracle check-image "$TARGET_PRE3" "$PID" "$ORACLE_STATE" || fail "third pre-dump generation placement is wrong"
[ "$PRE3_PAYLOAD" -gt 0 ] || fail "third pre-dump has no payload"
[ $((PRE3_WIRE * 4)) -lt "$PRE1_WIRE" ] || fail "third pre-dump wire traffic is not incremental"
run_oracle mutate "$TARGET_PRE3" "$PID" "$ORACLE_STATE" || fail "third mutation failed"

SOURCE_PRE_PAGES=$(sum_glob_bytes "$SOURCE_PRE3" 'pages-*.img')
SOURCE_PRE_PAGEMAPS=$(sum_glob_bytes "$SOURCE_PRE3" 'pagemap-*.img')
SOURCE_REMOTE_META=$(sum_glob_bytes "$SOURCE_PRE3" 'remote-parent-*.img')
[ "$SOURCE_PRE_PAGES" -eq 0 ] || fail "source retained pre-dump page payload"
case "$ROUTE" in
	final-page-server|local-no-parent)
		[ "$SOURCE_PRE_PAGEMAPS" -eq 0 ] || fail "route unexpectedly retained source pagemaps"
		[ "$SOURCE_REMOTE_META" -eq 0 ] || fail "route unexpectedly retained side metadata"
		;;
	local-pagemap)
		[ "$SOURCE_PRE_PAGEMAPS" -gt 0 ] || fail "standard-pagemap route produced no source pagemap"
		[ "$SOURCE_REMOTE_META" -eq 0 ] || fail "standard-pagemap route retained custom coverage"
		;;
	custom-coverage)
		[ "$SOURCE_REMOTE_META" -gt 0 ] || fail "custom coverage control produced no source metadata"
		;;
esac

if [ "$ROUTE" = final-page-server ]; then
	run_remote_round dump "$SOURCE_FINAL" "$TARGET_FINAL" "$SOURCE_PRE3" "$TARGET_PRE3" final "${ROUND_MODES[2]}"
	FINAL_WIRE=$LAST_WIRE_UP
	copy_non_memory_images "$SOURCE_FINAL" "$TARGET_FINAL"
	[ -L "$TARGET_FINAL/parent" ] || fail "destination final image has no parent link"
	FINAL_IMAGE="$TARGET_FINAL"
	[ $((FINAL_WIRE * 4)) -lt "$PRE1_WIRE" ] || fail "final page-server traffic is not incremental"
else
	mkdir -p "$SOURCE_FINAL"
	"${CRIU_CMD[@]}" dump -D "$SOURCE_FINAL" -o dump.log -t "$PID" -v4 --track-mem \
		"${MEMORY_OPTS[@]}" --prev-images-dir "../$(basename "$SOURCE_PRE3")" || fail "local final dump failed"
	FINAL_WIRE=0
	assemble_local_final "$SOURCE_FINAL" "$TARGET_FINAL" "$TARGET_PRE3"
	FINAL_IMAGE="$SOURCE_FINAL"
fi

run_oracle check-image "$FINAL_IMAGE" "$PID" "$ORACLE_STATE" || fail "final generation placement is wrong"
FINAL_PAYLOAD=$(sum_glob_bytes "$FINAL_IMAGE" 'pages-*.img')
[ "$FINAL_PAYLOAD" -gt 0 ] || fail "final dump has no dirty payload"
[ $((FINAL_PAYLOAD * 4)) -lt "$PRE1_PAYLOAD" ] || fail "final dump fell back toward a full image"

"${CRIU_CMD[@]}" restore -D "$TARGET_FINAL" -o restore.log -v4 -d || fail "restore failed"
stop_workload

cat > "$RESULT_DIR/summary.json" <<EOF_JSON
{
  "route": "$ROUTE",
  "case": "$CASE_ID",
  "status": "PASS",
  "round_modes": ["${ROUND_MODES[0]}", "${ROUND_MODES[1]}", "${ROUND_MODES[2]}"],
  "compression": "$COMPRESSION",
  "pre1_wire_source_to_server": $PRE1_WIRE,
  "pre2_wire_source_to_server": $PRE2_WIRE,
  "pre3_wire_source_to_server": $PRE3_WIRE,
  "final_wire_source_to_server": $FINAL_WIRE,
  "pre1_page_payload": $PRE1_PAYLOAD,
  "pre2_page_payload": $PRE2_PAYLOAD,
  "pre3_page_payload": $PRE3_PAYLOAD,
  "final_page_payload": $FINAL_PAYLOAD,
  "source_pre_pages": $SOURCE_PRE_PAGES,
  "source_pre_pagemaps": $SOURCE_PRE_PAGEMAPS,
  "source_remote_metadata": $SOURCE_REMOTE_META
}
EOF_JSON
cp "$WORK_DIR"/*-wire.json "$RESULT_DIR/" 2>/dev/null || true
printf 'PASS route=%s case=%s pre1-wire=%s pre2-wire=%s pre3-wire=%s final-wire=%s final-pages=%s\n' \
	"$ROUTE" "$CASE_ID" "$PRE1_WIRE" "$PRE2_WIRE" "$PRE3_WIRE" "$FINAL_WIRE" "$FINAL_PAYLOAD"

rm -rf "$WORK_DIR"
