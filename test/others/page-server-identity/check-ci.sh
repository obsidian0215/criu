#!/bin/sh
set -eu

cd /criu
make -j4
make unittest
make -C test/others/page-server-identity workload
export PYTHONPATH=/criu/lib

# Keep both failing baseline cases, not just the first assertion failure.
baseline=$(mktemp -d)
trap 'rm -rf "$baseline"' EXIT
git -c safe.directory=/criu archive 2e2d58c78fe9f12d36baa4d029308535e3b16653 | tar -x -C "$baseline"
make -C "$baseline" -j4 > test/others/page-server-identity/baseline-build.log 2>&1
for case in predump ancestor; do
	set +e
	CRIU="$baseline/criu/criu" python3 test/others/page-server-identity/run.py --case "$case" \
		> "test/others/page-server-identity/baseline-$case.log" 2>&1
	status=$?
	set -e
	if [ "$status" -eq 0 ]; then
		echo "Baseline unexpectedly passed $case"
		exit 1
	fi
	if [ "$case" = predump ]; then
		grep -q "AssertionError: ('predump-splice-mismatch'" \
			test/others/page-server-identity/baseline-predump.log
	else
		grep -q 'Parent memory generation does not match' \
			test/others/page-server-identity/work/broken-ancestor/target-final/restore.log
	fi
	mv test/others/page-server-identity/work "test/others/page-server-identity/baseline-$case"
	echo "REPRODUCED $case on baseline"
done
python3 test/others/page-server-identity/run.py
