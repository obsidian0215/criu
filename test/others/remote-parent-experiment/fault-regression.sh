#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
ROUTE=$(cat "$TOP/experiments/remote-parent/route")
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
ZDTM_DIR="$TOP/test/zdtm/static"
WORK_DIR="$SCRIPT_DIR/fault-work-$ROUTE"
RESULT_DIR="$SCRIPT_DIR/results/faults"
BASE_PAGE_LAUNCHER="$WORK_DIR/base-page-launcher"
FAULT_LIBRARY="$WORK_DIR/fail-image-write.so"
PID=""
PAGE_SERVER_PID=""

case "$ROUTE" in
	final-page-server|local-no-parent|local-pagemap|custom-coverage) ;;
	*) echo "invalid route: $ROUTE" >&2; exit 2 ;;
esac

fail() {
	echo "FAIL: $*" >&2
	find "$WORK_DIR" -type f -name '*.log' -print -exec sh -c 'echo "--- $1"; tail -n 100 "$1"' _ {} \; 2>/dev/null || true
	exit 1
}

cleanup() {
	if [ -n "$PAGE_SERVER_PID" ] && kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
		wait "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		kill -KILL "$PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT

free_port() {
	python3 - <<'PY'
import socket
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    print(sock.getsockname()[1])
PY
}

wait_listen() {
	local port=$1
	for _ in $(seq 1 150); do
		kill -0 "$PAGE_SERVER_PID" 2>/dev/null || return 1
		ss -H -ltn "sport = :$port" | grep -q . && return 0
		sleep 0.02
	done
	return 1
}

wait_server() {
	for _ in $(seq 1 200); do
		kill -0 "$PAGE_SERVER_PID" 2>/dev/null || break
		sleep 0.05
	done
	if kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	wait "$PAGE_SERVER_PID" 2>/dev/null || true
	PAGE_SERVER_PID=""
}

start_workload() {
	make -C "$ZDTM_DIR" compress_pages00.cleanout
	make -C "$ZDTM_DIR" compress_pages00
	(cd "$ZDTM_DIR" && "$BASE_PAGE_LAUNCHER" make compress_pages00.pid)
	PID=$(cat "$ZDTM_DIR/compress_pages00.pid")
	kill -0 "$PID" 2>/dev/null || fail "workload did not start"
}

stop_workload() {
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		(cd "$ZDTM_DIR" && make compress_pages00.stop && grep PASS compress_pages00.out) ||
			fail "workload verification failed"
	fi
	PID=""
}

sum_pages() {
	local directory=$1 total=0 file
	for file in "$directory"/pages-*.img; do
		[ -e "$file" ] || continue
		total=$((total + $(wc -c < "$file")))
	done
	printf '%d\n' "$total"
}

assert_failed_parent_safe() {
	local source=$1
	local base=$2
	local final="$base/final-after-failure"
	local payload
	mkdir -p "$final"
	if "${CRIU_CMD[@]}" dump -D "$final" -o dump.log -t "$PID" -v4 --track-mem \
		--prev-images-dir "../$(basename "$source")"; then
		payload=$(sum_pages "$final")
		[ "$payload" -gt $((8 * 1024 * 1024)) ] ||
			fail "failed parent was accepted as an incremental parent ($payload bytes)"
		printf 'safe-full:%s' "$payload"
		PID=""
	else
		kill -0 "$PID" 2>/dev/null || fail "rejected failed parent killed workload"
		printf 'rejected'
	fi
}

run_case() {
	local label=$1
	local server_image=${2:-}
	local disconnect=${3:-}
	local source_image=${4:-}
	local base="$WORK_DIR/$label"
	local source="$base/source"
	local target="$base/target"
	local port trust
	local server_env=()
	local source_env=()
	mkdir -p "$source" "$target"
	start_workload
	port=$(free_port)
	[ -z "$server_image" ] || server_env+=(CRIU_TEST_FAIL_IMAGE="$server_image")
	[ -z "$disconnect" ] || server_env+=(CRIU_TEST_DISCONNECT="$disconnect")
	[ -z "$source_image" ] || source_env+=(CRIU_TEST_FAIL_IMAGE="$source_image")
	if [ ${#server_env[@]} -gt 0 ]; then
		env LD_PRELOAD="$FAULT_LIBRARY" "${server_env[@]}" \
			"${CRIU_CMD[@]}" page-server -D "$target" -o page-server.log -v4 --port "$port" \
			>"$base/server.log" 2>&1 &
	else
		"${CRIU_CMD[@]}" page-server -D "$target" -o page-server.log -v4 --port "$port" \
			>"$base/server.log" 2>&1 &
	fi
	PAGE_SERVER_PID=$!
	wait_listen "$port" || fail "$label page server did not start"
	if [ ${#source_env[@]} -gt 0 ]; then
		if env LD_PRELOAD="$FAULT_LIBRARY" "${source_env[@]}" \
			"${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log \
			-t "$PID" -v4 --track-mem --page-server --address 127.0.0.1 --port "$port" \
			>"$base/source.log" 2>&1; then
			fail "$label source accepted injected failure"
		fi
	else
		if "${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$source" -o dump.log \
			-t "$PID" -v4 --track-mem --page-server --address 127.0.0.1 --port "$port" \
			>"$base/source.log" 2>&1; then
			fail "$label source accepted injected failure"
		fi
	fi
	wait_server
	grep -q TEST_FAULT "$base/server.log" "$base/source.log" ||
		fail "$label injection did not execute"
	if compgen -G "$source/.remote-parent-*.tmp.*" >/dev/null ||
	   compgen -G "$source/.pagemap-*.img.tmp.*" >/dev/null; then
		fail "$label left temporary parent metadata"
	fi
	kill -0 "$PID" 2>/dev/null || fail "$label did not resume workload"
	trust=$(assert_failed_parent_safe "$source" "$base")
	stop_workload
	printf '%s\tPASS\t%s\n' "$label" "$trust" >> "$RESULT_DIR/results.tsv"
}

rm -rf "$WORK_DIR" "$RESULT_DIR"
mkdir -p "$WORK_DIR" "$RESULT_DIR"
"${CC:-cc}" -Wall -Wextra -Werror "$SCRIPT_DIR/base_page_launcher.c" -o "$BASE_PAGE_LAUNCHER"
"${CC:-cc}" -shared -fPIC -Wall -Wextra -Werror "$SCRIPT_DIR/fail_image_write.c" -ldl -o "$FAULT_LIBRARY"
printf 'case\tstatus\tfailed_parent_handling\n' > "$RESULT_DIR/results.tsv"

run_case destination-pagemap-write pagemap
run_case destination-pages-write pages
run_case disconnect-transfer "" transfer
run_case disconnect-close "" close
run_case source-inventory-write "" "" inventory
case "$ROUTE" in
	local-pagemap|custom-coverage)
		run_case source-parent-write "" "" source-parent
		;;
	*)
		printf 'source-parent-write\tSKIP\troute has no source parent image\n' >> "$RESULT_DIR/results.tsv"
		;;
esac

python3 - "$ROUTE" "$RESULT_DIR/results.tsv" "$RESULT_DIR/summary.json" <<'PY'
import csv, json, sys
route, source, target = sys.argv[1:]
with open(source) as f:
    rows = list(csv.DictReader(f, delimiter='\t'))
json.dump({'route': route, 'status': 'PASS', 'cases': rows}, open(target, 'w'), indent=2)
open(target, 'a').write('\n')
PY
printf 'FAULT-REGRESSION PASS route=%s\n' "$ROUTE"
rm -rf "$WORK_DIR"
