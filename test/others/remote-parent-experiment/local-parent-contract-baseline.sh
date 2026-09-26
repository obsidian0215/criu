#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
WORK_DIR="$SCRIPT_DIR/local-parent-contract-work"
RESULT_DIR="$SCRIPT_DIR/results/local-parent-contract"
WORKLOAD="$WORK_DIR/parent-identity-workload"
STATE_FILE="$WORK_DIR/workload.state"
EXPECTED_FILE="$WORK_DIR/expected.hex"
PID=""
ORACLE_RESULT=""

cleanup() {
	if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
		kill -KILL "$PID" 2>/dev/null || true
		wait "$PID" 2>/dev/null || true
	fi
	PID=""
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	find "$WORK_DIR" -type f \( -name '*.log' -o -name '*.state' -o -name '*.hex' \) -print -exec sh -c '
		for file do
			echo "===== $file ====="
			tail -n 160 "$file"
		done
	' sh {} + 2>/dev/null >&2 || true
	exit 1
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

rm -rf "$WORK_DIR" "$RESULT_DIR"
mkdir -p "$WORK_DIR" "$RESULT_DIR"
"${CC:-cc}" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/parent_identity_workload.c" -o "$WORKLOAD"
printf '31\n' >"$EXPECTED_FILE"
"$WORKLOAD" "$STATE_FILE" "$EXPECTED_FILE" >"$WORK_DIR/workload.log" 2>&1 &
PID=$!
ROOT_PID=$PID
wait_phase READY || fail "workload did not become ready"
[ "$(field pid)" = "$PID" ] || fail "workload pid mismatch"

PRE1="$WORK_DIR/pre1"
PRE2="$WORK_DIR/pre2"
FINAL="$WORK_DIR/final"
mkdir -p "$PRE1" "$PRE2" "$FINAL"
"${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$PRE1" -o dump.log \
	-t "$PID" -v4 --track-mem || fail "first local pre-dump failed"
kill -USR1 "$PID" || fail "unable to create a second generation"
wait_phase GEN2 || fail "workload did not reach generation two"
printf '62\n' >"$EXPECTED_FILE"
"${CRIU_CMD[@]}" pre-dump --pre-dump-mode splice -D "$PRE2" -o dump.log \
	-t "$PID" -v4 --track-mem --prev-images-dir ../pre1 || fail "second local pre-dump failed"
"${CRIU_CMD[@]}" dump -D "$FINAL" -o dump.log -t "$PID" -v4 --track-mem \
	--prev-images-dir ../pre2 || fail "local final dump failed"
PID=""
rm -f "$FINAL/parent"
ln -s ../pre1 "$FINAL/parent"

if ! "${CRIU_CMD[@]}" restore -D "$FINAL" -o restore.log -v4 -d; then
	OUTCOME=DETECTED_AT_RESTORE
else
	PID=$ROOT_PID
	kill -0 "$PID" 2>/dev/null || fail "restore reported success but task is missing"
	kill -USR2 "$PID" || fail "unable to run restored workload oracle"
	if wait_phase PASS; then
		OUTCOME=RESTORED_CORRECTLY
	elif grep -q '^FAIL ' "$STATE_FILE" 2>/dev/null; then
		OUTCOME=UNBOUND_PARENT_ACCEPTED
	else
		fail "restored workload did not finish its oracle"
	fi
	PID=""
fi

cat >"$RESULT_DIR/summary.json" <<EOF_JSON
{
  "route": "ordinary-local-parent",
  "case": "generation-skew",
  "status": "CHARACTERIZED",
  "outcome": "$OUTCOME"
}
EOF_JSON
printf 'LOCAL-PARENT-CONTRACT outcome=%s\n' "$OUTCOME"
rm -rf "$WORK_DIR"
