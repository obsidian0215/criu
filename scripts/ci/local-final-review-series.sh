#!/bin/bash
# Validate the review partition without creating commits or changing remote refs.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0="$ROOT"
CC=${CC:-gcc}
export CC HOSTCC=${HOSTCC:-$CC}
case "$CC" in gcc|clang) ;; *) echo "Unsupported compiler: $CC" >&2; exit 2 ;; esac
BASE=3e067bc2639b7f98e8bc64b30c05f60b4dd52df8
VALIDATED=67eb051014b788f16600782c86cdd92a644d1040
VALIDATED_TREE=f9946ae4a0bf9e5efde8e3d10af3942b10a0ab23
PATCH_ROOT="$ROOT/review/local-final"
EVIDENCE="$ROOT/.local-final-ci/review-series"
mkdir -p "$EVIDENCE"
WORK=$(mktemp -d /tmp/criu-review-series.XXXXXX)/source
# Test workloads may run under an unprivileged UID.
chmod 755 "$(dirname "$WORK")"
status=0
last_built=0
# Invoked by the EXIT trap below, including early failures.
# shellcheck disable=SC2329
finish() {
	local result=$?
	trap - EXIT
	# Preserve only test results, not the complete temporary checkout/build.
	if [ -d "$WORK/test/others/page-server-remote-parent" ]; then
		if ! (
			cd "$WORK"
			shopt -s nullglob
			paths=(test/others/page-server-remote-parent/remote-parent-regression.*
			       test/zdtm/static/compress_pages00.out*)
			if ((${#paths[@]})); then
				find "${paths[@]}" \( -type f -o -type l \) -print0 |
					tar --null --no-recursion -czf "$EVIDENCE/runtime-evidence.tar.gz" --files-from=-
			fi
		); then
			echo 'Failed to package review-series test evidence' >&2
			result=1
		fi
	fi
	printf '%s\n' "$result" > "$EVIDENCE/exit-code"
	exit "$result"
}
trap finish EXIT
exec > >(tee "$EVIDENCE/runner.log") 2>&1
JOBS=$(nproc)
[ "$JOBS" -le 8 ] || JOBS=8
stage() {
	local name=$1
	shift
	echo "===== $name ====="
	if "$@" 2>&1 | tee "$EVIDENCE/$name.log"; then
		printf '%s PASS\n' "$name" | tee -a "$EVIDENCE/stages.txt"
	else
		printf '%s FAIL\n' "$name" | tee -a "$EVIDENCE/stages.txt"
		status=1
		return 1
	fi
}
: > "$EVIDENCE/stages.txt"
(cd "$PATCH_ROOT" && sha256sum --check patches.sha256)
git rev-parse HEAD > "$EVIDENCE/validation-checkout-commit.txt"
git rev-parse "$BASE^{tree}" > "$EVIDENCE/base-tree.txt"
test "$(git rev-parse "$VALIDATED^{tree}")" = "$VALIDATED_TREE"
"$CC" --version > "$EVIDENCE/compiler.txt"
# Detached worktree, no synthetic commit identities or author certification.
git worktree add --detach "$WORK" "$BASE"
cd "$WORK"
patches=(0001-page-xfer-read-buffer.patch
         0002-local-final-core-and-unit-tests.patch
         0003-local-final-integration-regression.patch)
trees=(4b5de5a9911084fcdfcd777ffe4e6fc619bebb85
       515f7ec083ed5393bfa26ad368dc6b49ea931dc6
       a1346bc77bb96269e19fb74e219a8cec30ecf45d)
for i in 0 1 2; do
	n=$((i + 1))
	git apply --index --whitespace=nowarn "$PATCH_ROOT/${patches[$i]}"
	actual=$(git write-tree)
	test "$actual" = "${trees[$i]}"
	printf 'stage%s %s %s\n' "$n" "${patches[$i]}" "$actual" >> "$EVIDENCE/trees.txt"
	# Clean builds ensure no earlier object can hide a dependency/link error.
	if stage "stage$n-clean" make mrproper; then
		if stage "stage$n-build" make -j "$JOBS" CC="$CC" HOSTCC="$HOSTCC"; then
			sha256sum criu/criu > "$EVIDENCE/stage$n-binary.sha256"
			stage "stage$n-unittest" make unittest CC="$CC" HOSTCC="$HOSTCC" || true
			if [ "$n" -eq 3 ]; then
				last_built=1
			fi
		fi
	fi
	git diff --exit-code > "$EVIDENCE/stage$n-unexpected-worktree-diff.txt"
done
# Production and unit tests already match the fully validated candidate.
git diff --exit-code "$VALIDATED" "${trees[2]}" -- criu/ > "$EVIDENCE/production-unit-diff.txt"
# Replay only into an isolated index: validation support does not enter builds.
PROOF_INDEX="$EVIDENCE/reconstruction.index"
GIT_INDEX_FILE="$PROOF_INDEX" git read-tree "${trees[2]}"
GIT_INDEX_FILE="$PROOF_INDEX" git apply --cached --whitespace=nowarn \
	"$PATCH_ROOT/validation-only-supplement.patch"
reconstructed=$(GIT_INDEX_FILE="$PROOF_INDEX" git write-tree)
test "$reconstructed" = "$VALIDATED_TREE"
printf '%s\n' "$reconstructed" > "$EVIDENCE/reconstructed-validated-tree.txt"
printf 'tree-identity PASS\n' | tee -a "$EVIDENCE/stages.txt"
# Only the changed normal regression wrapper needs another runtime exercise.
# Read/shared/runc/baseline matrices passed on the byte-identical validated tree.
if [ "$CC" = gcc ] && [ "$last_built" -eq 1 ]; then
	if stage stage3-dirty-tracking ./criu/criu --no-default-config check --feature mem_dirty_track; then
		if stage stage3-workloads make -C test/zdtm -j "$JOBS" CC="$CC"; then
			stage stage3-splice-regression env KEEP_WORK_DIR=1 PRE_DUMP_MODE=splice \
				make -C test/others/page-server-remote-parent regression || true
		fi
	fi
fi
exit "$status"
