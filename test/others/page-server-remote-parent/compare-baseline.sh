#!/bin/bash
# Run the same remote-pre-dump/local-final sequence with two actual binaries.
set -euo pipefail
if [ "$#" -ne 2 ]; then
	echo "Usage: $0 /absolute/path/to/unmodified/criu /absolute/path/to/candidate/criu" >&2
	exit 2
fi
BASELINE=$(readlink -f "$1")
CANDIDATE=$(readlink -f "$2")
[ -x "$BASELINE" ] && [ -x "$CANDIDATE" ] || exit 2
[ "$BASELINE" != "$CANDIDATE" ] || { echo 'Use distinct baseline and candidate binaries' >&2; exit 2; }
cd "$(dirname "$0")"
RESULTS=$(mktemp -d "$PWD/comparison.XXXXXX")
echo "Keeping comparison evidence in $RESULTS"
# Splice isolates the local-final change from the separate read-buffer fix.
for variant in baseline candidate; do
	binary=$CANDIDATE
	expect_full=0
	if [ "$variant" = baseline ]; then
		binary=$BASELINE
		expect_full=1
	fi
	"$binary" --version > "$RESULTS/$variant-version.txt"
	sha256sum "$binary" > "$RESULTS/$variant-binary.sha256"
	REMOTE_PARENT_CRIU="$binary" REMOTE_PARENT_EXPECT_FULL="$expect_full" \
		REMOTE_PARENT_SINGLE_CASE=1 REMOTE_PARENT_RESULT="$RESULTS/$variant.json" \
		PRE_DUMP_MODE=splice KEEP_WORK_DIR=1 bash ./regression.sh 2>&1 | tee "$RESULTS/$variant.log"
done
python3 - "$RESULTS" <<'PY'
import json
from pathlib import Path
import sys
root = Path(sys.argv[1])
base = json.loads((root / 'baseline.json').read_text())
new = json.loads((root / 'candidate.json').read_text())
assert base['restored_bytes_verified'] and new['restored_bytes_verified']
assert base['coverage_bytes'] == 0 and new['coverage_bytes'] > 0
assert new['final_bytes'] * 4 < base['final_bytes']
print(f"VERIFIED: unmodified final {base['final_bytes']} bytes; "
      f"candidate final {new['final_bytes']} bytes; both restored byte-correct")
PY
