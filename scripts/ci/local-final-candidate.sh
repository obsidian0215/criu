#!/bin/bash
# Focused CRIU-level validation, run inside the privileged Alpine test image.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
# The host checkout is owned by the runner, while this container runs as root.
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0="$ROOT"
CC=${CC:-gcc}
export CC HOSTCC=${HOSTCC:-$CC}
case "$CC" in gcc|clang) ;; *) echo "Unsupported compiler: $CC" >&2; exit 2 ;; esac
BASELINE=3e067bc2639b7f98e8bc64b30c05f60b4dd52df8
EVIDENCE="$ROOT/.local-final-ci"
mkdir -p "$EVIDENCE"
exec > >(tee "$EVIDENCE/runner.log") 2>&1
trap 'printf "%s\n" "$?" > "$EVIDENCE/exit-code"' EXIT
JOBS=$(nproc)
[ "$JOBS" -le 8 ] || JOBS=8
status=0
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
git rev-parse HEAD > "$EVIDENCE/candidate-commit.txt"
git status --short > "$EVIDENCE/candidate-status-before-build.txt"
git rev-parse "$BASELINE^{commit}" > "$EVIDENCE/baseline-commit.txt"
uname -a > "$EVIDENCE/kernel.txt"
grep '^Cap' /proc/self/status > "$EVIDENCE/capabilities.txt"
"$CC" --version > "$EVIDENCE/compiler.txt"
stage candidate-build make -j "$JOBS" CC="$CC" HOSTCC="$HOSTCC" || exit 1
stage workloads-build make -C test/zdtm -j "$JOBS" CC="$CC" || exit 1
./criu/criu --version > "$EVIDENCE/candidate-version.txt"
sha256sum ./criu/criu > "$EVIDENCE/candidate-binary.sha256"
# Required functionality must run; lack of dirty tracking is a failure, not PASS/SKIP.
stage dirty-tracking ./criu/criu --no-default-config check --feature mem_dirty_track || exit 1
stage unittest make unittest CC="$CC" HOSTCC="$HOSTCC" || true
stage splice-regression env KEEP_WORK_DIR=1 PRE_DUMP_MODE=splice \
	make -C test/others/page-server-remote-parent regression || true
stage read-regression env KEEP_WORK_DIR=1 PRE_DUMP_MODE=read \
	make -C test/others/page-server-remote-parent regression || true
stage shared-regression make -C test/others/page-server-remote-parent shared-regression || true
if [ "$CC" = gcc ]; then
	# Archive the exact unmodified tree; do not patch the baseline to pass tests.
	BASE_DIR=$(mktemp -d /tmp/criu-local-final-baseline.XXXXXX)
	git archive "$BASELINE" | tar -x -C "$BASE_DIR"
	if stage baseline-build make -C "$BASE_DIR" -j "$JOBS" CC=gcc HOSTCC=gcc; then
		stage baseline-comparison bash test/others/page-server-remote-parent/compare-baseline.sh \
			"$BASE_DIR/criu/criu" "$ROOT/criu/criu" || true
		# Official Alpine package, installed only in the disposable CI container.
		if stage runc-install apk add --no-cache runc; then
			stage runc-smoke bash test/others/page-server-remote-parent/runc-smoke.sh \
				"$BASE_DIR/criu/criu" "$ROOT/criu/criu" "$EVIDENCE/runc-smoke" || true
		fi
	fi
fi
exit "$status"
