#!/bin/sh
set -eu

cd /criu
make -j4
make unittest
make -C test/others/page-server-identity workload
export PYTHONPATH=/criu/lib


# Check that the production changes build without later commits.
for revision in 32ae6071ee759528afa94a49917a943db6610b64 76df9c0903129b7994c007c71687eb2e8e71e5fc; do
    (
        stage=$(mktemp -d)
        trap 'rm -rf "$stage"' EXIT
        git -c safe.directory=/criu archive "$revision" | tar -x -C "$stage"
        make -C "$stage" -j4 > "test/others/page-server-identity/commit-$revision.log" 2>&1
        make -C "$stage" unittest >> "test/others/page-server-identity/commit-$revision.log" 2>&1
        echo "PASS standalone commit $revision"
    )
done

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

mv test/others/page-server-identity/work test/others/page-server-identity/with-lz4
(
    plain=$(mktemp -d)
    trap 'rm -rf "$plain"' EXIT
    git -c safe.directory=/criu archive ef3736b245a10bbb90c4ce9bf1295e57c1f7abb1 | tar -x -C "$plain"
    make -C "$plain" -j4 NO_LZ4=1 > test/others/page-server-identity/no-lz4-build.log 2>&1
    make -C "$plain" unittest NO_LZ4=1 >> test/others/page-server-identity/no-lz4-build.log 2>&1
    CRIU="$plain/criu/criu" python3 test/others/page-server-identity/run.py
    echo "PASS candidate without LZ4"
)
