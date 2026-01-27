#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
Usage: $0 --name <test-name> [--image-path <path>] [--variant <label>] [--repeats N] [--bundle <bundle>] [--bandwidth MBPS]

Run repeated checkpoint trials for a single image.

Options:
  --name       Test name (required)
  --image-path Path to the image directory to exercise (OCI-style; required to select a specific image)
  --variant    Configuration label for run directories (human-readable; does NOT select image)
  --repeats    Number of repeats (default: 3)
  --bundle     Bundle name or path (default: redis)
  --bandwidth  Transfer bandwidth in Mbps used for transfer_time estimation (default: 50)
  --out        Output base directory (default: /tmp/chk_runs)
EOF
  exit 1
}

NAME=""
VARIANT_NAME="baseline"
REPEATS=3
BUNDLE="redis"
BANDWIDTH=50
OUT_BASE="${OUT_BASE:-/tmp/chk_runs}"
# Ensure TMP_ROOT is defined to avoid 'unbound variable' failures when set -u is active
TMP_ROOT="${TMP_ROOT:-}"


while [ $# -gt 0 ]; do
  case "$1" in
    --name)
      NAME="$2"; shift 2;;
    --image-path)
      IMAGE_PATH_OVERRIDE="$2"; shift 2;;
    --variant)
      VARIANT_NAME="$2"; shift 2;;
    --repeats)
      REPEATS="$2"; shift 2;;
    --bundle)
      BUNDLE="$2"; shift 2;;
    --bandwidth)
      BANDWIDTH="$2"; shift 2;;

    --out)
      OUT_BASE="$2"; shift 2;;
    -h|--help)
      usage;;
    *)
      echo "Unknown arg: $1"; usage;;
  esac
done

# Warn if a configuration label was provided but no explicit image path was supplied
if [ -n "$VARIANT_NAME" ] && [ -z "${IMAGE_PATH_OVERRIDE:-}" ]; then
  echo "WARNING: configuration label provided (--variant) but no --image-path specified; label will NOT select image—specify --image-path to control which image is used" >&2
fi

if [ -z "$NAME" ]; then
  echo "Error: --name is required" >&2
  usage
fi

# Do not pre-create OUT_BASE here; only create it when we will move per-run results into it
# (When TEMP_ROOT is provided by callers like run_grid.py, $OUT_BASE isn't used).
# Auto-install dirty-track module if device missing (try to load module from repo)
if [ ! -c "/dev/dirty-track" ]; then
  echo "dirty-track device missing; attempting to load module from /runc/dirty-track/light-dt"
  MODULE_DIR="/runc/dirty-track/light-dt"
  MODULE_KO="$MODULE_DIR/dirty-track.ko"

  if command -v modprobe >/dev/null 2>&1; then
    modprobe dirty-track 2>/dev/null || true
  fi

  if [ ! -c "/dev/dirty-track" ] && [ -f "$MODULE_KO" ]; then
    if command -v insmod >/dev/null 2>&1; then
      insmod "$MODULE_KO" 2>/dev/null || true
    fi
  fi

  if [ ! -c "/dev/dirty-track" ] && [ -f "$MODULE_DIR/Makefile" ]; then
    echo "Building dirty-track module in $MODULE_DIR ..."
    (cd "$MODULE_DIR" && make) 2>/dev/null || true
    if [ -f "$MODULE_KO" ] && command -v insmod >/dev/null 2>&1; then
      insmod "$MODULE_KO" 2>/dev/null || true
    fi
  fi

  if [ ! -c "/dev/dirty-track" ]; then
    echo "Warning: failed to load dirty-track module; some variants may require it"
  else
    echo "dirty-track device is available"
  fi
fi

had_failures=0

