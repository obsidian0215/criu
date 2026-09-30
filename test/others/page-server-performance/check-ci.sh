#!/bin/sh
set -eu
cd /criu
benchmark=/criu/test/others/page-server-performance
output="$benchmark/evidence"
mkdir -p "$output"
base_revision=3e067bc2639b7f98e8bc64b30c05f60b4dd52df8
candidate_revision=ef3736b245a10bbb90c4ce9bf1295e57c1f7abb1
baseline=$(mktemp -d)
candidate=$(mktemp -d)
trap 'rm -rf "$baseline" "$candidate"' EXIT
for variant in baseline candidate; do
    if [ "$variant" = baseline ]; then
        revision=$base_revision
        directory=$baseline
    else
        revision=$candidate_revision
        directory=$candidate
    fi
    git -c safe.directory=/criu archive "$revision" | tar -x -C "$directory"
    make -C "$directory" -j4 NO_LZ4=1 > "$output/$variant-build.log" 2>&1
    printf '%s %s\n' "$variant" "$revision" >> "$output/revisions.txt"
done
${CC:-gcc} -O2 -Wall -Wextra -Werror "$benchmark/workload.c" -o "$benchmark/workload"
${CC:-gcc} -O2 -Wall -Wextra -Werror "$benchmark/tcp-metrics.c" -o "$benchmark/tcp-metrics"
export PYTHONPATH="$candidate/lib"
# No caches are dropped and no network or host security settings are changed.
# A correctness failure stops the benchmark and is retained as evidence.
python3 "$benchmark/run.py" --baseline "$baseline/criu/criu" \
    --candidate "$candidate/criu/criu" --output "$output/smoke" \
    --memory-mib 16 --predumps 1 --repetitions 1 --dirty 1 --mode splice
python3 "$benchmark/summarize.py" "$output/smoke" | tee "$output/smoke-summary.json"
python3 "$benchmark/run.py" --baseline "$baseline/criu/criu" \
    --candidate "$candidate/criu/criu" --output "$output/screening" \
    --memory-mib 256 --predumps 3 --repetitions 5 --dirty 0 1 20 --mode splice \
    --full-final-control
python3 "$benchmark/summarize.py" "$output/screening" | tee "$output/summary.json"
