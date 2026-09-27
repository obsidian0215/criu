#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
TOP=$(cd "$SCRIPT_DIR/../../.." && pwd)
CRIU_CMD=("$TOP/criu/criu" --no-default-config)
WORK_DIR="$SCRIPT_DIR/query-cli-work"
RESULT_DIR="$SCRIPT_DIR/results/query-cli"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

reject() {
	local name=$1 expected=$2
	shift 2
	local log="$WORK_DIR/$name.log"

	if "${CRIU_CMD[@]}" "$@" >"$log" 2>&1; then
		fail "$name unexpectedly succeeded"
	fi
	grep -F -- "$expected" "$log" >/dev/null ||
		fail "$name did not report the expected option error"
}

rm -rf "$WORK_DIR" "$RESULT_DIR"
mkdir -p "$WORK_DIR/images" "$RESULT_DIR"

reject conflicting 	"--page-server and --page-server-parent cannot be used together" 	dump -t 1 -D "$WORK_DIR/images" --page-server --page-server-parent 	--prev-images-dir ../previous

reject wrong-command 	"--page-server-parent is only supported with dump" 	pre-dump -t 1 -D "$WORK_DIR/images" --page-server-parent 	--prev-images-dir ../previous

reject missing-parent 	"--page-server-parent requires --prev-images-dir" 	dump -t 1 -D "$WORK_DIR/images" --page-server-parent

cat >"$RESULT_DIR/summary.json" <<'EOF'
{
  "route": "local-no-parent",
  "case": "query-cli",
  "status": "PASS"
}
EOF

printf 'QUERY-CLI PASS\n'
rm -rf "$WORK_DIR"
