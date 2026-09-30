#!/bin/bash
# Run inside the isolated privileged CI test container. No production workload.
set -euo pipefail
if [ "$#" != 3 ]; then
	echo "Usage: $0 BASELINE_CRIU CANDIDATE_CRIU EVIDENCE_DIR" >&2
	exit 2
fi
BASELINE=$(readlink -f "$1")
CANDIDATE=$(readlink -f "$2")
RESULTS=$(mkdir -p "$3" && cd "$3" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
RUNC=$(command -v runc)
ORIGINAL_PATH=$PATH
test "$BASELINE" != "$CANDIDATE"
test -x "$BASELINE" && test -x "$CANDIDATE"
"$RUNC" --version | tee "$RESULTS/runc-version.txt"
uname -a > "$RESULTS/kernel.txt"
cc -static -O2 -Wall -Wextra -Werror "$HERE/runc-workload.c" -o "$RESULTS/workload"
PS_PID=
SRC_ROOT=
DST_ROOT=
ID=
failure_command=
failure_line=
cleanup() {
	[ -z "$PS_PID" ] || { kill "$PS_PID" 2>/dev/null || true; wait "$PS_PID" 2>/dev/null || true; }
	for root in "$SRC_ROOT" "$DST_ROOT"; do
		[ -z "$root" ] || "$RUNC" --root "$root" delete -f "$ID" 2>/dev/null || true
	done
}
on_exit() {
	local status=$?
	trap - EXIT
	if [ "$status" -ne 0 ]; then
		printf 'RUNC SMOKE FAIL: status=%s line=%s command=%s\n' "$status" "$failure_line" "$failure_command" >&2
		# Keep actionable errors in the job log, even when artifact download fails.
		local log count=0
		while IFS= read -r -d '' log; do
			printf '\n--- %s (last 40 lines) ---\n' "$log" >&2
			tail -n 40 "$log" >&2 || true
			count=$((count + 1))
			[ "$count" -lt 12 ] || break
		done < <(find "$RESULTS" -maxdepth 5 -type f -name '*.log' -print0)
	fi
	cleanup
	exit "$status"
}
trap 'failure_command=$BASH_COMMAND; failure_line=$LINENO' ERR
trap on_exit EXIT
wait_file() {
	for _ in $(seq 1 300); do [ ! -f "$1" ] || return 0; sleep 0.1; done
	echo "Timed out waiting for $1" >&2; return 1
}
sum_images() {
	python3 - "$1" "$2" <<'PY'
import pathlib, sys
print(sum(p.stat().st_size for p in pathlib.Path(sys.argv[1]).glob(sys.argv[2])))
PY
}
for variant in baseline candidate; do
	binary=$CANDIDATE
	[ "$variant" != baseline ] || binary=$BASELINE
	dir="$RESULTS/$variant"
	mkdir -p "$dir"/{bin,source/parent0,source/image,destination/parent0,destination/image,state,bundle/rootfs/state,bundle/rootfs/proc,bundle/rootfs/dev}
	# Modern runc ignores --criu. Both version and swrk use executable lookup.
	ln -s "$binary" "$dir/bin/criu"
	export PATH="$dir/bin:$ORIGINAL_PATH"
	test "$(readlink -f "$(command -v criu)")" = "$binary"
	criu --version > "$dir/criu-version.txt"
	sha256sum "$binary" > "$dir/criu-binary.sha256"
	criu --no-default-config check --feature mem_dirty_track
	cp "$RESULTS/workload" "$dir/bundle/rootfs/workload"
	python3 - "$dir" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
config = {
    'ociVersion': '1.0.2', 'hostname': 'criu-smoke',
    'annotations': {'org.criu.config': ''},
    'process': {'terminal': False, 'user': {'uid': 0, 'gid': 0},
                'args': ['/workload'], 'env': ['PATH=/'], 'cwd': '/',
                'capabilities': {'bounding': [], 'effective': [], 'inheritable': [], 'permitted': [], 'ambient': []}},
    'root': {'path': 'rootfs', 'readonly': True},
    'mounts': [
        {'destination': '/proc', 'type': 'proc', 'source': 'proc'},
        {'destination': '/dev', 'type': 'tmpfs', 'source': 'tmpfs', 'options': ['nosuid', 'strictatime', 'mode=755', 'size=65536k']},
        {'destination': '/state', 'type': 'bind', 'source': str(p / 'state'), 'options': ['rbind', 'rw']}
    ],
    'linux': {'namespaces': [{'type': n} for n in ('pid', 'network', 'ipc', 'uts', 'mount')]}
}
(p / 'bundle/config.json').write_text(json.dumps(config, indent=2) + '\n')
PY
	SRC_ROOT="$dir/source-runtime"
	DST_ROOT="$dir/destination-runtime"
	ID="local-final-$variant"
	"$RUNC" --root "$SRC_ROOT" run -d --bundle "$dir/bundle" "$ID" </dev/null >/dev/null 2>"$dir/run.log"
	wait_file "$dir/state/ready"
	"$RUNC" --root "$SRC_ROOT" state "$ID" > "$dir/source-state.json"
	port=$(python3 - <<'PY'
import socket
with socket.socket() as s:
    s.bind(('127.0.0.1', 0))
    print(s.getsockname()[1])
PY
)
	criu --no-default-config page-server --images-dir "$dir/destination/parent0" --port "$port" --auto-dedup -v4 -o page-server.log &
	PS_PID=$!
	listening=0
	for _ in $(seq 1 100); do
		kill -0 "$PS_PID"
		if [ -n "$(ss -H -ltn "sport = :$port")" ]; then listening=1; break; fi
		sleep 0.05
	done
	test "$listening" = 1
	# Exact topology of issue 2503, with an isolated deterministic workload.
	"$RUNC" --root "$SRC_ROOT" checkpoint --pre-dump --page-server "127.0.0.1:$port" \
		--image-path "$dir/source/parent0" "$ID" >"$dir/pre-dump.log" 2>&1
	wait "$PS_PID"
	PS_PID=
	source_pages=$(sum_images "$dir/source/parent0" 'pages-*.img')
	coverage=$(sum_images "$dir/source/parent0" 'pagemap-*.img')
	pre_bytes=$(sum_images "$dir/destination/parent0" 'pages-*.img')
	test "$source_pages" -eq 0
	test "$pre_bytes" -ge 33554432
	# Save receiver-owned byte hashes and preserve these payload-bearing maps.
	(cd "$dir/destination/parent0"; sha256sum pagemap-*.img pages-*.img) > "$dir/receiver-before.sha256"
	python3 - "$dir/source/parent0" "$dir/destination/parent0" <<'PY'
import pathlib, shutil, sys
src, dst = map(pathlib.Path, sys.argv[1:])
for p in src.iterdir():
    if p.name == 'parent' or p.name.startswith(('pagemap-', 'pages-', '.pagemap-')):
        continue
    if p.is_file():
        shutil.copy2(p, dst / p.name)
PY
	(cd "$dir/destination/parent0"; sha256sum -c "$dir/receiver-before.sha256")
	"$RUNC" --root "$SRC_ROOT" kill "$ID" USR1
	wait_file "$dir/state/changed"
	# Deliberately LOCAL final: do not add --page-server here.
	"$RUNC" --root "$SRC_ROOT" checkpoint --image-path "$dir/source/image" \
		--shell-job --tcp-established --parent-path ../parent0 "$ID" >"$dir/final-dump.log" 2>&1
	final_bytes=$(sum_images "$dir/source/image" 'pages-*.img')
	if [ "$variant" = candidate ]; then
		test "$coverage" -gt 0
		test "$final_bytes" -gt 0
		test $((final_bytes * 4)) -lt "$pre_bytes"
	else
		test "$coverage" -eq 0
		test $((final_bytes * 2)) -ge "$pre_bytes"
	fi
	# Same relative names at each endpoint preserve the final parent symlink.
	cp -a "$dir/source/image/." "$dir/destination/image/"
	test "$(readlink "$dir/destination/image/parent")" = ../parent0
	"$RUNC" --root "$DST_ROOT" restore -d --bundle "$dir/bundle" \
		--image-path "$dir/destination/image" --work-path "$dir/destination/image" \
		--shell-job --tcp-established "$ID" </dev/null >/dev/null 2>"$dir/restore.log"
	"$RUNC" --root "$DST_ROOT" state "$ID" > "$dir/restored-state.json"
	# Independent host-side byte oracle, before the restored workload reports PASS.
	python3 - "$dir" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
pid = json.loads((p / 'restored-state.json').read_text())['pid']
address, length = (p / 'state/address').read_text().split()
length = int(length)
assert length == 32 * 1024 * 1024
with open(f'/proc/{pid}/mem', 'rb', buffering=0) as mem:
    mem.seek(int(address, 16))
    actual = mem.read(length)
assert len(actual) == length
expected = bytearray((i * 17 + (i >> 12) * 31 + 73) % 251 for i in range(length))
for i in range(4096 * 101, 4096 * 102):
    expected[i] ^= 0x5a
assert actual == expected, 'external restored-memory oracle failed'
(p / 'external-oracle.txt').write_text(f'PASS {length} restored bytes including changed page\n')
PY
	"$RUNC" --root "$DST_ROOT" kill "$ID" USR2
	wait_file "$dir/state/result"
	grep -qx 'PASS 33554432 bytes including changed page' "$dir/state/result"
	printf '{"variant":"%s","source_pages":%s,"coverage_bytes":%s,"pre_bytes":%s,"final_bytes":%s,"restored_bytes_verified":true,"external_oracle_verified":true}\n' \
		"$variant" "$source_pages" "$coverage" "$pre_bytes" "$final_bytes" | tee "$dir/result.json"
	cleanup
	SRC_ROOT= DST_ROOT= ID=
done
python3 - "$RESULTS" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
b, c = [json.loads((p / v / 'result.json').read_text()) for v in ('baseline', 'candidate')]
assert b['restored_bytes_verified'] and c['restored_bytes_verified']
assert b['external_oracle_verified'] and c['external_oracle_verified']
assert c['final_bytes'] * 4 < b['final_bytes']
print(f"RUNC SMOKE PASS: baseline final {b['final_bytes']} B, candidate final {c['final_bytes']} B; both memory oracles passed")
print('Single-host transport/topology smoke; not a migration throughput benchmark.')
PY