for i in $(seq 1 "$REPEATS"); do
  # If caller provided TEMP_ROOT (eg. run_grid.py), respect it and do not override it.
  # When TEMP_ROOT is set externally, create a per-repeat subdir under it to avoid collisions
  EXTERN_TEMP=${TEMP_ROOT:-}
  if [ -n "$EXTERN_TEMP" ]; then
    # Derive image variant name from explicit IMAGE_VARIANT env var or fallback to IMAGE_PATH_OVERRIDE parent dir
    IMG="${IMAGE_VARIANT:-}"
    if [ -z "$IMG" ] && [ -n "${IMAGE_PATH_OVERRIDE:-}" ]; then
      IMG="$(basename "$(dirname "${IMAGE_PATH_OVERRIDE}")")"
    fi
    if [ -z "$IMG" ]; then
      IMG="unknown_image"
    fi
    VAR="${VARIANT_NAME:-$NAME}"
    # Use provided telegrid root as TEMP_ROOT; let checkpoint_run_impl.sh create the per-image/<variant>/run_<i> structure.
    mkdir -p "${EXTERN_TEMP}" || true
    export TEMP_ROOT="${EXTERN_TEMP}"
    TO_OUT_BASE=0
    # For user visibility, compute preview of the expected run directory
    PREVIEW_RUN_DIR="${TEMP_ROOT}/${IMG}/${VAR}/run_${i}"
  else
    TMP_ROOT="/tmp/chk_${NAME}_run_${i}_$$"
    mkdir -p "$TMP_ROOT"
    export TEMP_ROOT="$TMP_ROOT"
    TO_OUT_BASE=1
    PREVIEW_RUN_DIR="$TMP_ROOT"
  fi

  export VARIANT_NAME="$VARIANT_NAME"
  export SINGLE_VARIANT="$VARIANT_NAME"
  export TRANSFER_BANDWIDTH_MBPS="$BANDWIDTH"

  echo "Starting test $NAME variant=$VARIANT_NAME repeat=$i expected_run_dir=$PREVIEW_RUN_DIR"
  # Run checkpoint implementation and capture return value without aborting this wrapper script
  run_ret=0
  # Pass the repeat index as a run number so each invocation creates a distinct run_<N> directory
  if ! bash scripts/dirty-track/checkpoint_run_impl.sh "$BUNDLE" "$i"; then
    run_ret=$?
    echo "ERROR: checkpoint_run_impl.sh returned exit code $run_ret" >> "${TMP_ROOT:-$TEMP_ROOT}/run_impl_error.log" 2>/dev/null || true
    # Inspect per-run exitcodes under expected output dirs; if all are zero treat as success to avoid false positive wrapper failures.
    search_dir="${TMP_ROOT:-$TEMP_ROOT}"
    if [ "$TO_OUT_BASE" -eq 1 ]; then
      search_dir="$OUT_BASE/$NAME"
    fi
    all_ok=1
    if [ -d "$search_dir" ]; then
      # find exitcode files and examine them; if any non-zero or missing, consider wrapper failure real
      while IFS= read -r ecfile; do
        ec=$(tr -d ' \t\n\r' < "$ecfile" 2>/dev/null || echo "MISSING")
        if [ "$ec" != "0" ]; then
          all_ok=0
          break
        fi
      done < <(find "$search_dir" -type f -name 'exitcode.txt' 2>/dev/null || true)
    else
      all_ok=0
    fi
    if [ "$all_ok" -eq 1 ]; then
      echo "NOTICE: checkpoint_run_impl.sh returned non-zero ($run_ret) but all per-run exitcodes under $search_dir are 0; treating as success" >> "$TMP_ROOT/run_impl_error.log" 2>/dev/null || true
      run_ret=0
    fi
    if [ "$run_ret" -ne 0 ]; then
      had_failures=1
    fi
  fi

  # If we created a per-run TMP_ROOT (manual mode), move experiments into human-friendly OUT_BASE
  if [ "$TO_OUT_BASE" -eq 1 ]; then
    if [ -d "$TMP_ROOT/experiments" ]; then
      mv "$TMP_ROOT/experiments" "$OUT_BASE/$NAME/run_$i"
    else
      mkdir -p "$OUT_BASE/$NAME/run_$i"
      cp -a "$TMP_ROOT" "$OUT_BASE/$NAME/run_$i/tmp_root_copy" || true
    fi
  fi

  # Quick per-run summary: deterministic layout only (no heuristic detection).
  if [ "$TO_OUT_BASE" -eq 1 ]; then
    RUN_DIR="$OUT_BASE/$NAME/run_$i"
  else
    # Deterministic layout: TEMP_ROOT is the telegrid root and
    # checkpoint_run_impl.sh creates ${TEMP_ROOT}/${IMG}/${VAR}/run_${i}
    RUN_DIR="${PREVIEW_RUN_DIR}"
  fi
  echo "Run $i output: $RUN_DIR"
  # decisions.csv presence/summary deferred to later analysis (parse_dirtymap.py)

  echo "---"
done

# Aggregate summary
echo "Summary for $NAME (repeats=$REPEATS):"
# Prefer human-friendly OUT_BASE if present, otherwise search under TEMP_ROOT for run_* directories
if [ -d "$OUT_BASE/$NAME" ]; then
  # OUT_BASE mode: explicit per-run directories
  for run in "$OUT_BASE/$NAME"/run_*; do
    [ -d "$run" ] || continue
    echo "Repeat: $run"
  done
else
  # Telegrid/top-TEMP_ROOT mode: find run_* dirs under the telegrid root
  search_space="${TEMP_ROOT:-$TMP_ROOT}"
  found=0
  while IFS= read -r -d $'\0' run; do
    found=1
    echo "Repeat: $run"
  done < <(find "$search_space" -maxdepth 6 -type d -name 'run_*' -print0 2>/dev/null || true)
  if [ "$found" -eq 0 ]; then
    echo "No runs found under $search_space"
  fi
fi

if [ -d "$OUT_BASE/$NAME" ]; then
  echo "Results stored in $OUT_BASE/$NAME"
else
  echo "Results stored in ${TEMP_ROOT:-$TMP_ROOT}/experiments/${VARIANT_NAME:-$NAME}"
fi

# Cleanup: remove empty OUT_BASE/$NAME if it was never populated to avoid repository clutter
if [ -d "$OUT_BASE/$NAME" ]; then
  if ! ls -d "$OUT_BASE/$NAME"/run_* >/dev/null 2>&1; then
    echo "No repeats found under $OUT_BASE/$NAME; removing empty directory"
    rmdir "$OUT_BASE/$NAME" 2>/dev/null || true
    if [ -d "$OUT_BASE" ] && ! ls -A "$OUT_BASE" >/dev/null 2>&1; then
      rmdir "$OUT_BASE" 2>/dev/null || true
    fi
  fi
fi

# Exit non-zero if any repeat failed
if [ "${had_failures:-0}" -ne 0 ]; then
  echo "One or more repeats for $NAME failed; exiting with status 1"
  exit 1
fi