#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
CRIU_CMD=("$TOP/criu/criu" --no-default-config)

if [ "$#" -lt 5 ]; then
	echo "usage: $0 PID SOURCE_FINAL SOURCE_PARENT QUERY_DIR TARGET_PARENT [DUMP_OPTION ...]" >&2
	exit 2
fi

PID=$1
SOURCE_FINAL=$2
SOURCE_PARENT=$3
QUERY_DIR=$4
TARGET_PARENT=$5
shift 5
PAGE_SERVER_PID=""

cleanup() {
	if [ -n "$PAGE_SERVER_PID" ] && kill -0 "$PAGE_SERVER_PID" 2>/dev/null; then
		kill -KILL "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	if [ -n "$PAGE_SERVER_PID" ]; then
		wait "$PAGE_SERVER_PID" 2>/dev/null || true
	fi
	PAGE_SERVER_PID=""
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

mkdir -p "$SOURCE_FINAL" "$QUERY_DIR"
PORT=$(free_port)
"${CRIU_CMD[@]}" page-server -D "$QUERY_DIR" -o page-server.log -v4 --port "$PORT" \
	--prev-images-dir "../$(basename "$TARGET_PARENT")" \
	>"$QUERY_DIR/server-command.log" 2>&1 &
PAGE_SERVER_PID=$!
wait_listen "$PAGE_SERVER_PID" "$PORT" || {
	echo "query page server did not start" >&2
	exit 1
}

source_status=0
"${CRIU_CMD[@]}" dump -D "$SOURCE_FINAL" -o dump.log -t "$PID" -v4 --track-mem \
	--prev-images-dir "../$(basename "$SOURCE_PARENT")" \
	--page-server-parent --address 127.0.0.1 --port "$PORT" "$@" || source_status=$?
if [ "$source_status" -ne 0 ]; then
	exit "$source_status"
fi

server_status=0
wait "$PAGE_SERVER_PID" || server_status=$?
PAGE_SERVER_PID=""
exit "$server_status"
