#!/usr/bin/env bash
set -euo pipefail
# Checkpoint run script: perform checkpoint iterations for a variant and collect artifacts
# This script performs the checkpoint phase (pre-dump + dump) on containers restored from images.
# It records decisions, deferred events, iteration metrics and supporting logs for analysis.

if [ "${BASH_SOURCE[0]}" != "$0" ]; then
  echo "ERROR: This script must be executed, not sourced." >&2
  return 1 2>/dev/null || exit 1
fi

TS=$(date -u +%Y%m%dT%H%M%SZ)
SCRIPT_PID=$$
SCRIPT_RANDOM=$RANDOM

EXP_ROOT="/tmp/exp"
# Determine BUNDLE early so EXP_DIR uses the container name when TEMP_ROOT is set
BUNDLE=${BUNDLE:-"/runc/containers/redis"}
if [ -n "${1:-}" ]; then
  if [ -d "$1" ]; then
    BUNDLE="$1"
  else
    BUNDLE="/runc/containers/$1"
  fi
fi
BUNDLE_NAME=$(basename "$BUNDLE")
# 如果由调用者设置TEMP_ROOT，使用之；否则使用EXP_ROOT
if [ -n "${TEMP_ROOT:-}" ]; then
  # Use TEMP_ROOT as the telegrid root; per-image subdirs are created per-run
  EXP_DIR="${TEMP_ROOT}"
else
  EXP_DIR="${EXP_ROOT}/run_${SCRIPT_PID}_${SCRIPT_RANDOM}"
fi
mkdir -p "$EXP_DIR"

# Optional: enable command tracing to help diagnose unexpected actions.
# Set DEBUG_COMMAND_TRACE=1 to record each executed command to EXP_DIR/command_trace.log (default 0).
DEBUG_COMMAND_TRACE=${DEBUG_COMMAND_TRACE:-0}
if [ "${DEBUG_COMMAND_TRACE:-0}" -eq 1 ]; then
  # Log each executed command with timestamp and PID for post-mortem analysis.
  trap 'echo "CMD: $(date -u +%FT%T%z) $$ $BASH_COMMAND" >> "$EXP_DIR/command_trace.log"' DEBUG
fi

# RUNC_CONF_VARIANT deprecated: prefer explicit image selection via IMAGE_PATH or IMAGE_PATH_OVERRIDE.
# Deprecated file 'variant.txt' is no longer written to avoid confusion with image selection.
touch "$EXP_DIR/created_containers.txt"

# Configuration
export RUNC_LOG_FORMAT=text
# BUNDLE and BUNDLE_NAME parsed earlier so EXP_DIR can reflect container name when TEMP_ROOT is set.

DEFAULT_IMAGE_PATH="$BUNDLE/migrate/image"
IMAGE_PATH="$DEFAULT_IMAGE_PATH"
if [ -n "${IMAGE_PATH_OVERRIDE:-}" ]; then
  IMAGE_PATH="$IMAGE_PATH_OVERRIDE"
else
  # 尝试使用 /root/chk_images 下的默认镜像目录
  case "$BUNDLE_NAME" in
    redis)
      ALT_IMAGE="/root/chk_images/redis_sensoragg_chk/image"
      ;;
    influxdb)
      ALT_IMAGE="/root/chk_images/influxdb_sensoragg_chk/image"
      ;;
    elasticsearch)
      ALT_IMAGE="/root/chk_images/elasticsearch_chk/image"
      ;;
    *)
      ALT_IMAGE=""
      ;;
  esac

  if { [ ! -d "$IMAGE_PATH" ] || [ ! -f "$IMAGE_PATH/descriptors.json" ]; } && [ -n "$ALT_IMAGE" ] && [ -f "$ALT_IMAGE/descriptors.json" ]; then
    IMAGE_PATH="$ALT_IMAGE"
  fi
fi

CONSOLE_SOCK="$BUNDLE/console.sock"

TIMEOUT_SEC=${TIMEOUT_SEC:-30}
# ROUNDS deprecated: single run per invocation (outer repeats are handled by chk_rst.sh)
old_rounds="${ROUNDS:-}"
if [ -n "$old_rounds" ] && [ "$old_rounds" -ne 1 ]; then
  echo "ERROR: ROUNDS is deprecated and not supported (set to ${old_rounds}). Use --repeats (chk_rst.sh) and PREDUMP_ITERS for control." >> "$EXP_DIR/control.log" 2>/dev/null || true
  echo "ERROR: ROUNDS is deprecated and not supported. Aborting." >&2
  exit 2
fi
ROUNDS=1
CLEANUP_WAIT_SEC=${CLEANUP_WAIT_SEC:-30}

# Temporary recvtty management (ensure console socket for detached restores)
TEMP_RECVTTY_ENABLED=${TEMP_RECVTTY_ENABLED:-1}
# [Obsidian0215] Use a persistent socket in /tmp to ensure shorter paths and longevity across restores
TEMP_RECVTTY_SOCKET=${TEMP_RECVTTY_SOCKET:-"/tmp/run_session_${SCRIPT_PID}.sock"}
TEMP_RECVTTY_PID=""
OWN_CONSOLE=0

# Dirty-map settings
DIRTY_TRACK_DEV="/dev/dirty-track"
# 使用简化的dirtymap目录
DIRTYMAP_DIR="/tmp/dirtymap"
mkdir -p "$DIRTYMAP_DIR"
if ! mountpoint -q "$DIRTYMAP_DIR"; then
  mount -t tmpfs -o size=512M tmpfs "$DIRTYMAP_DIR" || echo "Warning: Failed to mount tmpfs on $DIRTYMAP_DIR"
fi
USER_DT_TOOL="/runc/dirty-track/dirtymap/user-dirty-track.py"
SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Lowered default to be less likely to abort experiments while still providing protection
# Default threshold for skip-accuracy abort (percent). Can be overridden via environment.
SKIP_ACCURACY_ABORT_THRESHOLD=${SKIP_ACCURACY_ABORT_THRESHOLD:-80}

# Time to wait for workload warmup (seconds) before pre-dump (default 6s)
WORKLOAD_WARMUP_SEC=${WORKLOAD_WARMUP_SEC:-6}

# Migration workload (use scripts from /runc/dirty-track/experiment/migration)
WORKLOAD_MIGRATION_SCRIPT=${WORKLOAD_MIGRATION_SCRIPT:-"/runc/dirty-track/experiment/migration/redis/bench_video_cache.py"}
WORKLOAD_MIGRATION_DURATION=${WORKLOAD_MIGRATION_DURATION:-600}
WORKLOAD_MIGRATION_THREADS=${WORKLOAD_MIGRATION_THREADS:-4}
WORKLOAD_MIGRATION_FRAMERATE=${WORKLOAD_MIGRATION_FRAMERATE:-60}
WORKLOAD_MIGRATION_ARGS="${WORKLOAD_MIGRATION_ARGS:-""}"

# Dirty-track window tuning (affects snapshot interval and convergence)
# Transfer bandwidth used to estimate image transfer time (Mbps). Default 50 Mbps.
TRANSFER_BANDWIDTH_MBPS=${TRANSFER_BANDWIDTH_MBPS:-50}
PREDUMP_SLEEP_SEC=${PREDUMP_SLEEP_SEC:-0.5}  # fallback minimum sleep if transfer time cannot be determined
# Default behavior: use measured transfer_time_ms to determine inter-predump sleep. Set
# PREDUMP_USE_TRANSFER_TIME=0 to force using fixed PREDUMP_SLEEP_SEC to stabilize experiments.
PREDUMP_USE_TRANSFER_TIME=${PREDUMP_USE_TRANSFER_TIME:-1}
WARMUP_SEC=${WARMUP_SEC:-5}

# Checkpoint output settings: store latest checkpoints persistently
# Default to TEMP_ROOT (telegrid root) if provided so checkpoint artifacts live under the selected temp root
CHECKPOINT_OUTPUT_DIR="${CHECKPOINT_OUTPUT_DIR:-${TEMP_ROOT:-${EXP_ROOT}}/ckpt/${BUNDLE_NAME}_dump_${SCRIPT_PID}_${SCRIPT_RANDOM}}"

# Restore after checkpoint: enable restore from newly created checkpoint to measure restore time
RESTORE_AFTER_CHECKPOINT=${RESTORE_AFTER_CHECKPOINT:-1}
mkdir -p "$CHECKPOINT_OUTPUT_DIR"

# Enforce canonical bundle paths
if ! echo "$BUNDLE" | grep -qE '^/runc/containers(/|$)'; then
  echo "ERROR: BUNDLE path '$BUNDLE' is outside /runc/containers. Aborting." >> "$EXP_DIR/control.log" || true
  echo "Refusing to use external bundle path '$BUNDLE'." >&2
  exit 1
fi

# Verify image exists
if [ ! -d "$IMAGE_PATH" ] || [ ! -f "$IMAGE_PATH/descriptors.json" ]; then
  echo "ERROR: checkpoint image at '$IMAGE_PATH' missing or invalid (no descriptors.json)" >> "$EXP_DIR/control.log"
  echo "ERROR: checkpoint image at '$IMAGE_PATH' is missing or invalid; ensure you pass a valid OCI-style image." >&2
  exit 1
fi

CUR_CTID=""
on_sigint() {
  echo "Received SIGINT/SIGTERM, cleaning up..." >> "$EXP_DIR/control.log"
  if [ -n "$CUR_CTID" ]; then
    echo "Cleaning up container $CUR_CTID" >> "$EXP_DIR/control.log"
    # Try to stop possible workloads running inside the container (redis-benchmark removed)
    # (cd "$BUNDLE" && runc exec "$CUR_CTID" pkill -f redis-benchmark) >> "$EXP_DIR/control.log" 2>&1 || true
    (cd "$BUNDLE" && runc exec "$CUR_CTID" pkill -f "simple-mem") >> "$EXP_DIR/control.log" 2>&1 || true
    runc kill "$CUR_CTID" SIGKILL >> "$EXP_DIR/control.log" 2>&1 || true
    runc delete --force "$CUR_CTID" >> "$EXP_DIR/control.log" 2>&1 || true
  fi
  stop_temp_recvtty || true
  # Umount dirtymap on interrupt
  for mnt in $(mount | grep '/tmp/dirtymap' | command awk '{print $3}'); do
    umount "$mnt" 2>/dev/null || true
  done
  exit 130
}
trap on_sigint INT TERM
# Ensure recvtty is always stopped on script exit
trap "stop_temp_recvtty || true" EXIT

echo "EXP_DIR=$EXP_DIR" > "$EXP_DIR/README.txt"
date -u > "$EXP_DIR/start_time.txt"

# Helper: verify dirty-track module is loaded and configured
check_dirty_track() {
  # If the device node is missing try to auto-install the module from the repository
  if [ ! -c "$DIRTY_TRACK_DEV" ]; then
    echo "WARNING: dirty-track device $DIRTY_TRACK_DEV not found; attempting to (auto)install module" >> "$EXP_DIR/control.log"

    MODULE_DIR="/runc/dirty-track/light-dt"
    MODULE_KO="$MODULE_DIR/dirty-track.ko"

    # Try to modprobe by module name first (if available in modules path)
    if command -v modprobe >/dev/null 2>&1; then
      modprobe dirty-track >> "$EXP_DIR/control.log" 2>&1 || true
    fi

    # If still missing try insmod the bundled .ko
    if [ ! -c "$DIRTY_TRACK_DEV" ] && [ -f "$MODULE_KO" ]; then
      echo "INFO: attempting insmod $MODULE_KO" >> "$EXP_DIR/control.log"
      if command -v insmod >/dev/null 2>&1; then
        insmod "$MODULE_KO" >> "$EXP_DIR/control.log" 2>&1 || true
      fi
    fi

    # If still missing and Makefile present, try to build then insmod
    if [ ! -c "$DIRTY_TRACK_DEV" ] && [ -f "$MODULE_DIR/Makefile" ]; then
      echo "INFO: trying to build dirty-track module in $MODULE_DIR" >> "$EXP_DIR/control.log"
      if (cd "$MODULE_DIR" && make) >> "$EXP_DIR/control.log" 2>&1; then
        echo "INFO: build succeeded; attempting insmod $MODULE_KO" >> "$EXP_DIR/control.log"
        if command -v insmod >/dev/null 2>&1; then
          insmod "$MODULE_KO" >> "$EXP_DIR/control.log" 2>&1 || true
        fi
      else
        echo "WARNING: failed to build dirty-track module in $MODULE_DIR" >> "$EXP_DIR/control.log"
      fi
    fi

    # Give udev a moment to create the device node
    sleep 0.3

    if [ ! -c "$DIRTY_TRACK_DEV" ]; then
      echo "ERROR: dirty-track device $DIRTY_TRACK_DEV not found after attempted install; ensure dirty-track.ko is loaded" >> "$EXP_DIR/control.log"
      return 1
    else
      echo "INFO: dirty-track device $DIRTY_TRACK_DEV is now available" >> "$EXP_DIR/control.log"
    fi
  fi

  # Create dirtymap directory if needed
  mkdir -p "$DIRTYMAP_DIR"

  # Verify user-dirty-track.py exists
  if [ ! -f "$USER_DT_TOOL" ]; then
    echo "ERROR: user-dirty-track.py not found at $USER_DT_TOOL" >> "$EXP_DIR/control.log"
    return 1
  fi

  return 0
}

# Helper: setup dirty-track for a container checkpoint
setup_dirty_track() {
  local dirtymap_dir="$1"

  # Create directory if needed
  mkdir -p "$dirtymap_dir"

  # Use user-dirty-track.py to set dirtymap directory via ioctl
  python3 "$USER_DT_TOOL" set_path --path "$dirtymap_dir" >> "$EXP_DIR/control.log" 2>&1
  if [ $? -ne 0 ]; then
    echo "ERROR: failed to set dirty-map path via user-dirty-track.py" >> "$EXP_DIR/control.log"
     return 1
   fi
   return 0
 }


# Helper: read latest dirtymap duration in ms (from header)
get_latest_dirtymap_duration_ms() {
  local dirtymap_dir="$1"
  local latest_dm
  local duration_ns

  latest_dm=$(ls -t "$dirtymap_dir"/*.dirtymap 2>/dev/null | head -1)
  if [ -z "$latest_dm" ]; then
    return 1
  fi

  duration_ns=$(od -An -t u8 -N 8 "$latest_dm" 2>/dev/null | command awk '{print $1}')
  if [ -z "$duration_ns" ] || ! [[ "$duration_ns" =~ ^[0-9]+$ ]]; then
    return 1
  fi

  echo $((duration_ns / 1000000))
  return 0
}

# Helper: parse prediction stats from dump.log (current iteration only)
parse_pred_stats_from_log() {
  local dump_log="$1"

  if [ ! -f "$dump_log" ]; then
    echo "0 0 0 0.00"
    return 0
  fi

  awk '
    /\[ObsidianPred\]/ {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /predicted_total=/) {split($i,a,"="); t+=a[2]}
        else if ($i ~ /predicted_hit=/) {split($i,a,"="); h+=a[2]}
        else if ($i ~ /predicted_miss=/) {split($i,a,"="); m+=a[2]}
      }
    }
    END {acc=(t>0)?(h*100.0/t):0; printf "%d %d %d %.2f", t, h, m, acc}
  ' "$dump_log"
}

# Helper: parse deferred coverage stats from dump.log (sum of ObsidianDef)
parse_def_stats_from_log() {
  local dump_log="$1"

  if [ ! -f "$dump_log" ]; then
    echo "0"
    return 0
  fi

  awk '
    /\[ObsidianDef\]/ {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /deferred_total=/) {split($i,a,"="); d+=a[2]}
      }
    }
    END {printf "%d", d}
  ' "$dump_log"
}

# Helper: parse warm_list stats from dump.log (ObsidianWarmStats/ObsidianWarmIter)
# Returns: warm_total warm_hit warm_miss pruned decayed candidates promoted
parse_warm_stats_from_log() {
  local dump_log="$1"

  if [ ! -f "$dump_log" ]; then
    echo "0 0 0 0 0 0 0 0 0"
    return 0
  fi

  awk '
    /\[ObsidianWarmStats\]/ {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /warm_total=/) {split($i,a,"="); t+=a[2]}
        else if ($i ~ /warm_hit=/) {split($i,a,"="); h+=a[2]}
        else if ($i ~ /warm_miss=/) {split($i,a,"="); m+=a[2]}
        else if ($i ~ /pruned=/) {split($i,a,"="); p+=a[2]}
        else if ($i ~ /decayed=/) {split($i,a,"="); d+=a[2]}
      }
    }
    /\[ObsidianWarmIter\]/ {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /candidates=/) {split($i,a,"="); ca+=a[2]}
        else if ($i ~ /promoted=/) {split($i,a,"="); pm+=a[2]}
        else if ($i ~ /promoted_by_score=/) {split($i,a,"="); pb+=a[2]}
        else if ($i ~ /pruned=/) {split($i,a,"="); p+=a[2]}
        else if ($i ~ /pruned_by_score=/) {split($i,a,"="); pr+=a[2]}
        else if ($i ~ /decayed=/) {split($i,a,"="); d+=a[2]}
      }
    }
    END {printf "%d %d %d %d %d %d %d %d %d", (t+0),(h+0),(m+0),(p+0),(d+0),(ca+0),(pm+0),(pb+0),(pr+0)}
  ' "$dump_log"
}

# Helper: record per-iteration metrics to CSV
record_iter_metrics() {
  local stage="$1"
  local iter="$2"
  local work_dir="$3"
  local use_dirtymap="$4"
  local metrics_file="$5"

  local pages=0
  local duration=0
  local dt_ms=0
  local pred_total=0 pred_hit=0 pred_miss=0 pred_acc=0.00
  local def_total=0
  local warm_total=0 warm_hit=0 warm_miss=0 warm_pruned=0 warm_decayed=0 warm_candidates=0 warm_promoted=0 warm_promoted_by_score=0 warm_pruned_by_score=0

  if [ -f "$work_dir/pages_transferred.txt" ]; then
    pages=$(cat "$work_dir/pages_transferred.txt")
  fi
  if [ -f "$work_dir/duration_ms.txt" ]; then
    duration=$(cat "$work_dir/duration_ms.txt")
  fi
  if [ -f "$work_dir/dirty_track_duration_ms.txt" ]; then
    dt_ms=$(cat "$work_dir/dirty_track_duration_ms.txt")
  fi

  # Compute estimated transfer time based on produced image size and configured bandwidth
  local transfer_bytes=0
  local transfer_time_ms=0
  local bw_mbps=${TRANSFER_BANDWIDTH_MBPS:-50}

  if [ -f "$work_dir/total_size.txt" ]; then
    transfer_bytes=$(cat "$work_dir/total_size.txt")
  elif [ -f "$work_dir/dump_total_size.txt" ]; then
    transfer_bytes=$(cat "$work_dir/dump_total_size.txt")
  fi

  if [ "$transfer_bytes" -gt 0 ]; then
    transfer_time_ms=$(python3 - <<PY
size = int($transfer_bytes)
bw = float("""$bw_mbps""")
if bw <= 0:
    print(0)
else:
    bps = bw * 1000000.0 / 8.0
    ms = int(round(size / bps * 1000.0))
    print(ms)
PY
)
  fi

  echo "$transfer_bytes" > "$work_dir/transfer_bytes.txt"
  echo "$transfer_time_ms" > "$work_dir/transfer_time_ms.txt"
  echo "$bw_mbps" > "$work_dir/bandwidth_used.txt"

  if [ "$use_dirtymap" = "1" ]; then
    read -r pred_total pred_hit pred_miss pred_acc <<< "$(parse_pred_stats_from_log "$work_dir/dump.log")"
    def_total=$(parse_def_stats_from_log "$work_dir/dump.log")
    # Robust parsing: ensure we always have nine fields (fallback to zeros) so 'set -u' doesn't fail
    warm_out="$(parse_warm_stats_from_log "$work_dir/dump.log" 2>/dev/null || true)"
    if [ -z "${warm_out:-}" ]; then
      warm_out="0 0 0 0 0 0 0 0 0"
    fi
    read -r warm_total warm_hit warm_miss warm_pruned warm_decayed warm_candidates warm_promoted warm_promoted_by_score warm_pruned_by_score <<< "$warm_out"
    printf "predicted_total=%s\npredicted_hit=%s\npredicted_miss=%s\npredicted_accuracy=%s\n" \
      "$pred_total" "$pred_hit" "$pred_miss" "$pred_acc" > "$work_dir/prediction_stats.txt"
    echo "deferred_total=${def_total}" > "$work_dir/deferred_stats.txt"
    printf "warm_total=%s\nwarm_hit=%s\nwarm_miss=%s\nwarm_pruned=%s\nwarm_decayed=%s\nwarm_candidates=%s\nwarm_promoted=%s\nwarm_promoted_by_score=%s\nwarm_pruned_by_score=%s\n" \
      "$warm_total" "$warm_hit" "$warm_miss" "$warm_pruned" "$warm_decayed" "$warm_candidates" "$warm_promoted" "$warm_promoted_by_score" "$warm_pruned_by_score" > "$work_dir/warm_stats.txt"
  fi

  echo "$stage,$iter,$pages,$duration,$dt_ms,$transfer_bytes,$transfer_time_ms,$bw_mbps,$pred_total,$pred_hit,$pred_miss,$pred_acc,$def_total,$warm_total,$warm_hit,$warm_miss,$warm_pruned,$warm_decayed,$warm_candidates,$warm_promoted,$warm_promoted_by_score,$warm_pruned_by_score" >> "$metrics_file"
}

# Helper: analyze timing from a dump.log and append summary
analyze_dump_log_time() {
  local label="$1"
  local log_path="$2"
  local out_file="$3"

  if [ ! -f "$log_path" ]; then
    echo "$label: dump.log not found" >> "$out_file"
    return 0
  fi

  python3 - "$label" "$log_path" >> "$out_file" << 'PY'
import re
import sys

label = sys.argv[1]
path = sys.argv[2]
ts_all = []
ts_vma = []
ts_pagemap = []
ts_dump = None

with open(path, "r", errors="ignore") as f:
    for line in f:
        m = re.match(r"\((\d+\.\d+)\)", line)
        if not m:
            continue
        t = float(m.group(1))
        ts_all.append(t)
        if "Handling VMA" in line:
            ts_vma.append(t)
        if "Pagemap generated" in line:
            ts_pagemap.append(t)
        if "Dumping pages" in line:
            ts_dump = t

if not ts_all:
    print(f"{label}: no timestamps")
    sys.exit(0)

def span(seq):
    return (seq[-1] - seq[0]) if len(seq) > 1 else 0.0

total = ts_all[-1] - ts_all[0]
vma_span = span(ts_vma)
pagemap_span = span(ts_pagemap)
dump_to_end = (ts_all[-1] - ts_dump) if ts_dump is not None else 0.0

print(f"{label}: total={total:.6f}s vma_span={vma_span:.6f}s pagemap_span={pagemap_span:.6f}s dump_to_end={dump_to_end:.6f}s lines={len(ts_all)}")
# transfer info (if available)
import os
tfile = os.path.join(os.path.dirname(path), 'transfer_time_ms.txt')
bfile = os.path.join(os.path.dirname(path), 'transfer_bytes.txt')
if os.path.exists(tfile):
    try:
        with open(tfile) as f:
            tm = int(f.read().strip())
    except:
        tm = None
else:
    tm = None
if os.path.exists(bfile):
    try:
        with open(bfile) as f:
            tb = int(f.read().strip())
    except:
        tb = None
else:
    tb = None
if tm is not None:
    print(f"{label}: transfer_time_ms={tm}ms transfer_bytes={tb}B")
PY
}

start_temp_recvtty() {
  local run_log_dir="${1:-$EXP_DIR}"
  # Enforce recvtty enabled: temporary recvtty is required for detached restores
  if [ "${TEMP_RECVTTY_ENABLED:-1}" -ne 1 ]; then
    echo "Note: TEMP_RECVTTY_ENABLED set to ${TEMP_RECVTTY_ENABLED}; overriding to 1 (recvtty required)" >> "$EXP_DIR/control.log"
    TEMP_RECVTTY_ENABLED=1
  fi

  # Find recvtty binary (PATH first, then common locations)
  local RECVTTY_BIN=""
  if command -v recvtty >/dev/null 2>&1; then
    RECVTTY_BIN=$(command -v recvtty)
  elif [ -x "/root/go/bin/recvtty" ]; then
    RECVTTY_BIN="/root/go/bin/recvtty"
  elif [ -x "$HOME/go/bin/recvtty" ]; then
    RECVTTY_BIN="$HOME/go/bin/recvtty"
  elif [ -x "/usr/local/bin/recvtty" ]; then
    RECVTTY_BIN="/usr/local/bin/recvtty"
  fi
  if [ -z "$RECVTTY_BIN" ]; then
    echo "ERROR: recvtty not found in PATH or known locations; cannot start temporary recvtty" >> "$EXP_DIR/control.log"
    echo "recvtty not found; aborting." >&2
    return 1
  fi

  local recvtty_log="$run_log_dir/recvtty.log"

  # If socket exists, try to reuse an existing recvtty or remove stale socket
  if [ -e "$TEMP_RECVTTY_SOCKET" ]; then
    local owner_pids=""
    if command -v ss >/dev/null 2>&1; then
      owner_pids=$(ss -xlp 2>/dev/null | command awk -v s="$TEMP_RECVTTY_SOCKET" '$0 ~ s { if(match($0,/pid=[0-9]+/)) { pid=substr($0, RSTART+4, RLENGTH-4); if(pid!="") print pid }}' | tr '\n' ' ')
    elif command -v lsof >/dev/null 2>&1; then
      owner_pids=$(lsof -t -U "$TEMP_RECVTTY_SOCKET" 2>/dev/null || true)
    fi

    local stale_socket=0
    for p in $owner_pids; do
      if [ -n "$p" ] && [ -d "/proc/$p" ]; then
        comm=$(cat /proc/$p/comm 2>/dev/null || true)
        # If it's recvtty, try reusing (ensure it's accepting)
        if echo "$comm" | grep -qi '^recvtty$'; then
          if python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.settimeout(0.3); s.connect('$TEMP_RECVTTY_SOCKET'); s.close(); sys.exit(0)" >/dev/null 2>&1; then
            echo "start_temp_recvtty: reusing existing recvtty pid $p for socket $TEMP_RECVTTY_SOCKET" >> "$EXP_DIR/control.log"
            TEMP_RECVTTY_PID="$p"
            CONSOLE_SOCK="$TEMP_RECVTTY_SOCKET"
            OWN_CONSOLE=0
            echo "start_temp_recvtty: confirmed existing recvtty accepting connections" >> "$EXP_DIR/control.log"
            return 0
          else
            echo "start_temp_recvtty: existing recvtty pid $p not accepting connections; treating socket as stale" >> "$EXP_DIR/control.log"
            stale_socket=1
            break
          fi
        else
          # Not recvtty: if it's accepting connections, abort; otherwise treat as stale and remove socket
          if python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.settimeout(0.3); s.connect('$TEMP_RECVTTY_SOCKET'); s.close(); sys.exit(0)" >/dev/null 2>&1; then
            echo "start_temp_recvtty: socket $TEMP_RECVTTY_SOCKET is in use by non-recvtty process pid $p and is accepting; aborting" >> "$EXP_DIR/control.log"
            return 1
          else
            echo "start_temp_recvtty: non-recvtty pid $p appears not to be accepting; treating socket as stale" >> "$EXP_DIR/control.log"
            stale_socket=1
            break
          fi
        fi
      fi
    done

    if [ "$stale_socket" -eq 1 ] || [ -z "$owner_pids" ] || ! ss -xlp 2>/dev/null | grep -q -- "$TEMP_RECVTTY_SOCKET"; then
      rm -f "$TEMP_RECVTTY_SOCKET" >/dev/null 2>&1 || true
      echo "start_temp_recvtty: removed stale socket $TEMP_RECVTTY_SOCKET" >> "$EXP_DIR/control.log"
    fi

    # If socket still in use after checks, abort
    if [ -e "$TEMP_RECVTTY_SOCKET" ]; then
      echo "start_temp_recvtty: socket $TEMP_RECVTTY_SOCKET is still present after checks; aborting" >> "$EXP_DIR/control.log"
      return 1
    fi
  fi

  # Start recvtty detached and capture pid/log, with retries
  local start_attempts=${TEMP_RECVTTY_START_ATTEMPTS:-3}
  local wait_attempts=${TEMP_RECVTTY_WAIT_ATTEMPTS:-50}
  for sa in $(seq 1 "$start_attempts"); do
    setsid nohup "$RECVTTY_BIN" -m null "$TEMP_RECVTTY_SOCKET" >> "$recvtty_log" 2>&1 &
    sleep 0.1
    TEMP_RECVTTY_PID=$!
    echo "$TEMP_RECVTTY_PID" > "$run_log_dir/recvtty.pid" || true
    CONSOLE_SOCK="$TEMP_RECVTTY_SOCKET"
    OWN_CONSOLE=1

    for i in $(seq 1 "$wait_attempts"); do
      if [ -S "$TEMP_RECVTTY_SOCKET" ] && python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.settimeout(0.3); s.connect('$TEMP_RECVTTY_SOCKET'); s.close(); sys.exit(0)" >/dev/null 2>&1; then
        # discover actual owner pid (in case $! isn't the recvtty)
        local owner_pid
        if command -v ss >/dev/null 2>&1; then
          owner_pid=$(ss -xlp 2>/dev/null | command awk -v s="$TEMP_RECVTTY_SOCKET" '$0 ~ s { if(match($0,/pid=[0-9]+/)) { pid=substr($0, RSTART+4, RLENGTH-4); if(pid!="") print pid }}' | head -n1)
        elif command -v lsof >/dev/null 2>&1; then
          owner_pid=$(lsof -t -U "$TEMP_RECVTTY_SOCKET" 2>/dev/null | head -n1 || true)
        fi
        if [ -n "$owner_pid" ]; then
          TEMP_RECVTTY_PID="$owner_pid"
          echo "$TEMP_RECVTTY_PID" > "$run_log_dir/recvtty.pid" || true
        fi
        echo "start_temp_recvtty: Temp recvtty listening on $CONSOLE_SOCK pid=$TEMP_RECVTTY_PID" >> "$EXP_DIR/control.log"
        return 0
      fi
      sleep 0.2
    done

    echo "start_temp_recvtty: attempt $sa/$start_attempts failed to start recvtty (see $recvtty_log); retrying" >> "$EXP_DIR/control.log"
  done

  echo "ERROR: recvtty failed to start or accept connections within timeout (see $recvtty_log)" >> "$EXP_DIR/control.log"
  cat "$recvtty_log" >> "$EXP_DIR/control.log" 2>/dev/null || true
  return 1
 }

 stop_temp_recvtty() {
  echo "stop_temp_recvtty: stopping recvtty (socket=${TEMP_RECVTTY_SOCKET:-}, pid=${TEMP_RECVTTY_PID:-})" >> "$EXP_DIR/control.log"

  # If we have a recorded PID, try to stop it gracefully only if we started it
  if [ -n "${TEMP_RECVTTY_PID:-}" ]; then
    if ! [[ "${TEMP_RECVTTY_PID}" =~ ^[0-9]+$ ]] || [ "${TEMP_RECVTTY_PID}" -lt 2 ]; then
      echo "stop_temp_recvtty: refusing to kill suspicious TEMP_RECVTTY_PID='${TEMP_RECVTTY_PID}'" >> "$EXP_DIR/control.log"
    else
      if [ "${OWN_CONSOLE:-0}" -eq 1 ]; then
        if kill -0 "${TEMP_RECVTTY_PID}" >/dev/null 2>&1; then
          echo "stop_temp_recvtty: killing TEMP_RECVTTY_PID=${TEMP_RECVTTY_PID}" >> "$EXP_DIR/control.log"
          kill "${TEMP_RECVTTY_PID}" >/dev/null 2>&1 || true
          wait "${TEMP_RECVTTY_PID}" 2>/dev/null || true
          echo "stop_temp_recvtty: killed pid ${TEMP_RECVTTY_PID}" >> "$EXP_DIR/control.log"
        fi
      else
        echo "stop_temp_recvtty: TEMP_RECVTTY_PID=${TEMP_RECVTTY_PID} not owned by this script; leaving it running" >> "$EXP_DIR/control.log"
      fi
    fi
    TEMP_RECVTTY_PID=""
    OWN_CONSOLE=0
  fi

  # If socket exists, try to find and stop processes using it (lsof/ss/fuser fallback)
  if [ -n "${TEMP_RECVTTY_SOCKET:-}" ] && [ -S "${TEMP_RECVTTY_SOCKET}" ]; then
    echo "stop_temp_recvtty: socket ${TEMP_RECVTTY_SOCKET} exists, attempting to clean owners" >> "$EXP_DIR/control.log"
    # Prefer ss to find recvtty-specific owners first (safer than blindly killing all owners returned by lsof)
    if command -v ss >/dev/null 2>&1; then
      recv_pids=$(ss -xlp 2>/dev/null | command awk -v s="${TEMP_RECVTTY_SOCKET}" '$0 ~ s && $0 ~ /recvtty/ { if(match($0,/pid=[0-9]+/)) { pid=substr($0, RSTART+4, RLENGTH-4); if (pid != "1") print pid }}' | tr '\n' ' ')
      if [ -n "$recv_pids" ]; then
        for p in $recv_pids; do
          if ! [[ "$p" =~ ^[0-9]+$ ]] || [ "$p" -lt 2 ]; then
            echo "stop_temp_recvtty: ss reported suspicious pid '$p', skipping" >> "$EXP_DIR/control.log"
            continue
          fi
          cmdline=$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null || true)
          comm=$(cat /proc/$p/comm 2>/dev/null || true)
          echo "stop_temp_recvtty: ss verified recvtty pid $p (cmd=$cmdline comm=$comm), killing" >> "$EXP_DIR/control.log"
          kill "$p" >/dev/null 2>&1 || true
          wait "$p" 2>/dev/null || true
        done
      fi
    fi

    # Fall back to lsof but filter by cmdline/comm and avoid mass-kills
    if command -v lsof >/dev/null 2>&1; then
      pids=$(lsof -t -U "${TEMP_RECVTTY_SOCKET}" 2>/dev/null || true)
      pid_count=$(echo "$pids" | wc -w)
      if [ "$pid_count" -gt 10 ]; then
        echo "stop_temp_recvtty: lsof reported ${pid_count} PIDs for ${TEMP_RECVTTY_SOCKET}; refusing to kill en masse. Manual investigation required." >> "$EXP_DIR/control.log"
      else
        for p in $pids; do
          if ! [[ "$p" =~ ^[0-9]+$ ]] || [ "$p" -lt 2 ]; then
            echo "stop_temp_recvtty: skipping suspicious pid '$p' from lsof for $TEMP_RECVTTY_SOCKET" >> "$EXP_DIR/control.log"
            continue
          fi
          cmdline=$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null || true)
          comm=$(cat /proc/$p/comm 2>/dev/null || true)
          if echo "$cmdline $comm" | grep -qiE 'recvtty|user-dirty-track|console'; then
            echo "stop_temp_recvtty: lsof verified owner pid $p (cmd=$cmdline comm=$comm), killing" >> "$EXP_DIR/control.log"
            kill "$p" >/dev/null 2>&1 || true
            wait "$p" 2>/dev/null || true
          else
            echo "stop_temp_recvtty: skipping non-owner pid $p (cmd=$cmdline comm=$comm)" >> "$EXP_DIR/control.log"
          fi
        done
      fi
    fi

    # fuser fallback: list PIDs but apply same verification logic and avoid direct fuser -k
    if command -v fuser >/dev/null 2>&1; then
      fuser_output=$(fuser "${TEMP_RECVTTY_SOCKET}" 2>/dev/null || true)
      for p in $(echo "$fuser_output" | sed 's/:/ /g' | command awk '{for(i=2;i<=NF;i++) print $i}'); do
        if ! [[ "$p" =~ ^[0-9]+$ ]] || [ "$p" -lt 2 ]; then
          echo "stop_temp_recvtty: skipping suspicious pid '$p' reported by fuser for $TEMP_RECVTTY_SOCKET" >> "$EXP_DIR/control.log"
          continue
        fi
        cmdline=$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null || true)
        comm=$(cat /proc/$p/comm 2>/dev/null || true)
        if echo "$cmdline $comm" | grep -qiE 'recvtty|user-dirty-track|console'; then
          echo "stop_temp_recvtty: fuser verified owner pid $p (cmd=$cmdline comm=$comm), killing" >> "$EXP_DIR/control.log"
          kill "$p" >/dev/null 2>&1 || true
          wait "$p" 2>/dev/null || true
        else
          echo "stop_temp_recvtty: skipping non-owner pid '$p' reported by fuser (cmd=$cmdline comm=$comm)" >> "$EXP_DIR/control.log"
        fi
      done
    fi

    # If no verified owners remain, and socket is not in use, remove it.
    if ! ss -xlp 2>/dev/null | grep -q -- "${TEMP_RECVTTY_SOCKET}"; then
      rm -f "${TEMP_RECVTTY_SOCKET}" >/dev/null 2>&1 || true
      echo "stop_temp_recvtty: removed socket ${TEMP_RECVTTY_SOCKET}" >> "$EXP_DIR/control.log"
    else
      echo "stop_temp_recvtty: verified owners remain; leaving socket ${TEMP_RECVTTY_SOCKET} in place for manual cleanup" >> "$EXP_DIR/control.log"
    fi
  fi

  # Remove stale console sockets older than 2 minutes (avoid removing active ones)
  for s in /tmp/console_ab_*.sock; do
    [ -e "$s" ] || continue
    if find "$s" -mmin +2 -print -quit | grep -q .; then
      if command -v lsof >/dev/null 2>&1; then
        if [ -z "$(lsof -t -U "$s" 2>/dev/null || true)" ]; then
          rm -f "$s" >/dev/null 2>&1 || true
          echo "stop_temp_recvtty: removed stale socket $s" >> "$EXP_DIR/control.log"
        fi
      else
        if ! ss -xlp 2>/dev/null | grep -q -- "$s"; then
          rm -f "$s" >/dev/null 2>&1 || true
          echo "stop_temp_recvtty: removed stale socket $s (no ss/lsof)" >> "$EXP_DIR/control.log"
        fi
      fi
    fi
  done

  rm -f "$EXP_DIR/recvtty.pid" "$EXP_DIR/recvtty.log" 2>/dev/null || true
  TEMP_RECVTTY_PID=""
  OWN_CONSOLE=0
  echo "stop_temp_recvtty: done" >> "$EXP_DIR/control.log"
  }

# Start a workload inside the restored container (or run a custom command)
start_workload() {
  local ctid="$1"
  local run_dir="$2"

  # [Obsidian0215] Wait for Redis to be ready before starting workload
  echo "Waiting for Redis to be ready in $ctid before starting benchmark..." >> "$EXP_DIR/control.log"
  local redis_ready=false
  for i in $(seq 1 30); do
    if (cd "$BUNDLE" && runc exec "$ctid" redis-cli ping 2>/dev/null | grep -q "PONG"); then
      redis_ready=true
      break
    fi
    sleep 1
  done

  if [ "$redis_ready" != "true" ]; then
    echo "ERROR: Redis failed to start in $ctid within 30s" >> "$EXP_DIR/control.log"
    return 1
  fi
  echo "Redis is ready in $ctid, starting benchmark..." >> "$EXP_DIR/control.log"

  mkdir -p "$run_dir/workload"
  local pidfile="$run_dir/workload/workload.pid"
  local started_list="$run_dir/workload/started_workload.txt"
  # Remove any stale pidfile before starting to avoid reusing leftover PIDs (eg. '1')
  rm -f "$pidfile" || true
  echo "Starting migration workload for $ctid (pidfile=$pidfile)" >> "$EXP_DIR/control.log"

  # Launch migration workload (fixed behavior): run host migration script in container's network namespace
  if [ ! -f "$WORKLOAD_MIGRATION_SCRIPT" ]; then
    echo "ERROR: migration workload script $WORKLOAD_MIGRATION_SCRIPT not found" >> "$EXP_DIR/control.log"
    return 1
  fi
  if ! command -v nsenter >/dev/null 2>&1; then
    echo "ERROR: nsenter not found; cannot run migration workload" >> "$EXP_DIR/control.log"
    return 1
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found on host; cannot run migration workload" >> "$EXP_DIR/control.log"
    return 1
  fi

  # Determine container init PID (try init_process_pid then pid)
  init_pid=$(cd "$BUNDLE" && runc state "$ctid" 2>/dev/null | command awk -F: '/"init_process_pid"/ {gsub(/[^0-9]/,"",$2); print $2; exit} /"pid"/ {gsub(/[^0-9]/,"",$2); print $2; exit}')
  if [ -z "$init_pid" ]; then
    echo "ERROR: failed to determine init pid for $ctid" >> "$EXP_DIR/control.log"
    return 1
  fi

  # Get container type from BUNDLE (e.g., /runc/containers/redis -> redis)
  container_type=$(basename "$BUNDLE")

  # Build migration args based on container type
  if [ -n "$WORKLOAD_MIGRATION_ARGS" ]; then
    mig_args="$WORKLOAD_MIGRATION_ARGS"
    connect_args=""
  else
    case "$container_type" in
      redis|mongodb|memcached)
        mig_args="--threads $WORKLOAD_MIGRATION_THREADS --duration $WORKLOAD_MIGRATION_DURATION"
        connect_args="--redis-host 127.0.0.1 --redis-port 6379"
        ;;
      influxdb|timeseries)
        mig_args="--threads $WORKLOAD_MIGRATION_THREADS --duration $WORKLOAD_MIGRATION_DURATION"
        connect_args="--influx-url http://127.0.0.1:8181"
        ;;
      elasticsearch|opensearch)
        mig_args="--threads $WORKLOAD_MIGRATION_THREADS --duration $WORKLOAD_MIGRATION_DURATION --rps 1000 --bulk-size 500"
        connect_args="--es-host 127.0.0.1 --es-port 9200"
        ;;
      nginx|apache|web)
        mig_args="--threads $WORKLOAD_MIGRATION_THREADS --duration $WORKLOAD_MIGRATION_DURATION --connections 100"
        connect_args=""
        ;;
      *)
        mig_args="--threads $WORKLOAD_MIGRATION_THREADS --duration $WORKLOAD_MIGRATION_DURATION"
        connect_args="--redis-host 127.0.0.1 --redis-port 6379"
        ;;
    esac
  fi

  # Launch the bench script in the target container's network namespace
  nsenter --target "$init_pid" --net -- python3 "$WORKLOAD_MIGRATION_SCRIPT" $connect_args $mig_args >& "$run_dir/workload/workload.stdout" &
  bgpid=$!
  echo $bgpid > "$pidfile"
  echo "DEBUG: after nsenter: bgpid=$bgpid" >> "$EXP_DIR/control.log"
  if [ -f "$pidfile" ]; then
    pf_content=$(sed -n '1p' "$pidfile" 2>/dev/null || true)
    echo "DEBUG: after nsenter pidfile=$pidfile content='$pf_content' owner=$(stat -c '%U' "$pidfile" 2>/dev/null || true) mtime=$(stat -c '%y' "$pidfile" 2>/dev/null || true)" >> "$EXP_DIR/control.log"
  else
    echo "DEBUG: after nsenter pidfile=$pidfile MISSING" >> "$EXP_DIR/control.log"
  fi
  echo "${bgpid}|migration|$(date -u +%FT%T%z)|${WORKLOAD_MIGRATION_SCRIPT} $connect_args $mig_args" >> "$started_list" 2>/dev/null || true
  sleep "$WORKLOAD_WARMUP_SEC"


  # Wait for pidfile (common for all workloads)
  local wait_iters=$(( WORKLOAD_WARMUP_SEC * 5 ))
  local workload_pid=""
  for i in $(seq 1 $wait_iters); do
    if [ -s "$pidfile" ]; then
      workload_pid=$(cat "$pidfile" 2>/dev/null || true)
      if [ -n "$workload_pid" ]; then
        # Validate pid: must be numeric and >= 2 (never PID 1)
        if ! [[ "$workload_pid" =~ ^[0-9]+$ ]] || [ "$workload_pid" -lt 2 ]; then
          echo "ERROR: workload pidfile contains invalid pid '$workload_pid' (path=$pidfile), removing & treating as not started" >> "$EXP_DIR/control.log"
          rm -f "$pidfile" || true
          workload_pid=""
        else
          echo "Workload started (pid=$workload_pid)" >> "$EXP_DIR/control.log"
          break
        fi
      else
        echo "Warning: workload pidfile exists but is empty (path=$pidfile)" >> "$EXP_DIR/control.log"
        rm -f "$pidfile" || true
      fi
    fi
    sleep 0.2
  done
  if [ -z "$workload_pid" ]; then
    echo "ERROR: workload did not write a valid pid within ${WORKLOAD_WARMUP_SEC}s" >> "$EXP_DIR/control.log"
    return 1
  fi

  # Store workload PID for dirty-track (if needed)
  echo "$workload_pid" > "$run_dir/workload/workload_actual_pid.txt"
  # Record started workload (fixed: migration)
  workload_cmd_str="${WORKLOAD_MIGRATION_SCRIPT}"
  echo "${workload_pid}|migration|$(date -u +%FT%T%z)|${workload_cmd_str}" >> "$started_list" 2>/dev/null || true
  sleep "$WORKLOAD_WARMUP_SEC"


}

stop_workload() {
  local ctid="$1"
  local run_dir="$2"

  local pidfile="$run_dir/workload/workload.pid"
  local started_list="$run_dir/workload/started_workload.txt"
  if [ -f "$pidfile" ]; then
    local wpid
    wpid=$(cat "$pidfile" 2>/dev/null || true)
    if [ -n "$wpid" ]; then
      # Sanity check: pid must be numeric and >= 2 (never kill PID 1)
      if ! [[ "$wpid" =~ ^[0-9]+$ ]] || [ "$wpid" -lt 2 ]; then
        echo "Refusing to stop suspicious workload pid='$wpid' for $ctid (from $pidfile)" >> "$EXP_DIR/control.log"
        rm -f "$pidfile" || true
        return 1
      fi
      # Only stop PIDs we recorded as started by this run
      if [ -f "$started_list" ] && grep -q "^${wpid}|" "$started_list"; then
        echo "Stopping workload pid=$wpid for $ctid (killing) at $(date -u +%FT%T%z)" >> "$EXP_DIR/control.log"
      else
        echo "Refusing to stop unknown or untracked pid '$wpid' for $ctid (not in $started_list); skipping" >> "$EXP_DIR/control.log"
        rm -f "$pidfile" || true
        return 1
      fi
      kill "$wpid" >> "$EXP_DIR/control.log" 2>&1 || true
      sleep 0.1
      if kill -0 "$wpid" >/dev/null 2>&1; then
        kill -9 "$wpid" >> "$EXP_DIR/control.log" 2>&1 || true
      fi
      # remove the pid entry from started_list
      if [ -f "$started_list" ]; then
        grep -v "^${wpid}|" "$started_list" > "$started_list.tmp" 2>/dev/null || true
        mv "$started_list.tmp" "$started_list" 2>/dev/null || true
      fi
      rm -f "$pidfile" || true
      echo "Stopped workload pid=$wpid for $ctid" >> "$EXP_DIR/control.log"
      return 0
    fi
  fi

  # If no pidfile, but we have recorded started pids, attempt to stop them
  if [ -f "$started_list" ]; then
    while IFS='|' read -r spid stype stime scmd; do
      if ! [[ "$spid" =~ ^[0-9]+$ ]] || [ "$spid" -lt 2 ]; then
        echo "Skipping invalid recorded pid '$spid' in $started_list" >> "$EXP_DIR/control.log"
        continue
      fi
      if ps -p "$spid" >/dev/null 2>&1; then
        echo "Stopping recorded workload pid=$spid (type=$stype cmd=$scmd)" >> "$EXP_DIR/control.log"
        kill "$spid" >> "$EXP_DIR/control.log" 2>&1 || true
        sleep 0.1
        if kill -0 "$spid" >/dev/null 2>&1; then
          kill -9 "$spid" >> "$EXP_DIR/control.log" 2>&1 || true
        fi
      else
        echo "Recorded workload pid=$spid not running; removing entry" >> "$EXP_DIR/control.log"
      fi
    done < "$started_list"
    rm -f "$started_list" || true
    return 0
  fi

  # fallback: try to kill known workload processes inside the container (redis-benchmark removed)
  # (cd "$BUNDLE" && runc exec "$ctid" pkill -f redis-benchmark) >> "$EXP_DIR/control.log" 2>&1 || true
  (cd "$BUNDLE" && runc exec "$ctid" pkill -f "simple-mem") >> "$EXP_DIR/control.log" 2>&1 || true
}

# Helper: restore from a checkpoint and collect stats
restore_from_checkpoint() {
  local ctid="$1"
  local checkpoint_dir="$2"
  local log_dir="$3"
  local label="$4"  # e.g., "initial" or "post-dump"

  echo "Restoring $label: container $ctid from $checkpoint_dir" >> "$EXP_DIR/control.log"

  mkdir -p "$log_dir"

  local start_ms=$(date +%s%3N)

  # Use console socket if available
  if [ -n "${CONSOLE_SOCK:-}" ]; then
    SOCKET="$CONSOLE_SOCK"
  else
    SOCKET="$log_dir/console.sock"
    rm -f "$SOCKET" 2>/dev/null || true
  fi

  # Restore to collect detailed restore stats (via /etc/criu/runc.conf)
  (cd "$BUNDLE" && runc restore --tcp-established --console-socket "$SOCKET" --image-path "$checkpoint_dir" --work-path "$log_dir" --detach "$ctid") || \
    (echo "Warning: runc restore --detach with console socket failed; trying non-detach fallback" >> "$EXP_DIR/control.log" && (cd "$BUNDLE" && runc restore --tcp-established --image-path "$checkpoint_dir" --work-path "$log_dir" "$ctid"))
  local ret=$?
  local end_ms=$(date +%s%3N)
  local duration=$((end_ms - start_ms))

  if [ $ret -ne 0 ]; then
    echo "ERROR: $label restore failed with exit code $ret" >> "$EXP_DIR/control.log"
    return $ret
  fi

  # Wait for container to be running
  local poll_interval=0.5
  local start_wait=$(date +%s)
  while [ $(( $(date +%s) - start_wait )) -lt "$TIMEOUT_SEC" ]; do
    if runc state "$ctid" 2>/dev/null | grep -qE '"status"[[:space:]]*:[[:space:]]*"running"'; then
      echo "$label restore: container $ctid running after ${duration}ms" >> "$EXP_DIR/control.log"
      echo "$duration" > "$log_dir/restore_duration_ms.txt"

      # Wait for service to be responsive (critical before starting workload/dirty-track)
      local service_ready=false
      local bundle_name=$(basename "$BUNDLE")
      for i in $(seq 1 120); do
        case "$bundle_name" in
          redis)
            if (cd "$BUNDLE" && runc exec "$ctid" redis-cli ping 2>/dev/null | grep -q "PONG"); then
              service_ready=true
              break
            fi
            ;;
          influxdb)
            # Try common InfluxDB endpoints (v1/v2): /ping (8086) expects 204, /health may return JSON with "status"
            if (cd "$BUNDLE" && runc exec "$ctid" sh -lc "curl -s -o /dev/null -w '%{http_code}' --max-time 2 http://127.0.0.1:8086/ping" 2>/dev/null | grep -qE '^(200|204)$'); then
              service_ready=true
              break
            fi
            if (cd "$BUNDLE" && runc exec "$ctid" sh -lc "curl -s --max-time 2 http://127.0.0.1:8086/health" 2>/dev/null | grep -qiE '\"status\"|\"pass\"'); then
              service_ready=true
              break
            fi
            # Fallback: check port 8086 is listening
            if (cd "$BUNDLE" && runc exec "$ctid" nc -z 127.0.0.1 8086 2>/dev/null); then
              service_ready=true
              break
            fi
            ;;
          elasticsearch)
            if (cd "$BUNDLE" && runc exec "$ctid" curl -s http://localhost:9200/_cluster/health 2>/dev/null | grep -q 'status'); then
              service_ready=true
              break
            fi
            ;;
          *)
            # Default: assume ready after restore
            service_ready=true
            break
            ;;
        esac
        sleep 0.5
      done

      if [ "$service_ready" = true ]; then
        echo "Service ready in container $ctid" >> "$EXP_DIR/control.log"
      else
        echo "WARNING: Service may not be ready in container $ctid (readiness check timeout)" >> "$EXP_DIR/control.log"
        # [Obsidian0215] If readiness failed, try one more time with a direct network check for the expected service port
        case "$bundle_name" in
          redis) check_port=6379 ;;
          influxdb) check_port=8086 ;;
          elasticsearch) check_port=9200 ;;
          *) check_port=0 ;;
        esac
        if [ "$check_port" -ne 0 ]; then
          if ! (cd "$BUNDLE" && runc exec "$ctid" nc -z 127.0.0.1 "$check_port" 2>/dev/null); then
            echo "ERROR: ${bundle_name} port ${check_port} not even listening in $ctid; workload WILL fail." >> "$EXP_DIR/control.log"
          fi
        fi
      fi

      # Extract restore stats from restore.log (CRIU --display-stats output)
      if grep -q "Restore time:" "$log_dir/restore.log" 2>/dev/null; then
        grep -E "(Restore time|Forking time|Pages read time|Pages compare time|Pages copy time|Pages restored):" "$log_dir/restore.log" > "$log_dir/restore_stats.txt" 2>/dev/null || true
        echo "$label restore stats collected" >> "$EXP_DIR/control.log"
      fi

      return 0
    fi
    sleep $poll_interval
  done

  echo "ERROR: $label restore - container $ctid did not reach running state after ${TIMEOUT_SEC}s" >> "$EXP_DIR/control.log"
  return 1
}

restore_container() {
  local ctid="$1"
  local img_path="$2"
  local log_dir="$3"

  echo "Restoring container $ctid from $img_path" >> "$EXP_DIR/control.log"

  mkdir -p "$log_dir"

  local start_ms=$(date +%s%3N)
  # Determine which console socket to use: prefer managed $CONSOLE_SOCK if set; otherwise use per-run console under $log_dir
    if [ -n "${CONSOLE_SOCK:-}" ]; then
      SOCKET="$CONSOLE_SOCK"
    else
      SOCKET="$log_dir/console.sock"
      rm -f "$SOCKET" 2>/dev/null || true
    fi

    # Try detach with console socket; if it fails, fallback to non-detach and log the event.
    (cd "$BUNDLE" && runc restore --tcp-established --console-socket "$SOCKET" --image-path "$img_path" --work-path "$log_dir" --detach "$ctid") || \
      ( echo "Warning: runc restore --detach with console socket failed; trying non-detach fallback" >> "$EXP_DIR/control.log" && (cd "$BUNDLE" && runc restore --tcp-established --image-path "$img_path" --work-path "$log_dir" "$ctid") )
    local ret=$?
    local end_ms=$(date +%s%3N)
    local duration=$((end_ms - start_ms))

    if [ $ret -ne 0 ]; then
      echo "ERROR: restore failed with exit code $ret" >> "$EXP_DIR/control.log"
      return $ret
    fi

  # Wait for container to be running (poll until TIMEOUT_SEC)
  local poll_interval=0.5
  local start_wait=$(date +%s)
  while [ $(( $(date +%s) - start_wait )) -lt "$TIMEOUT_SEC" ]; do
    if runc state "$ctid" 2>/dev/null | grep -qE '"status"[[:space:]]*:[[:space:]]*"running"'; then
      echo "Container $ctid restored and running after ${duration}ms" >> "$EXP_DIR/control.log"
      return 0
    fi
    sleep $poll_interval
  done

  echo "ERROR: container $ctid did not reach running state after ${TIMEOUT_SEC}s" >> "$EXP_DIR/control.log"
  echo "--- runc state output ---" >> "$EXP_DIR/control.log"
  runc state "$ctid" >> "$EXP_DIR/control.log" 2>&1 || true
  echo "--- runc list ---" >> "$EXP_DIR/control.log"
  runc list >> "$EXP_DIR/control.log" 2>&1 || true
  echo "--- tail of restore log ---" >> "$EXP_DIR/control.log"
  if [ -f "$log_dir/restore.log" ]; then
    tail -n 200 "$log_dir/restore.log" >> "$EXP_DIR/control.log" 2>&1 || true
  fi
  # collect dmesg for possible kernel errors
  echo "--- dmesg tail ---" >> "$EXP_DIR/control.log"
  dmesg | tail -n 200 >> "$EXP_DIR/control.log" 2>&1 || true

  return 1
}

# Helper: perform pre-dump checkpoint iteration
do_pre_dump() {
  local ctid="$1"
  local parent_dir="$2"
  local work_dir="$3"
  local use_dirtymap="$4"
  local dirtymap_dir="$5"
  local iter="$6"

  mkdir -p "$parent_dir"
  mkdir -p "$work_dir"

  local cmd="runc checkpoint"
  cmd+=" --pre-dump"
  cmd+=" --image-path $parent_dir"
  cmd+=" --work-path $work_dir"

  if [ "$use_dirtymap" = "1" ]; then
    cmd+=" --use-dirty-map --dirty-map-dir $dirtymap_dir"
  fi

  if [ "$iter" -gt 0 ]; then
    local prev_parent="$(dirname "$parent_dir")/parent_$((iter-1))"
    if [ -d "$prev_parent" ]; then
      cmd+=" --parent-path $prev_parent"
      # [Obsidian0215] Link parent for recursive image chain verification (using relative path)
      if [ ! -e "$parent_dir/parent" ]; then
        ln -s "../$(basename "$prev_parent")" "$parent_dir/parent"
      fi
    fi
  fi

  cmd+=" $ctid"

  echo "Running pre-dump iteration $iter: $cmd" >> "$EXP_DIR/control.log"

  # Note: dirty-track启动由首次predump前统一控制，这里不再手动start/stop
  # CRIU会在--use-dirty-map模式下自动管理dirty-track

  local start_ms=$(date +%s%3N)

  # construct optional parent arguments (use paths relative to the bundle directory)
  parent_opt=()
  if [ "$iter" -gt 0 ]; then
    local prev_parent="$(dirname "$parent_dir")/parent_$((iter-1))"
    if [ -d "$prev_parent" ]; then
      # Try to get a path relative to the bundle; runc requires --parent-path to be relative
      if command -v realpath >/dev/null 2>&1; then
        # Parent path should be relative to the image path (which for pre-dump is 'parent_dir')
        rel_prev_parent=$(realpath --relative-to="$parent_dir" "$prev_parent" 2>/dev/null || printf "%s" "$prev_parent")
      else
        rel_prev_parent="$prev_parent"
      fi
      parent_opt+=("--parent-path" "$rel_prev_parent")
      # [Obsidian0215] Link parent for recursive image chain verification (relative)
      if [ ! -e "$parent_dir/parent" ]; then
        ln -snf "$rel_prev_parent" "$parent_dir/parent"
      fi
    fi
  fi

  # run runc checkpoint directly (avoid eval quoting issues)
  if [ "$use_dirtymap" = "1" ]; then
    (cd "$BUNDLE" && runc checkpoint --file-locks --pre-dump --image-path "$parent_dir" --work-path "$work_dir" --use-dirty-map --dirty-map-dir "$dirtymap_dir" "${parent_opt[@]}" "$ctid")
    ret=$?
  else
    (cd "$BUNDLE" && runc checkpoint --file-locks --pre-dump --image-path "$parent_dir" --work-path "$work_dir" "${parent_opt[@]}" "$ctid")
    ret=$?
  fi

  local end_ms=$(date +%s%3N)
  local duration=$((end_ms - start_ms))

  if [ $ret -ne 0 ]; then
    echo "ERROR: pre-dump iteration $iter failed with exit code $ret" >> "$EXP_DIR/control.log"
    return $ret
  fi

  # Measure checkpoint image size
  local img_size=$(du -sb "$parent_dir" 2>/dev/null | command awk '{print $1}')
  local pages_size=$(find "$parent_dir" -name 'pages-*.img' -exec du -bc {} + 2>/dev/null | tail -1 | command awk '{print $1}')
  local pages_transferred=$((pages_size / 4096))

  echo "Pre-dump $iter: ${duration}ms, total=${img_size}B, pages=${pages_size}B (${pages_transferred} pages)" >> "$EXP_DIR/control.log"
  echo "$duration" > "$work_dir/duration_ms.txt"
  echo "$img_size" > "$work_dir/total_size.txt"
  echo "$pages_size" > "$work_dir/pages_size.txt"
  echo "$pages_transferred" > "$work_dir/pages_transferred.txt"

  if [ "$use_dirtymap" = "1" ]; then
    local dt_ms
    dt_ms=$(get_latest_dirtymap_duration_ms "$dirtymap_dir" 2>/dev/null || true)
    if [ -n "$dt_ms" ]; then
      echo "$dt_ms" > "$work_dir/dirty_track_duration_ms.txt"
      echo "Pre-dump $iter: dirty-track duration ${dt_ms}ms" >> "$EXP_DIR/control.log"
    fi
  fi

  # Log dirty-map artifacts for debugging (warm_list and dirtymap samples)
  if [ "$use_dirtymap" = "1" ]; then
    if [ -d "$dirtymap_dir" ]; then
      # Copy convergence metrics from predump for analysis
      for metrics_file in "$dirtymap_dir"/convergence_metrics.*; do
        [ -e "$metrics_file" ] && cp "$metrics_file" "$work_dir/convergence_metrics.predump_$iter.$(basename "$metrics_file" | cut -d. -f2-)"
      done

      # Preserve a copy of the entire dirtymap dir for post-mortem analysis
      cp -a "$dirtymap_dir" "$work_dir/dirtymap_backup_predump_$iter" 2>/dev/null || true

      # Create an index file mapping run-specific subdirs to dirtymap files so downstream
      # tooling can unambiguously find the correct dirtymap for a decision.
      if [ -d "$work_dir/dirtymap_backup_predump_$iter" ]; then
        python3 - "$work_dir/dirtymap_backup_predump_$iter" > "$work_dir/dirtymap_index_predump_$iter.json" <<'PY'
import os, json, sys
root = sys.argv[1]
idx = {}
for entry in sorted(os.listdir(root)):
    p = os.path.join(root, entry)
    if os.path.isdir(p) and entry.startswith('run_'):
        files = [f for f in sorted(os.listdir(p)) if f.endswith('.dirtymap')]
        if files:
            idx[entry] = files
# also list all dirtymap files under the backup dir for debugging
all_files = []
for r, dirs, files in os.walk(root):
    for f in files:
        if f.endswith('.dirtymap'):
            rel = os.path.relpath(r, root)
            all_files.append(os.path.join(rel, f))
json.dump({'predump_index': idx, 'all_dirtymaps': sorted(all_files)}, sys.stdout)
PY
        chmod 0644 "$work_dir/dirtymap_index_predump_$iter.json" 2>/dev/null || true
      fi

      echo "--- dirtymap contents after predump $iter ---" >> "$EXP_DIR/control.log"
      for f in "$dirtymap_dir"/warm_list.* "$dirtymap_dir"/*.dirtymap "$dirtymap_dir"/timestamp_list.* "$dirtymap_dir"/threshold.* "$dirtymap_dir"/convergence_metrics.*; do
        [ -e "$f" ] || continue
        echo "$(basename "$f") size=$(stat -c%s "$f")" >> "$EXP_DIR/control.log"
      done
      echo "--- end dirtymap contents ---" >> "$EXP_DIR/control.log"
    fi
  fi

  return 0
}

# Helper: perform final dump checkpoint
do_final_dump() {
  local ctid="$1"
  local image_dir="$2"
  local work_dir="$3"
  local use_dirtymap="$4"
  local dirtymap_dir="$5"
  local last_predump="$6"

  mkdir -p "$image_dir"
  mkdir -p "$work_dir"

  local cmd="runc checkpoint"
  cmd+=" --image-path $image_dir"
  cmd+=" --work-path $work_dir"

  if [ "$use_dirtymap" = "1" ]; then
    cmd+=" --use-dirty-map --dirty-map-dir $dirtymap_dir"
  fi

  if [ -n "$last_predump" ] && [ -d "$last_predump" ]; then
    cmd+=" --parent-path $last_predump"
  fi

  cmd+=" $ctid"

  echo "Running final dump: $cmd" >> "$EXP_DIR/control.log"

  # Note: dirty-track已在首次predump前启动，CRIU通过--use-dirty-map自动管理
  # 这里不再手动启动dirty-track

  local start_ms=$(date +%s%3N)

  parent_opt=()
  if [ -n "$last_predump" ] && [ -d "$last_predump" ]; then
    if command -v realpath >/dev/null 2>&1; then
      # For final dump, make the parent path relative to the image directory so runc resolves it correctly
      rel_last_parent=$(realpath --relative-to="$image_dir" "$last_predump" 2>/dev/null || printf "%s" "$last_predump")
    else
      rel_last_parent="$last_predump"
    fi
    parent_opt+=("--parent-path" "$rel_last_parent")
    # [Obsidian0215] Link parent for recursive image chain verification (using relative path)
    if [ ! -e "$image_dir/parent" ]; then
      ln -s "$rel_last_parent" "$image_dir/parent"
    fi
  fi

  # run runc checkpoint (final dump) directly to avoid eval splitting issues
  dump_log="$work_dir/dump.log"

  if [ "$use_dirtymap" = "1" ]; then
    (cd "$BUNDLE" && runc checkpoint --tcp-established --file-locks --image-path "$image_dir" --work-path "$work_dir" --use-dirty-map --dirty-map-dir "$dirtymap_dir" "${parent_opt[@]}" "$ctid")
    ret=$?
  else
    (cd "$BUNDLE" && runc checkpoint --tcp-established --file-locks --image-path "$image_dir" --work-path "$work_dir" "${parent_opt[@]}" "$ctid")
    ret=$?
  fi

  local end_ms=$(date +%s%3N)
  local duration=$((end_ms - start_ms))

  # If dump failed, check if it was due to connected TCP sockets and retry with --tcp-established
  if [ $ret -ne 0 ]; then
    if grep -q -E "Connected TCP socket|--tcp-established|consider using --tcp-established" "$dump_log" 2>/dev/null || grep -q "inet: Connected TCP socket" "$dump_log" 2>/dev/null; then
      echo "Detected connected TCP socket in dump.log; retrying final dump with --tcp-established" >> "$EXP_DIR/control.log"
      if [ "$use_dirtymap" = "1" ]; then
        (cd "$BUNDLE" && runc checkpoint --file-locks --tcp-established --image-path "$image_dir" --work-path "$work_dir" --use-dirty-map --dirty-map-dir "$dirtymap_dir" "${parent_opt[@]}" "$ctid")
        ret2=$?
      else
        (cd "$BUNDLE" && runc checkpoint --file-locks --tcp-established --image-path "$image_dir" --work-path "$work_dir" "${parent_opt[@]}" "$ctid")
        ret2=$?
      fi
      if [ $ret2 -eq 0 ]; then
        echo "Retry with --tcp-established succeeded" >> "$EXP_DIR/control.log"
        ret=0
      else
        echo "Retry with --tcp-established failed with exit code $ret2" >> "$EXP_DIR/control.log"
      fi
    fi
  fi

  if [ $ret -ne 0 ]; then
    echo "ERROR: final dump failed with exit code $ret" >> "$EXP_DIR/control.log"
    echo "DUMP_FAILED:CODE_${ret}" > "$run_dir/abort_reason.txt"
    echo "1" > "$run_dir/exitcode.txt"
    cleanup_checkpoint "$ctid" "$checkpoint_base" "${PRESERVE_ON_FAIL:-0}"
    return $ret
  fi

  # Measure checkpoint image size
  local img_size=$(du -sb "$image_dir" 2>/dev/null | command awk '{print $1}')
  local pages_size=$(find "$image_dir" -name 'pages-*.img' -exec du -bc {} + 2>/dev/null | tail -1 | command awk '{print $1}')
  local pages_transferred=$((pages_size / 4096))

  echo "Final dump: ${duration}ms, total=${img_size}B, pages=${pages_size}B (${pages_transferred} pages)" >> "$EXP_DIR/control.log"
  echo "$duration" > "$work_dir/duration_ms.txt"
  echo "$img_size" > "$work_dir/total_size.txt"
  echo "$img_size" > "$work_dir/dump_total_size.txt"
  echo "$pages_size" > "$work_dir/pages_size.txt"
  echo "$pages_transferred" > "$work_dir/pages_transferred.txt"

  if [ "$use_dirtymap" = "1" ]; then
    local dt_ms
    dt_ms=$(get_latest_dirtymap_duration_ms "$dirtymap_dir" 2>/dev/null || true)
    if [ -n "$dt_ms" ]; then
      echo "$dt_ms" > "$work_dir/dirty_track_duration_ms.txt"
      echo "Final dump: dirty-track duration ${dt_ms}ms" >> "$EXP_DIR/control.log"
    fi

    local pred_metrics
    pred_metrics=$(ls -t "$dirtymap_dir"/prediction_metrics.* 2>/dev/null | head -1)
    if [ -n "$pred_metrics" ]; then
      cp "$pred_metrics" "$work_dir/" 2>/dev/null || true
    fi
  fi

  # Copy convergence metrics to checkpoint directory for analysis
  if [ "$use_dirtymap" = "1" ]; then
    if [ -d "$dirtymap_dir" ]; then
      for metrics_file in "$dirtymap_dir"/convergence_metrics.*; do
        [ -e "$metrics_file" ] && cp "$metrics_file" "$work_dir/"
      done
    fi
  fi

  # Log dirty-map artifacts for debugging (warm_list and dirtymap samples)
  if [ "$use_dirtymap" = "1" ]; then
    if [ -d "$dirtymap_dir" ]; then
      echo "--- dirtymap contents after final dump ---" >> "$EXP_DIR/control.log"
      # Preserve final dirtymap snapshot for debugging
      cp -a "$dirtymap_dir" "$work_dir/dirtymap_backup_final" 2>/dev/null || true

      # Create index for final dirtymap snapshot
      if [ -d "$work_dir/dirtymap_backup_final" ]; then
        python3 - "$work_dir/dirtymap_backup_final" > "$work_dir/dirtymap_index_final.json" <<'PY'
import os, json, sys
root = sys.argv[1]
idx = {}
for entry in sorted(os.listdir(root)):
    p = os.path.join(root, entry)
    if os.path.isdir(p) and entry.startswith('run_'):
        files = [f for f in sorted(os.listdir(p)) if f.endswith('.dirtymap')]
        if files:
            idx[entry] = files
all_files = []
for r, dirs, files in os.walk(root):
    for f in files:
        if f.endswith('.dirtymap'):
            rel = os.path.relpath(r, root)
            all_files.append(os.path.join(rel, f))
json.dump({'predump_index': idx, 'all_dirtymaps': sorted(all_files)}, sys.stdout)
PY
        chmod 0644 "$work_dir/dirtymap_index_final.json" 2>/dev/null || true
      fi

      for f in "$dirtymap_dir"/warm_list.* "$dirtymap_dir"/*.dirtymap "$dirtymap_dir"/timestamp_list.* "$dirtymap_dir"/threshold.* "$dirtymap_dir"/convergence_metrics.*; do
        [ -e "$f" ] || continue
        echo "$(basename "$f") size=$(stat -c%s "$f")" >> "$EXP_DIR/control.log"
      done
      echo "--- end dirtymap contents ---" >> "$EXP_DIR/control.log"
    fi
  fi

  return 0
}

# Helper: cleanup container (preserve checkpoint artifacts only for failed runs)
cleanup_checkpoint() {
  local ctid="$1"
  local checkpoint_base="$2"
  local preserve_request="${3:-0}"

  echo "Cleaning up container $ctid" >> "$EXP_DIR/control.log"

  # Kill and delete container
  runc kill "$ctid" SIGKILL >> "$EXP_DIR/control.log" 2>&1 || true
  sleep 0.5
  runc delete --force "$ctid" >> "$EXP_DIR/control.log" 2>&1 || true

  # Decide whether to preserve checkpoint artifacts.
  # Rule: if caller explicitly requests preserve (preserve_request=1) -> preserve.
  # Otherwise, preserve only when run appears to have failed (exitcode != 0 or missing exitcode file).
  local run_dir
  run_dir=$(dirname "$checkpoint_base")
  local exitcode_file="$run_dir/exitcode.txt"
  local exitcode
  if [ -f "$exitcode_file" ]; then
    exitcode=$(tr -d ' \t\n\r' < "$exitcode_file" 2>/dev/null || echo "MISSING")
  else
    exitcode="MISSING"
  fi

  local preserve=0
  if [ "$preserve_request" -eq 1 ]; then
    preserve=1
  else
    if [ "$exitcode" = "MISSING" ]; then
      # missing exitcode -> treat as failure and preserve for debugging
      preserve=1
    else
      if [ "$exitcode" -ne 0 ]; then
        preserve=1
      else
        preserve=0
      fi
    fi
  fi

  if [ "$preserve" -eq 1 ]; then
    echo "Preserving checkpoint artifacts at $checkpoint_base (exitcode=${exitcode})" >> "$EXP_DIR/control.log"
  else
    echo "Removing checkpoint artifacts at $checkpoint_base (exitcode=${exitcode})" >> "$EXP_DIR/control.log"
    rm -rf "$checkpoint_base" 2>/dev/null || true
  fi

  # Ensure temp recvtty/socket is stopped to avoid stale sockets
  stop_temp_recvtty || true
  CUR_CTID=""
}

# Main checkpoint run routine
run_checkpoint_variant() {
  local variant_label="$1"
  local round="$2"
  # Resolved config label (human runtime label): prefer VARIANT_NAME env set by callers; fall back to passed variant_label
  local config_label="${VARIANT_NAME:-$variant_label}"

  # Determine image/service name for layout (derived from IMAGE_PATH); default to bundle if unknown
  image_variant="$(basename "$(dirname "${IMAGE_PATH:-$DEFAULT_IMAGE_PATH}")" 2>/dev/null || echo "${BUNDLE_NAME:-unknown}")"

  # Per-image directory under the telegrid TEMP_ROOT (or EXP_ROOT fallback).
  # We produce: <TEMP_ROOT>/<image_variant>/<config_label>/run_<N>
  local per_image_root="${TEMP_ROOT:-$EXP_ROOT}/${image_variant}"
  mkdir -p "$per_image_root"
  local EXP_DIR="$per_image_root"

  # Create run dir: <per_image_root>/<config_label>/run_$round
  local run_dir="$EXP_DIR/${config_label}/run_$round"
  mkdir -p "$run_dir"

  # Record run metadata early so post-processing can unambiguously associate this run
  # with the originating container, image variant and configured label.
  # Use the canonical container/bundle name (basename) for model keys and metadata.
  echo "$BUNDLE_NAME" > "$run_dir/bundle.txt" || true
  # Preserve the original bundle path when present for debugging/repro:$BUNDLE
  echo "$BUNDLE" > "$run_dir/bundle_path.txt" || true
  echo "${BUNDLE_NAME:-unknown}" > "$run_dir/container.txt" || true
  echo "$image_variant" > "$run_dir/image_variant.txt" || true
# Record resolved image path used for this run (for auditing)
echo "$IMAGE_PATH" > "$run_dir/image_path.txt" || true

# Record configuration label in run dir (config label is a human identifier and controls dirty-track enabling)
echo "$config_label" > "$run_dir/config_label.txt" || true
  # Only use a skip model when CRIU_SKIP_MODEL_FILE is explicitly provided by caller
  # If caller requested full decision telemetry, ensure sampling is low (sample=1)
  # so we record all decisions for bootstrap / dry-run diagnostics.
  if [ "${CRIU_DECISION_TELEMETRY:-}" = "2" ]; then
    : "${CRIU_DECISION_TELEMETRY_SAMPLE:=1}"
    export CRIU_DECISION_TELEMETRY_SAMPLE
  fi

  # Copy model into run dir for reproducibility if present
  if [ -n "${CRIU_SKIP_MODEL_FILE:-}" ] && [ -f "${CRIU_SKIP_MODEL_FILE}" ]; then
    cp -a "$CRIU_SKIP_MODEL_FILE" "$run_dir/skip_model.json" 2>/dev/null || true
    chmod 0644 "$run_dir/skip_model.json" 2>/dev/null || true
  fi

if [ -n "${TEMP_ROOT:-}" ]; then
        meta_entry=$(printf '{"bundle":"%s","bundle_path":"%s","container":"%s","image_variant":"%s","config_label":"%s","round":%s,"run_dir":"%s","start_time":"%s"}' \
          "$BUNDLE_NAME" "$BUNDLE" "$BUNDLE_NAME" "$image_variant" "$config_label" "$round" "$run_dir" "$(date -u +%Y-%m-%dT%H:%M:%SZ)")
        echo "$meta_entry" >> "${TEMP_ROOT}/run_meta.json" || true
      fi

  # sanitize variant name for use in ctid (replace slashes and spaces)
  local safe_variant_label
  safe_variant_label=$(echo "$config_label" | tr '/ ' '__')

  local ctid="run_${safe_variant_label}_${round}_${SCRIPT_PID}_${SCRIPT_RANDOM}"
  # use unique container id (ctid) for this run; do not reuse bundle name
  # local runc_ctid="$BUNDLE_NAME"
  CUR_CTID="$ctid"
  echo "$ctid" >> "$EXP_DIR/created_containers.txt"

  local checkpoint_base="$run_dir/checkpoint"
  local checkpoint_log="$run_dir/checkpoint_log"
  local initial_restore_log="$run_dir/initial_restore_log"
  local restore_log="$run_dir/restore_log"  # reserved for post-dump verify restore logs
  local iter_metrics_file="$run_dir/iteration_metrics.csv"
  local time_breakdown_file="$run_dir/time_breakdown.txt"
  mkdir -p "$checkpoint_base" "$checkpoint_log"

  # transfer_bytes: total image size in bytes, transfer_time_ms: estimated transfer time at configured bandwidth
  echo "stage,iter,pages_transferred,duration_ms,dirty_track_ms,transfer_bytes,transfer_time_ms,bandwidth_mbps,predicted_total,predicted_hit,predicted_miss,predicted_accuracy,deferred_total,warm_total,warm_hit,warm_miss,pruned_count,decayed_count,candidates_count,promoted_count,promoted_by_score,pruned_by_score" > "$iter_metrics_file"
  : > "$time_breakdown_file"

  # Ensure a temporary console listener is available for detached restores (required)
  # Use short path in /tmp to avoid Unix socket 108-byte path limit
  TEMP_RECVTTY_SOCKET="/tmp/console_${ctid}.sock"
  local recvtty_attempts=${TEMP_RECVTTY_ATTEMPTS:-3}
  local recvtty_ok=0
  for i in $(seq 1 "$recvtty_attempts"); do
    if start_temp_recvtty "$run_dir"; then
      recvtty_ok=1
      break
    fi
    echo "ERROR: start_temp_recvtty attempt $i/$recvtty_attempts failed for run $ctid" >> "$EXP_DIR/control.log"
    sleep 1
  done
  if [ "$recvtty_ok" -ne 1 ]; then
    echo "ERROR: failed to ensure temporary recvtty for run $ctid after ${recvtty_attempts} attempts; aborting." >> "$EXP_DIR/control.log"
    echo "RECVTTY_START_FAILED" > "$run_dir/abort_reason.txt"
    echo "1" > "$run_dir/exitcode.txt"
    cleanup_checkpoint "$ctid" "$checkpoint_base" 0
    return 1
  fi

  # Determine dirtymap settings based on explicit image/path or requested predump iters (variant concept removed).
  local use_dirtymap=0
  local dirtymap_dir="$DIRTYMAP_DIR/$ctid"
  mkdir -p "$dirtymap_dir"

  # Dirty-track enablement: baseline => no dirty-track; other labels => enable dirty-track
  if [ "$config_label" = "baseline" ]; then
    echo "INFO: config_label 'baseline' detected; skipping dirty-track setup" >> "$EXP_DIR/control.log"
  else
    if check_dirty_track; then
      use_dirtymap=1
      if ! setup_dirty_track "$dirtymap_dir"; then
        echo "ERROR: failed to setup dirty-track" >> "$EXP_DIR/control.log"
        echo "DIRTYTRACK_SETUP_FAILED" > "$run_dir/abort_reason.txt"
        echo "1" > "$run_dir/exitcode.txt"
        cleanup_checkpoint "$ctid" "$checkpoint_base" 0
        return 1
      fi
    else
      echo "ERROR: dirty-track device $DIRTY_TRACK_DEV not found after attempted install; aborting" >> "$EXP_DIR/control.log"
      echo "DIRTYTRACK_NOT_AVAILABLE" > "$run_dir/abort_reason.txt"
      echo "1" > "$run_dir/exitcode.txt"
      cleanup_checkpoint "$ctid" "$checkpoint_base" 0
      return 1
    fi
  fi

  # Step 1: Restore container from image (read-only, never modified) - "initial restore"
  restore_from_checkpoint "$ctid" "$IMAGE_PATH" "$initial_restore_log" "initial"
  ret=$?
  if [ $ret -ne 0 ]; then
    echo "ERROR: initial restore failed with exit code $ret" >> "$EXP_DIR/control.log"
    echo "INITIAL_RESTORE_FAILED:CODE_${ret}" > "$run_dir/abort_reason.txt"
    echo "1" > "$run_dir/exitcode.txt"
    cleanup_checkpoint "$ctid" "$checkpoint_base" 0
    return $ret
  fi

  echo "Initial restore logs in: $initial_restore_log" >> "$EXP_DIR/control.log"
  echo "Post-dump restore logs will be placed in: $restore_log" >> "$EXP_DIR/control.log"
  # Marker: indicate initial restore succeeded for this run
  touch "$run_dir/initial_restore_done" || true

  # Give container time to stabilize
  sleep 1

  # Start workload to generate sustained memory activity
  # 负载需要运行足够长时间以覆盖整个predump→dump流程
  local workload_pid=""
  echo "Starting workload for $ctid (will run throughout predump→dump)" >> "$EXP_DIR/control.log"
  if start_workload "$ctid" "$run_dir"; then
    workload_pid=$(cat "$run_dir/workload/workload_actual_pid.txt" 2>/dev/null || echo "")
    if [ -n "$workload_pid" ]; then
      echo "Workload process PID: $workload_pid" >> "$EXP_DIR/control.log"
    fi
    # Marker: indicate workload was started for this run
    touch "$run_dir/workload_started" || true
  else
    echo "Warning: workload startup failed" >> "$EXP_DIR/control.log"
  fi
  # Give a short warmup period to let workload stabilize
  sleep "${WORKLOAD_WARMUP_SEC}"
  echo "Workload warmup completed (waited ${WORKLOAD_WARMUP_SEC}s)" >> "$EXP_DIR/control.log"

  # Step 1.5: 如果使用dirty-map，在首次predump前启动dirty-track（仅此一次）
  # 之后的所有predump/dump操作中，CRIU会自动管理dirty-track的启停
  if [ "$use_dirtymap" -eq 1 ]; then
    # Clean up any orphaned tracking processes before starting new one
    echo "Cleaning up orphaned dirty-track processes" >> "$EXP_DIR/control.log"
    pkill -f "user-dirty-track" 2>/dev/null || true
    sleep 1


    # Verify container is still running before dirty-track
    echo "Checking container status before dirty-track start" >> "$EXP_DIR/control.log"
    if ! (cd "$BUNDLE" && runc state "$ctid" 2>&1 | grep -qE '"status"\s*:\s*"running"'); then
      echo "ERROR: container $ctid is not running (state check failed)" >> "$EXP_DIR/control.log"
      (cd "$BUNDLE" && runc state "$ctid") >> "$EXP_DIR/control.log" 2>&1 || true
      echo "1" > "$run_dir/exitcode.txt"
      cleanup_checkpoint "$ctid" "$checkpoint_base" 0
      return 1
    fi
    echo "Container $ctid verified running" >> "$EXP_DIR/control.log"

    echo "Starting dirty-track once before first predump (CRIU will manage thereafter)" >> "$EXP_DIR/control.log"

    python3 "$USER_DT_TOOL" start "$ctid" >> "$EXP_DIR/control.log" 2>&1
    ret=$?
    if [ $ret -ne 0 ]; then
      echo "ERROR: failed to start dirty-track before first predump (exit code $ret)" >> "$EXP_DIR/control.log"
      echo "1" > "$run_dir/exitcode.txt"
      cleanup_checkpoint "$ctid" "$checkpoint_base" 0
      return 1
    fi
  fi

  # Step 2: Perform pre-dump iterations (if configured)
  local predump_iters=${PREDUMP_ITERS:-8}
  # Adaptive predump tuning (simplified)
  # When dirtymap is enabled, DM-based stop policy is active by default; tune via PREDUMP_STOP_POLICY={aggressive|balanced|conservative}
  # and PREDUMP_DM_* vars (defaults shown below). This no longer requires PREDUMP_ADAPTIVE to be set.
  local predump_stop_policy=${PREDUMP_STOP_POLICY:-balanced}
  local predump_stop_consec=${PREDUMP_DM_STOP_CONSEC:-2}

  # Fixed stop policy (pages-based), independent of adaptive/DM (balanced defaults)
  local predump_fixed_stop=${PREDUMP_FIXED_STOP:-1}
  local predump_fixed_gain_ratio=0.02
  local predump_fixed_incr_ratio=1.10
  local predump_fixed_min_pages=2048

  # Zero-page retry threshold (independent from adaptive stop policy)
  local predump_zero_retry_min_pages=${PREDUMP_ZERO_RETRY_MIN_PAGES:-2048}

  # Dirtymap/deferred-based stop signals (only when dirtymap enabled)
  local predump_dm_enable=${PREDUMP_DM_ENABLE:-1}
  local predump_dm_min_pred_total
  local predump_dm_acc_converge
  local predump_dm_acc_diverge
  local predump_dm_def_growth_converge
  local predump_dm_def_growth_diverge
  case "$predump_stop_policy" in
    aggressive)
      predump_dm_min_pred_total=128
      predump_dm_acc_converge=60
      predump_dm_acc_diverge=50
      predump_dm_def_growth_converge=0.15
      predump_dm_def_growth_diverge=0.25
      ;;
    conservative)
      predump_dm_min_pred_total=512
      predump_dm_acc_converge=70
      predump_dm_acc_diverge=55
      predump_dm_def_growth_converge=0.08
      predump_dm_def_growth_diverge=0.25
      ;;
    *)
      predump_dm_min_pred_total=256
      predump_dm_acc_converge=65
      predump_dm_acc_diverge=55
      predump_dm_def_growth_converge=0.12
      predump_dm_def_growth_diverge=0.25
      ;;
  esac
  # Allow overrides via env
  predump_dm_min_pred_total=${PREDUMP_DM_MIN_PRED_TOTAL:-$predump_dm_min_pred_total}
  predump_dm_acc_converge=${PREDUMP_DM_ACC_CONVERGE:-$predump_dm_acc_converge}
  predump_dm_acc_diverge=${PREDUMP_DM_ACC_DIVERGE:-$predump_dm_acc_diverge}
  predump_dm_def_growth_converge=${PREDUMP_DM_DEF_GROWTH_CONVERGE:-$predump_dm_def_growth_converge}
  predump_dm_def_growth_diverge=${PREDUMP_DM_DEF_GROWTH_DIVERGE:-$predump_dm_def_growth_diverge}

  local total_predump_ms=0
  local last_parent=""
  local prev_predump_pages=0
  local prev2_predump_pages=0
  local prev_def_total=0
  local prev_pred_acc=0
  local dm_converge_count=0
  local dm_diverge_count=0
  local fixed_low_gain_count=0
  local fixed_increase_count=0
  local stopped_early=0
  local last_iter=-1
  local extra_final_predump=${EXTRA_FINAL_PREDUMP:-0}
  # When a predump iteration reports zero pages while the previous predump had many pages,
  # retry the predump up to this many times to avoid recording a spurious zero iteration.
  local predump_zero_retry=${PREDUMP_ZERO_RETRY:-2}

  # [Phase0] Warm-up: 利用Step 1.5已启动的dirty-track，等待足够长时间积累baseline
  # 关键：A和B运行时间一致，B利用这段时间生成baseline
  local warmup_duration=${WARMUP_SEC}

  if [ "$use_dirtymap" -eq 1 ] && [ -f "$USER_DT_TOOL" ]; then
    echo "=== Phase0 Warm-up (dirty-map enabled): Waiting ${warmup_duration}s for baseline accumulation ===" >> "$EXP_DIR/control.log"

    # Step 1.5已启动dirty-track，等待让它积累真实热度分布
    echo "Warm-up: waiting ${warmup_duration}s for dirty-track to accumulate baseline" >> "$EXP_DIR/control.log"
    sleep $warmup_duration

    # Stop生成baseline dirtymap（包含${warmup_duration}s的热度数据）
    echo "Warm-up: stopping dirty-track to generate baseline dirtymap" >> "$EXP_DIR/control.log"
    python3 "$USER_DT_TOOL" stop "$ctid" >> "$EXP_DIR/control.log" 2>&1 || {
      echo "Warning: stop failed in warm-up (exit=$?)" >> "$EXP_DIR/control.log"
    }
    sleep 0.2

    # 立即restart为pre-dump 0准备
    python3 "$USER_DT_TOOL" start "$ctid" >> "$EXP_DIR/control.log" 2>&1 || {
      echo "Warning: start failed in warm-up (exit=$?)" >> "$EXP_DIR/control.log"
    }

    echo "Warm-up complete: baseline dirtymap generated (${warmup_duration}s duration)" >> "$EXP_DIR/control.log"

    # 检查dirtymap文件数量
    local dm_count=$(ls "$dirtymap_dir"/*.dirtymap 2>/dev/null | wc -l)
    echo "Dirtymap files after warm-up: $dm_count" >> "$EXP_DIR/control.log"

    # 检查最新dirtymap的duration
    local latest_dm=$(ls -t "$dirtymap_dir"/*.dirtymap 2>/dev/null | head -1)
    if [ -n "$latest_dm" ]; then
      local duration_ns=$(od -An -t u8 -N 8 -j 16 "$latest_dm" 2>/dev/null | tr -d ' ')
      if echo "$duration_ns" | grep -qE '^[0-9]+$'; then
        local duration_ms
        duration_ms=$(python3 - << PY
ns = int("""$duration_ns""")
print(ns // 1000000)
PY
)
        echo "Latest dirtymap duration after warm-up: ${duration_ms}ms" >> "$EXP_DIR/control.log"
      fi
    fi
  else
    # No dirty-map: equalized wait to match timing
    echo "=== Phase0 Equalized wait (no dirty-map): Waiting ${warmup_duration}s to match timing ===" >> "$EXP_DIR/control.log"
    sleep $warmup_duration
    echo "Wait complete, proceeding to pre-dump" >> "$EXP_DIR/control.log"
  fi

  # Adaptive pre-dump loop (allows extension and adds confirm rounds for convergence)
  local iter=0
  local stopped_reason=""
  while [ "$iter" -lt "$predump_iters" ]; do
    local parent_dir="$checkpoint_base/parent_$iter"
    local work_dir="$checkpoint_log/predump_$iter"

    # Attempt pre-dump with retries on known mount-parse errors
    max_pre_dump_attempts=${MAX_PRE_DUMP_ATTEMPTS:-3}
    attempt=1
    pre_dump_ok=0
    while [ "$attempt" -le "$max_pre_dump_attempts" ]; do
      if do_pre_dump "$ctid" "$parent_dir" "$work_dir" "$use_dirtymap" "$dirtymap_dir" "$iter"; then
        pre_dump_ok=1
        break
      fi
      dump_log="$work_dir/dump.log"
      echo "Pre-dump attempt $attempt failed; see $dump_log" >> "$EXP_DIR/control.log"
      if [ "$attempt" -lt "$max_pre_dump_attempts" ]; then
        echo "Retrying pre-dump after short sleep..." >> "$EXP_DIR/control.log"
        rm -rf "$work_dir" || true
        mkdir -p "$work_dir"
        sleep 1
      fi
      attempt=$((attempt+1))
    done

    if [ "$pre_dump_ok" -ne 1 ]; then
      echo "ERROR: pre-dump iteration $iter failed after $max_pre_dump_attempts attempts" >> "$EXP_DIR/control.log"
      echo "1" > "$run_dir/exitcode.txt"
      echo "MOUNT_PARSE_FAILED_OR_PRE_DUMP_FAILED" > "$run_dir/abort_reason.txt"
      cleanup_checkpoint "$ctid" "$checkpoint_base" "${PRESERVE_ON_FAIL:-0}"
      return 1
    fi

    record_iter_metrics "predump" "$iter" "$work_dir" "$use_dirtymap" "$iter_metrics_file"

    # Retry logic: if current predump reported 0 pages but previous non-zero predump had many pages,
    # attempt a small number of retries to avoid recording an isolated 0 which can later push
    # activity to the final dump. This rewrites the iteration_metrics line for the iteration when retried.
    if [ "$use_dirtymap" = "1" ]; then
      cur_predump_pages=$(cat "$work_dir/pages_transferred.txt" 2>/dev/null || echo 0)
      if [ "$cur_predump_pages" -eq 0 ] && [ "$prev_predump_pages" -gt "$predump_zero_retry_min_pages" ]; then
        try=0
        while [ "$cur_predump_pages" -eq 0 ] && [ "$try" -lt "$predump_zero_retry" ]; do
          echo "Detected zero-page predump while previous predump had pages ($prev_predump_pages); retrying predump ($((try+1))/$predump_zero_retry)" >> "$EXP_DIR/control.log"
          # Remove previous work_dir contents and re-attempt pre-dump
          rm -rf "$work_dir" || true
          mkdir -p "$work_dir"
          if do_pre_dump "$ctid" "$parent_dir" "$work_dir" "$use_dirtymap" "$dirtymap_dir" "$iter"; then
            # Remove earlier iteration_metrics line for this iter and append the new one
            if [ -f "$iter_metrics_file" ]; then
              tmpfile="${iter_metrics_file}.tmp"
              grep -v ",$iter," "$iter_metrics_file" > "$tmpfile" || true
              mv "$tmpfile" "$iter_metrics_file" || true
            fi
            record_iter_metrics "predump" "$iter" "$work_dir" "$use_dirtymap" "$iter_metrics_file"
            cur_predump_pages=$(cat "$work_dir/pages_transferred.txt" 2>/dev/null || echo 0)
            # Small backoff before next potential retry
            sleep 1
          else
            echo "Retry pre-dump attempt $((try+1)) failed; continuing" >> "$EXP_DIR/control.log"
          fi
          try=$((try+1))
        done
      fi
    fi

    last_iter="$iter"

    # Copy all logs for analysis
    echo "Contents of $work_dir:" >> "$EXP_DIR/control.log"
    ls -R "$work_dir" >> "$EXP_DIR/control.log" 2>/dev/null || true

    if [ -f "$work_dir/duration_ms.txt" ]; then
      local dur=$(cat "$work_dir/duration_ms.txt")
      total_predump_ms=$((total_predump_ms + dur))
    fi

    last_parent="$parent_dir"

    # Check stopping condition (fixed pages policy + DM-adaptive signals)
    local cur_predump_pages
    cur_predump_pages=$(cat "$work_dir/pages_transferred.txt" 2>/dev/null || echo 0)

    if [ "$predump_fixed_stop" -eq 1 ] && [ "$cur_predump_pages" -gt 0 ]; then
      if [ "$prev_predump_pages" -gt 0 ]; then
        local ratio_gain_fixed
        ratio_gain_fixed=$(command awk -v prev="$prev_predump_pages" -v cur="$cur_predump_pages" 'BEGIN { if (prev > 0) printf "%.4f", (prev - cur) / prev; else print "0" }')
        echo "Iteration $iter: fixed_stop predump_pages=$cur_predump_pages gain_ratio=$ratio_gain_fixed" >> "$EXP_DIR/control.log"

        if [ "$cur_predump_pages" -le "$predump_fixed_min_pages" ]; then
          fixed_low_gain_count=$((fixed_low_gain_count + 1))
        elif command awk -v r="$ratio_gain_fixed" -v t="$predump_fixed_gain_ratio" 'BEGIN { if (r >= 0 && r < t) exit 0; else exit 1 }'; then
          fixed_low_gain_count=$((fixed_low_gain_count + 1))
        else
          fixed_low_gain_count=0
        fi

        if command awk -v prev="$prev_predump_pages" -v cur="$cur_predump_pages" -v thr="$predump_fixed_incr_ratio" 'BEGIN { exit !(cur > prev * thr) }'; then
          fixed_increase_count=$((fixed_increase_count + 1))
        else
          fixed_increase_count=0
        fi

        if [ "$fixed_increase_count" -ge "$predump_stop_consec" ]; then
          echo "Stopping pre-dump iterations: fixed policy divergence fixed_increase_count=$fixed_increase_count" >> "$EXP_DIR/control.log"
          stopped_early=1
          stopped_reason="FIXED_NON_CONVERGING"
          break
        fi

        if [ "$fixed_low_gain_count" -ge "$predump_stop_consec" ]; then
          echo "Stopping pre-dump iterations: fixed policy convergence fixed_low_gain_count=$fixed_low_gain_count" >> "$EXP_DIR/control.log"
          stopped_early=1
          stopped_reason="FIXED_CONVERGED"
          break
        fi
      fi
    fi

    if [ "$use_dirtymap" = "1" ] && [ "$predump_dm_enable" -eq 1 ] && [ "$cur_predump_pages" -gt 0 ]; then
      if [ "$prev_predump_pages" -gt 0 ]; then
        # dirtymap/deferred-based signals (DM-based stop is active when dirtymap enabled)
        dm_pred_valid=0
        if [ "$predump_dm_enable" -eq 1 ]; then
          local cur_pred_total=0
          local cur_pred_acc=0
          local cur_def_total=0
          local def_growth_ratio=0

          if [ -f "$work_dir/prediction_stats.txt" ]; then
            cur_pred_total=$(command awk -F= '/^predicted_total=/{print $2}' "$work_dir/prediction_stats.txt" | tail -n1)
            cur_pred_acc=$(command awk -F= '/^predicted_accuracy=/{print $2}' "$work_dir/prediction_stats.txt" | tail -n1)
          fi
          if [ -f "$work_dir/deferred_stats.txt" ]; then
            cur_def_total=$(command awk -F= '/^deferred_total=/{print $2}' "$work_dir/deferred_stats.txt" | tail -n1)
          fi

          cur_pred_total=${cur_pred_total:-0}
          cur_pred_acc=${cur_pred_acc:-0}
          cur_def_total=${cur_def_total:-0}

          if [ "$prev_def_total" -gt 0 ]; then
            def_growth_ratio=$(command awk -v prev="$prev_def_total" -v cur="$cur_def_total" 'BEGIN { if (prev > 0) printf "%.4f", (cur - prev) / prev; else print "0" }')
          else
            def_growth_ratio=0
          fi

          if [ "$cur_pred_total" -ge "$predump_dm_min_pred_total" ] && [ "$prev_def_total" -gt 0 ]; then
            dm_pred_valid=1
            if command awk -v acc="$cur_pred_acc" -v thr="$predump_dm_acc_converge" 'BEGIN { exit !(acc >= thr) }' && \
               command awk -v gr="$def_growth_ratio" -v thr="$predump_dm_def_growth_converge" 'BEGIN { exit !(gr <= thr) }'; then
              dm_converge_count=$((dm_converge_count + 1))
            else
              dm_converge_count=0
            fi

            if command awk -v acc="$cur_pred_acc" -v thr="$predump_dm_acc_diverge" 'BEGIN { exit !(acc <= thr) }' && \
               command awk -v gr="$def_growth_ratio" -v thr="$predump_dm_def_growth_diverge" 'BEGIN { exit !(gr >= thr) }'; then
              dm_diverge_count=$((dm_diverge_count + 1))
            else
              dm_diverge_count=0
            fi
          else
            dm_converge_count=0
            dm_diverge_count=0
          fi

          echo "Iteration $iter: deferred_total=$cur_def_total pred_total=$cur_pred_total pred_acc=${cur_pred_acc}% def_growth_ratio=$def_growth_ratio dm_converge_count=$dm_converge_count dm_diverge_count=$dm_diverge_count dm_pred_valid=$dm_pred_valid" >> "$EXP_DIR/control.log"
          prev_def_total=$cur_def_total
          prev_pred_acc=$cur_pred_acc
        fi

        if [ "$dm_pred_valid" -eq 1 ]; then
          if [ "$dm_diverge_count" -ge "$predump_stop_consec" ]; then
            echo "Stopping pre-dump iterations: dirtymap divergence confirmed dm_diverge_count=$dm_diverge_count" >> "$EXP_DIR/control.log"
            stopped_early=1
            stopped_reason="NON_CONVERGING_DIRTYMAP"
            break
          fi

          if [ "$dm_converge_count" -ge "$predump_stop_consec" ]; then
            echo "Stopping pre-dump iterations: dirtymap convergence confirmed dm_converge_count=$dm_converge_count" >> "$EXP_DIR/control.log"
            stopped_early=1
            stopped_reason="CONVERGED_DIRTYMAP"
            break
          fi
        else
          echo "DM-based signals not yet valid; continuing" >> "$EXP_DIR/control.log"
        fi
      fi
    fi

    # shift prev2 and prev for next iteration (still record for debug/consistency)
    if [ "$cur_predump_pages" -gt 0 ]; then
      prev2_predump_pages=$prev_predump_pages
      prev_predump_pages="$cur_predump_pages"
    fi
    if command -v python3 >/dev/null 2>&1 && [ -x "${SCRIPTS_DIR}/analyze_skip_accuracy_fast.py" ]; then
      sa=$(python3 "${SCRIPTS_DIR}/analyze_skip_accuracy_fast.py" "$EXP_DIR" "$config_label" "$round" 2>&1 | sed -n 's/.*Overall Accuracy: *\([0-9.][0-9.]*\)%.*/\1/p' | tail -n1)
      if [ -n "$sa" ]; then
        echo "Skip accuracy after iter $iter = ${sa}%" >> "$EXP_DIR/control.log"
        threshold=${SKIP_ACCURACY_ABORT_THRESHOLD:-95}
        cmp=$(awk -v a="$sa" -v t="$threshold" 'BEGIN{print (a+0 < t+0)?0:1}')
        if [ "$cmp" -eq 0 ]; then
          echo "SKIP_ACCURACY below threshold ($sa% < $threshold%); aborting further pre-dumps" >> "$EXP_DIR/control.log"
          echo "SKIP_ACCURACY_ABORT: $sa" > "$run_dir/abort_reason.txt"
          echo "SKIP_ACCURACY_ABORT $sa" > "$run_dir/predump_stop_reason.txt"
          cleanup_checkpoint "$ctid" "$checkpoint_base" 1
          return 1
        fi
      fi
    fi

    # Small pause between iterations: use transfer-time-based sleep when enabled, else fallback to PREDUMP_SLEEP_SEC
    if [ "${PREDUMP_USE_TRANSFER_TIME:-1}" -eq 1 ] && [ -f "$work_dir/transfer_time_ms.txt" ]; then
      sleep_ms=$(cat "$work_dir/transfer_time_ms.txt" 2>/dev/null || echo 0)
      min_sleep_ms=${PREDUMP_MIN_SLEEP_MS:-0}
      if [ "$sleep_ms" -lt "$min_sleep_ms" ]; then
        sleep_ms="$min_sleep_ms"
      fi
      transfer_bytes=$(cat "$work_dir/transfer_bytes.txt" 2>/dev/null || echo 0)
      bw_mbps=$(cat "$work_dir/bandwidth_used.txt" 2>/dev/null || echo "${TRANSFER_BANDWIDTH_MBPS:-50}")
      bw_MBps=$(awk -v mbps="$bw_mbps" 'BEGIN { if (mbps > 0) printf "%.3f", mbps/8.0; else print "0" }')
      # Convert ms to seconds with 3 decimal places
      sleep_sec=$(awk -v ms="$sleep_ms" 'BEGIN { printf "%.3f", ms/1000.0 }')
      echo "Sleeping for ${sleep_sec}s based on estimated transfer_time (${sleep_ms}ms, bytes=${transfer_bytes}, bw=${bw_mbps}Mbps≈${bw_MBps}MB/s)" >> "$EXP_DIR/control.log"
      sleep "$sleep_sec"
    else
      echo "Sleeping for ${PREDUMP_SLEEP_SEC}s (PREDUMP_USE_TRANSFER_TIME=${PREDUMP_USE_TRANSFER_TIME:-1})" >> "$EXP_DIR/control.log"
      sleep "$PREDUMP_SLEEP_SEC"
    fi

    iter=$((iter + 1))
  done

  # record stop reason when applicable
  if [ "$stopped_early" -eq 1 ]; then
    echo "$stopped_reason" > "$run_dir/predump_stop_reason.txt"
  fi

  # Optional: add one extra pre-dump right before final dump to reduce dump_pages
  if [ "$use_dirtymap" = "1" ] && [ "$extra_final_predump" -eq 1 ]; then
    if [ "$last_iter" -ge 0 ]; then
      local extra_iter=$((last_iter + 1))
      local parent_dir="$checkpoint_base/parent_${extra_iter}"
      local work_dir="$checkpoint_log/predump_${extra_iter}"

      echo "Running extra final pre-dump iteration $extra_iter" >> "$EXP_DIR/control.log"
      if ! do_pre_dump "$ctid" "$parent_dir" "$work_dir" "$use_dirtymap" "$dirtymap_dir" "$extra_iter"; then
        echo "1" > "$run_dir/exitcode.txt"
        cleanup_checkpoint "$ctid" "$checkpoint_base" "${PRESERVE_ON_FAIL:-0}"
        return 1
      fi

      record_iter_metrics "predump" "$extra_iter" "$work_dir" "$use_dirtymap" "$iter_metrics_file"

      echo "Contents of $work_dir:" >> "$EXP_DIR/control.log"
      ls -R "$work_dir" >> "$EXP_DIR/control.log" 2>/dev/null || true

      if [ -f "$work_dir/duration_ms.txt" ]; then
        local dur=$(cat "$work_dir/duration_ms.txt")
        total_predump_ms=$((total_predump_ms + dur))
      fi

      last_parent="$parent_dir"
    fi
  fi

  # Step 3: Perform final dump
  local image_dir="$checkpoint_base/image"
  local dump_work="$checkpoint_log/dump_log"

  if ! do_final_dump "$ctid" "$image_dir" "$dump_work" "$use_dirtymap" "$dirtymap_dir" "$last_parent"; then
    echo "1" > "$run_dir/exitcode.txt"
    # If PRESERVE_ON_FAIL is set, preserve checkpoint artifacts for debugging
    cleanup_checkpoint "$ctid" "$checkpoint_base" "${PRESERVE_ON_FAIL:-1}"  # Remove failed checkpoint unless PRESERVE_ON_FAIL=1
    return 1
  fi

  record_iter_metrics "dump" "final" "$dump_work" "$use_dirtymap" "$iter_metrics_file"

  # Summarize prediction stats across iterations (script-level accumulation)
  if [ "$use_dirtymap" = "1" ]; then
    # iteration_metrics.csv columns (per header):
    # 1:stage,2:iter,3:pages_transferred,4:duration_ms,5:dirty_track_ms,
    # 6:transfer_bytes,7:transfer_time_ms,8:bandwidth_mbps,9:predicted_total,10:predicted_hit,
    # 11:predicted_miss,12:predicted_accuracy,13:deferred_total,14:warm_total,15:warm_hit,
    # 16:warm_miss,17:pruned_count,18:decayed_count,19:candidates_count,20:promoted_count,21:promoted_by_score,22:pruned_by_score
    awk -F',' 'NR>1 {t+=$9; h+=$10; m+=$11; d+=$13; w+=$14; wh+=$15; wm+=$16; pr+=$17; dc+=$18; ca+=$19; pm+=$20; pbs+=$21; prbs+=$22} END {acc=(t>0)?(h*100.0/t):0; printf "predicted_total=%d\npredicted_hit=%d\npredicted_miss=%d\npredicted_accuracy=%.2f\ndeferred_total_sum=%d\nwarm_total_sum=%d\nwarm_hit_sum=%d\nwarm_miss_sum=%d\nwarm_pruned_sum=%d\nwarm_decayed_sum=%d\nwarm_candidate_sum=%d\nwarm_promoted_sum=%d\nwarm_promoted_by_score_sum=%d\nwarm_pruned_by_score_sum=%d\n", t, h, m, acc, d, w, wh, wm, pr, dc, ca, pm, pbs, prbs}' \
      "$iter_metrics_file" > "$run_dir/prediction_summary.txt"
  fi

  # Copy all logs for analysis
  echo "Contents of $dump_work:" >> "$EXP_DIR/control.log"
  ls -R "$dump_work" >> "$EXP_DIR/control.log" 2>/dev/null || true

  local dump_duration=0
  if [ -f "$dump_work/duration_ms.txt" ]; then
    dump_duration=$(cat "$dump_work/duration_ms.txt")
  fi

  # Step 4: Stop workload after dump completes
  stop_workload "$ctid" "$run_dir" || true

  # Note: dirty-track is automatically stopped by CRIU during final dump when using --use-dirty-map
  # Do NOT manually stop dirty-track; CRIU handles it
  if [ "$use_dirtymap" -eq 1 ]; then
    echo "dirty-track automatically stopped by CRIU during final dump" >> "$EXP_DIR/control.log"
  fi

  # Step 5: Record post-dump checkpoint availability
  # Perform actual restore from the newly created checkpoint to verify correctness and measure time
  local restore_from_new_ckpt_ms=0
  if [ "${RESTORE_AFTER_CHECKPOINT:-1}" -eq 1 ]; then
    echo "Performing restore from new checkpoint at $image_dir" >> "$EXP_DIR/control.log"
    local restore_ctid="${ctid}_restore_${SCRIPT_PID}_${SCRIPT_RANDOM}"
    local restore_work="$run_dir/restore_log"
    mkdir -p "$restore_work"

    local start_ms=$(date +%s%3N)
    # [Obsidian0215] Use the PERSISTENT console socket already started by start_temp_recvtty
    local verify_console="$TEMP_RECVTTY_SOCKET"

    echo "Post-dump restore logs to: $restore_work" >> "$EXP_DIR/control.log"
    (cd "$BUNDLE" && runc restore --tcp-established --console-socket "$verify_console" --detach --image-path "$image_dir" --work-path "$restore_work" "$restore_ctid")
    local ret=$?
    local end_ms=$(date +%s%3N)

    if [ $ret -eq 0 ]; then
      restore_from_new_ckpt_ms=$((end_ms - start_ms))
      echo "Post-dump restore succeeded in ${restore_from_new_ckpt_ms}ms" >> "$EXP_DIR/control.log"
      # Wait for container to be actually running to confirm health
      sleep 2
      if runc state "$restore_ctid" &>/dev/null; then
        echo "Post-dump restore health check passed" >> "$EXP_DIR/control.log"
        # [Obsidian0215] Perform application-level health check (Redis PING)
        local app_health=false
        case "$BUNDLE_NAME" in
          redis)
            # [Obsidian] Increased timeout to 20s and improved logging for post-restore health check
            echo "Performing Redis PING health check for $restore_ctid..." | tee -a "$EXP_DIR/control.log"
            # Debug info
            echo "Container state before check:" >> "$EXP_DIR/control.log"
            runc state "$restore_ctid" >> "$EXP_DIR/control.log" 2>&1 || true

            for try_idx in $(seq 1 6); do
              # [Obsidian] Use network-based check (nc) instead of runc exec to avoid "broken pipe" flakiness
              if (echo "PING" | nc -w 1 127.0.0.1 6379 2>/dev/null | grep -q "PONG"); then
                   app_health=true
                   break
              fi
              echo "  PING (via nc) attempt $try_idx failed, retrying..." | tee -a "$EXP_DIR/control.log"
              sleep 1
            done

            if [ "$app_health" = false ]; then
               echo "  Final attempt failed. Container state:" >> "$EXP_DIR/control.log"
               runc state "$restore_ctid" >> "$EXP_DIR/control.log" 2>&1 || true
               echo "  Container top:" >> "$EXP_DIR/control.log"
               runc top "$restore_ctid" >> "$EXP_DIR/control.log" 2>&1 || true
            fi
            ;;
          *)
            app_health=true # Assume OK for others for now
            ;;
        esac

        if [ "$app_health" = true ]; then
            echo "Post-dump app-level health check (PING) passed" >> "$EXP_DIR/control.log"
        else
            echo "Post-dump app-level health check (PING) FAILED!" >> "$EXP_DIR/control.log"
            # Record abort reason so analysis excludes this run
            echo "POST_DUMP_VERIFY_FAILED:REDIS_PING" > "$run_dir/abort_reason.txt"
            echo "1" > "$run_dir/exitcode.txt"
            ret=1
        fi
      else
        echo "Post-dump restore health check FAILED (container not running)" >> "$EXP_DIR/control.log"
        echo "POST_DUMP_RESTORE_NOT_RUNNING" > "$run_dir/abort_reason.txt"
        echo "1" > "$run_dir/exitcode.txt"
        ret=1
      fi
      # Cleanup restored container
      runc delete --force "$restore_ctid" >> "$EXP_DIR/control.log" 2>&1 || true
    else
      echo "ERROR: Post-dump restore failed with exit code $ret" >> "$EXP_DIR/control.log"
      # Mark this run as failed for analysis
      echo "POST_DUMP_RESTORE_FAILED:CODE_${ret}" > "$run_dir/abort_reason.txt"
      echo "1" > "$run_dir/exitcode.txt"
      echo "First 50 lines of restore log:" >> "$EXP_DIR/control.log"
      head -n 50 "$restore_work/restore.log" >> "$EXP_DIR/control.log"
      echo "Last 50 lines of cleaned restore log:" >> "$EXP_DIR/control.log"
      tail -n 50 "$restore_work/restore.log" >> "$EXP_DIR/control.log"
    fi

    if [ $ret -ne 0 ]; then
        echo "ERROR: run_checkpoint_variant failed due to restore failure or health check failure" >> "$EXP_DIR/control.log"
        return 1
    fi
  fi

  # Step 6: Calculate totals and collect page statistics
  local total_checkpoint_ms=$((total_predump_ms + dump_duration))

  # Collect page transfer statistics from each predump iteration
  local total_predump_pages=0
  for iter_dir in "$checkpoint_log"/predump_*; do
    if [ -f "$iter_dir/pages_transferred.txt" ]; then
      local iter_pages=$(cat "$iter_dir/pages_transferred.txt")
      total_predump_pages=$((total_predump_pages + iter_pages))
    fi
  done

  # Collect final dump page statistics
  local dump_pages=0
  if [ -f "$dump_work/pages_transferred.txt" ]; then
    dump_pages=$(cat "$dump_work/pages_transferred.txt")
  fi

  local total_pages=$((total_predump_pages + dump_pages))

  # Record predump iteration count for analysis
  local predump_count=0
  predump_count=$(ls -d "$checkpoint_log"/predump_* 2>/dev/null | wc -l | tr -d ' ')
  echo "$predump_count" > "$run_dir/predump_iter_count.txt"

  # Time breakdown: predump_0, predump_1, predump_last, final dump
  local predump_last_dir=""
  predump_last_dir=$(ls -d "$checkpoint_log"/predump_* 2>/dev/null | sort -V | tail -n 1 || true)
  echo "Config ${config_label} Round ${round} timing breakdown" >> "$time_breakdown_file"
  analyze_dump_log_time "${config_label}-predump_0" "$checkpoint_log/predump_0/dump.log" "$time_breakdown_file"
  analyze_dump_log_time "${config_label}-predump_1" "$checkpoint_log/predump_1/dump.log" "$time_breakdown_file"
  if [ -n "$predump_last_dir" ]; then
    analyze_dump_log_time "${config_label}-predump_last" "$predump_last_dir/dump.log" "$time_breakdown_file"
  fi
  analyze_dump_log_time "${config_label}-dump" "$dump_work/dump.log" "$time_breakdown_file"

  # Compute transfer time totals and derived metrics (based on TRANSFER_BANDWIDTH_MBPS)
  local total_transfer_ms=0
  for iter_dir in "$checkpoint_log"/predump_*; do
    if [ -f "$iter_dir/transfer_time_ms.txt" ]; then
      val=$(cat "$iter_dir/transfer_time_ms.txt" 2>/dev/null || echo 0)
      total_transfer_ms=$((total_transfer_ms + val))
    fi
  done
  if [ -f "$dump_work/transfer_time_ms.txt" ]; then
    total_transfer_ms=$((total_transfer_ms + $(cat "$dump_work/transfer_time_ms.txt")))
  fi
  echo "$total_transfer_ms" > "$run_dir/total_transfer_ms.txt"
  # Include post-dump restore verification time in total migration time
  local total_migration_ms=$((total_checkpoint_ms + total_transfer_ms + restore_from_new_ckpt_ms))
  echo "$total_migration_ms" > "$run_dir/total_migration_ms.txt"

  local dump_transfer_ms=0
  if [ -f "$dump_work/transfer_time_ms.txt" ]; then
    dump_transfer_ms=$(cat "$dump_work/transfer_time_ms.txt")
  fi
  # Include post-dump restore verification time in downtime
  local downtime_ms=$((dump_duration + dump_transfer_ms + restore_from_new_ckpt_ms))
  echo "$downtime_ms" > "$run_dir/downtime_ms.txt"
  echo "${TRANSFER_BANDWIDTH_MBPS:-50}" > "$run_dir/bandwidth_used.txt"

  echo "0" > "$run_dir/exitcode.txt"

  # Clear any stale abort reason on successful run to avoid misleading post-mortem
  if [ -f "$run_dir/abort_reason.txt" ]; then
    rm -f "$run_dir/abort_reason.txt" || true
    echo "Cleared stale abort_reason.txt on successful run" >> "$EXP_DIR/control.log"
  fi

  # PAGE/Pagemap/mount-parse matching and recording removed per policy: these patterns are noisy and unrelated to experiment-level failures.
  # Intentionally do not search for or record PAGE/Pagemap/mount-parse messages here.


  echo "$total_checkpoint_ms" > "$run_dir/total_checkpoint_ms.txt"
  echo "$total_predump_ms" > "$run_dir/total_predump_ms.txt"
  echo "$dump_duration" > "$run_dir/dump_ms.txt"
  echo "$restore_from_new_ckpt_ms" > "$run_dir/post_dump_restore_ms.txt"
  echo "$total_predump_pages" > "$run_dir/total_predump_pages.txt"
  echo "$dump_pages" > "$run_dir/dump_pages.txt"
  echo "$total_pages" > "$run_dir/total_pages.txt"

  # Log summary with page statistics
  echo "Round $round (${config_label}): checkpoint_total=${total_checkpoint_ms}ms (predump=${total_predump_ms}ms, dump=${dump_duration}ms), post-dump-restore=${restore_from_new_ckpt_ms}ms, pages=${total_pages} (predump=${total_predump_pages}, dump=${dump_pages})" >> "$EXP_DIR/control.log"

  # Step 7: Copy successful checkpoint to persistent storage (keep latest)
  # With single-run invocations, always preserve the latest checkpoint when dirtymap optimization is used
  if [ "$use_dirtymap" -eq 1 ]; then
    # Create a per-run checkpoint directory encoding config & run (no origin_run.txt file)
    safe_cfg=$(echo "${config_label}" | sed 's/[^a-zA-Z0-9_.-]/_/g')
    RUN_CKPT_DIR="${CHECKPOINT_OUTPUT_DIR}/${image_variant}_cfg_${safe_cfg}_run_${round}_${SCRIPT_PID}_${SCRIPT_RANDOM}"
    mkdir -p "$RUN_CKPT_DIR"
    echo "Copying latest checkpoint to $RUN_CKPT_DIR" >> "$EXP_DIR/control.log"
    echo "Note: run_grid.py will remove checkpoints for successful runs by default; failed runs' checkpoints are preserved." >> "$EXP_DIR/control.log"
    # Keep files for debugging
    cp -r "$image_dir" "$RUN_CKPT_DIR/" 2>/dev/null || true
    for pdir in "$checkpoint_base"/parent_*; do
      [ -d "$pdir" ] && cp -r "$pdir" "$RUN_CKPT_DIR/" || true
    done
    # We no longer write origin_run.txt; naming encodes config and run
    echo "Latest checkpoint preserved at $RUN_CKPT_DIR" >> "$EXP_DIR/control.log"
  fi

  # Cleanup (workload已在dump后stop，这里只需清理容器)
  # Note: CUR_CTID may have been updated to post_dump_ctid, reset it for cleanup
  CUR_CTID="$ctid"
  cleanup_checkpoint "$ctid" "$checkpoint_base" 0

  return 0
}

# Main experiment loop
PRESERVE_ON_FAIL=0
# Pre-declare image_variant (derived from IMAGE_PATH) to avoid unbound variable errors when set -u is active
image_variant="${IMAGE_VARIANT:-$(basename "$(dirname "${IMAGE_PATH:-$DEFAULT_IMAGE_PATH}")" 2>/dev/null || echo "${BUNDLE_NAME:-unknown}") }"
echo "Starting checkpoint run for bundle $BUNDLE" >> "$EXP_DIR/control.log"
echo "Image: $IMAGE_PATH" >> "$EXP_DIR/control.log"
echo "Single run per invocation (ROUNDS deprecated)" >> "$EXP_DIR/control.log"

# Create summary CSV (append header if not present)
summary_csv="$EXP_DIR/summary.csv"
if [ ! -s "$summary_csv" ]; then
  echo "variant,round,exitcode,total_checkpoint_ms,total_predump_ms,dump_ms,post_dump_restore_ms,total_pages,predump_pages,dump_pages,iteration_count,predump_iter_pages,total_transfer_ms,total_migration_ms,downtime_ms,bandwidth_mbps" > "$summary_csv"
fi
#
# Single run invocation (image-driven; variant concept removed)
variant_failed=0
# Allow caller to provide an explicit run number as second positional arg (for chk_rst.sh repeats)
# Fallback to 1 to preserve backward compatibility
r="${2:-1}"
  config_label="${VARIANT_NAME:-baseline}"
  variant_dir_name="$config_label"
  echo "Starting run $r (config_label=${config_label})" >> "$EXP_DIR/control.log"
  run_checkpoint_variant "$config_label" "$r" || variant_failed=1

  # Resolve run directory: prefer deterministic per-image path created by run_checkpoint_variant
  per_image_root="${TEMP_ROOT:-$EXP_ROOT}/${image_variant}"
  run_dir="${per_image_root}/${config_label}/run_$r"
  # Fallback: if expected run dir not found, search for run_$r under EXP_DIR
  if [ ! -d "$run_dir" ]; then
    run_dir="$(find "$EXP_DIR" -type d -name "run_$r" -print | head -n 1 || true)"
  fi
  if [ -z "$run_dir" ]; then
    echo "ERROR: could not locate run directory under $EXP_DIR" >> "$EXP_DIR/control.log"
    echo "1" > "$EXP_DIR/exitcode.txt"
    cleanup_checkpoint "$ctid" "$checkpoint_base" 0
    return 1
  fi

  exitcode=$(cat "$run_dir/exitcode.txt" 2>/dev/null || echo "1")
  total_chk=$(cat "$run_dir/total_checkpoint_ms.txt" 2>/dev/null || echo "0")
  predump=$(cat "$run_dir/total_predump_ms.txt" 2>/dev/null || echo "0")
  dump=$(cat "$run_dir/dump_ms.txt" 2>/dev/null || echo "0")
  post_restore=$(cat "$run_dir/post_dump_restore_ms.txt" 2>/dev/null || echo "0")
  initial_restore=$(cat "$run_dir/restore_log/restore_duration_ms.txt" 2>/dev/null || echo "0")
  total_pages=$(cat "$run_dir/total_pages.txt" 2>/dev/null || echo "0")
  predump_pages=$(cat "$run_dir/total_predump_pages.txt" 2>/dev/null || echo "0")
  dump_pages=$(cat "$run_dir/dump_pages.txt" 2>/dev/null || echo "0")

  predump_iter_count=$(cat "$run_dir/predump_iter_count.txt" 2>/dev/null || echo "0")
  iter_count=$((predump_iter_count + 1))

  # Collect per-iteration predump pages into a semicolon-separated sequence for easy inspection
  predump_pages_seq=""
  for pd in "$run_dir"/checkpoint_log/predump_*; do
    if [ -d "$pd" ]; then
      pages=$(cat "$pd/pages_transferred.txt" 2>/dev/null || echo 0)
      if [ -z "$predump_pages_seq" ]; then
        predump_pages_seq="$pages"
      else
        predump_pages_seq="$predump_pages_seq;$pages"
      fi
    fi
  done

  total_transfer_ms=$(cat "$run_dir/total_transfer_ms.txt" 2>/dev/null || echo 0)
  total_migration_ms=$(cat "$run_dir/total_migration_ms.txt" 2>/dev/null || echo 0)
  downtime_ms=$(cat "$run_dir/downtime_ms.txt" 2>/dev/null || echo 0)
  bandwidth_mbps=$(cat "$run_dir/bandwidth_used.txt" 2>/dev/null || echo "${TRANSFER_BANDWIDTH_MBPS:-50}")

  echo "${variant_dir_name},$r,$exitcode,$total_chk,$predump,$dump,$post_restore,$total_pages,$predump_pages,$dump_pages,$iter_count,$predump_pages_seq,$total_transfer_ms,$total_migration_ms,$downtime_ms,$bandwidth_mbps" >> "$summary_csv"

  if [ "$variant_failed" -eq 1 ]; then
    echo "ERROR: Variant ${variant_dir_name} run $r failed (exitcode=$exitcode). Stopping." >> "$EXP_DIR/control.log"
    exit 1
  fi

# Calculate aggregate statistics
echo "" >> "$EXP_DIR/control.log"
echo "=== Checkpoint run Summary ===" >> "$EXP_DIR/control.log"

for variant in $(awk -F, 'NR>1{print $1}' "$summary_csv" | sort -u); do
  success=$(command awk -F, -v v="$variant" '$1==v && $3==0 {c++} END {print c+0}' "$summary_csv")
  failed=$(command awk -F, -v v="$variant" '$1==v && $3!=0 {c++} END {print c+0}' "$summary_csv")
  avg_total=$(command awk -F, -v v="$variant" '$1==v && $3==0 && $4!="" {sum+=$4; c++} END {if(c>0) printf("%.2f", sum/c); else print "NA"}' "$summary_csv")
  avg_predump=$(command awk -F, -v v="$variant" '$1==v && $3==0 && $5!="" {sum+=$5; c++} END {if(c>0) printf("%.2f", sum/c); else print "NA"}' "$summary_csv")
  avg_dump=$(command awk -F, -v v="$variant" '$1==v && $3==0 && $6!="" {sum+=$6; c++} END {if(c>0) printf("%.2f", sum/c); else print "NA"}' "$summary_csv")
  avg_total_pages=$(command awk -F, -v v="$variant" '$1==v && $3==0 && $9!="" {sum+=$9; c++} END {if(c>0) printf("%.0f", sum/c); else print "NA"}' "$summary_csv")
  avg_predump_pages=$(command awk -F, -v v="$variant" '$1==v && $3==0 && $10!="" {sum+=$10; c++} END {if(c>0) printf("%.0f", sum/c); else print "NA"}' "$summary_csv")
  avg_dump_pages=$(command awk -F, -v v="$variant" '$1==v && $3==0 && $11!="" {sum+=$11; c++} END {if(c>0) printf("%.0f", sum/c); else print "NA"}' "$summary_csv")

  echo "$variant: success=$success failed=$failed avg_total=${avg_total}ms avg_predump=${avg_predump}ms avg_dump=${avg_dump}ms pages=${avg_total_pages} (predump=${avg_predump_pages}, dump=${avg_dump_pages})" >> "$EXP_DIR/control.log"
done

date -u > "$EXP_DIR/end_time.txt"
echo "Checkpoint run complete. Results in $EXP_DIR" >> "$EXP_DIR/control.log"

# 清理函数
final_cleanup() {
  local exit_code=$?

  echo "" >> "$EXP_DIR/control.log"
  echo "=== Final Cleanup ===" >> "$EXP_DIR/control.log"

  # 1. 清理所有容器
  if [ -f "$EXP_DIR/created_containers.txt" ]; then
    while IFS= read -r ctid; do
      [ -z "$ctid" ] && continue
      echo "Cleaning container: $ctid" >> "$EXP_DIR/control.log"
      runc kill "$ctid" SIGKILL 2>/dev/null || true
      sleep 0.2
      runc delete --force "$ctid" 2>/dev/null || true
    done < "$EXP_DIR/created_containers.txt"
  fi

  # 2. 卸载dirtymap挂载点
  echo "Unmounting dirtymap directories..." >> "$EXP_DIR/control.log"
  for mnt in $(mount 2>/dev/null | grep "$DIRTYMAP_DIR" | command awk '{print $3}'); do
    umount -l "$mnt" 2>/dev/null || true
    echo "Unmounted $mnt" >> "$EXP_DIR/control.log"
  done

  # 3. 清理临时recvtty
  if [ -n "${TEMP_RECVTTY_PID:-}" ]; then
    if [[ "${TEMP_RECVTTY_PID}" =~ ^[0-9]+$ ]] && [ "${TEMP_RECVTTY_PID}" -ge 2 ]; then
      kill -9 "${TEMP_RECVTTY_PID}" 2>/dev/null || true
      echo "final_cleanup: killed TEMP_RECVTTY_PID=${TEMP_RECVTTY_PID}" >> "$EXP_DIR/control.log" 2>/dev/null || true
    else
      echo "final_cleanup: refusing to kill suspicious TEMP_RECVTTY_PID='${TEMP_RECVTTY_PID}'" >> "$EXP_DIR/control.log" 2>/dev/null || true
    fi
  fi

  echo "Cleanup complete at $(date)" >> "$EXP_DIR/control.log"

  # Summarize control log: move full log aside and create condensed control.log
  if [ -f "$EXP_DIR/control.log" ]; then
    cp "$EXP_DIR/control.log" "$EXP_DIR/control_full.log" 2>/dev/null || true
    tmpfile="$(mktemp "${EXP_DIR}/control_summary.XXXXXX")"
    echo "Condensed control log (generated at $(date -u +%Y-%m-%dT%H:%M:%SZ))" > "$tmpfile"
    echo "" >> "$tmpfile"
    echo "Per-run summary:" >> "$tmpfile"
    # Legacy layout: experiments/<variant>/run_*
    if [ -d "$EXP_DIR/experiments" ]; then
      for variant_dir in "$EXP_DIR/experiments"/*; do
        [ -d "$variant_dir" ] || continue
        for run_dir in "$variant_dir"/run_*; do
          [ -d "$run_dir" ] || continue
          ec='MISSING'; ar='-'; errs=''
          if [ -f "$run_dir/exitcode.txt" ]; then ec=$(tr -d ' \t\n\r' < "$run_dir/exitcode.txt"); fi
          if [ -f "$run_dir/abort_reason.txt" ]; then ar=$(tr -d '\n' < "$run_dir/abort_reason.txt"); fi
          msg=$(grep -RIn --include='dump.log' -E "Pre-dumping FAILED|Dumping FAILED|ERROR:|POST_DUMP_RESTORE_FAILED|POST_DUMP_VERIFY_FAILED|INITIAL_RESTORE_FAILED|DUMP_FAILED|MOUNT_PARSE_FAILED_OR_PRE_DUMP_FAILED|SKIP_ACCURACY_ABORT" "$run_dir" 2>/dev/null | head -n 1 || true)
          if [ -n "$msg" ]; then errs="$msg"; fi
          printf "RUN: %s status=exit=%s abort=\"%s\" errors=\"%s\"\n" "$run_dir" "$ec" "$ar" "$errs" >> "$tmpfile"
        done
      done
    fi
    # New layout: <container>/<variant>/run_*
    for container_dir in "$EXP_DIR"/*; do
      [ -d "$container_dir" ] || continue
      base="$(basename "$container_dir")"
      case "$base" in
        experiments|analysis|ckpt|control_full.log|control.log|run.log|start_time.txt|end_time.txt|summary.csv) continue ;;
      esac
      for variant_dir in "$container_dir"/*; do
        [ -d "$variant_dir" ] || continue
        for run_dir in "$variant_dir"/run_*; do
          [ -d "$run_dir" ] || continue
          ec='MISSING'; ar='-'; errs=''
          if [ -f "$run_dir/exitcode.txt" ]; then ec=$(tr -d ' \t\n\r' < "$run_dir/exitcode.txt"); fi
          if [ -f "$run_dir/abort_reason.txt" ]; then ar=$(tr -d '\n' < "$run_dir/abort_reason.txt"); fi
          msg=$(grep -RIn --include='dump.log' -E "Pre-dumping FAILED|Dumping FAILED|ERROR:|POST_DUMP_RESTORE_FAILED|POST_DUMP_VERIFY_FAILED|INITIAL_RESTORE_FAILED|DUMP_FAILED|MOUNT_PARSE_FAILED_OR_PRE_DUMP_FAILED|SKIP_ACCURACY_ABORT" "$run_dir" 2>/dev/null | head -n 1 || true)
          if [ -n "$msg" ]; then errs="$msg"; fi
          printf "RUN: %s status=exit=%s abort=\"%s\" errors=\"%s\"\n" "$run_dir" "$ec" "$ar" "$errs" >> "$tmpfile"
        done
      done
    done

    echo "" >> "$tmpfile"
    echo "Top-level ERROR/FAILED lines (first 200, filtered to actual failed runs):" >> "$tmpfile"
    if [ -f "$EXP_DIR/control_full.log" ]; then
      # Only include ERROR/FAILED lines if they relate to runs that currently indicate failure
      # Iterate over the per-run summary lines we just wrote and pick failed runs
      while read -r runline; do
        run_dir=$(echo "$runline" | awk '{print $2}')
        ec=$(echo "$runline" | sed -n 's/.*status=exit=\([^ ]*\).*/\1/p')
        ar=$(echo "$runline" | sed -n 's/.*abort=\"\([^\"]*\)\".*/\1/p')
        if [ "${ec:-}" != "0" ] && [ "${ec:-}" != "MISSING" ]; then
          # Match the run path in the full control log
          grep -En -F -- "$run_dir" "$EXP_DIR/control_full.log" 2>/dev/null | sed -n '1,200p' >> "$tmpfile" || true
        elif [ -n "${ar:-}" ] && [ "$ar" != "-" ]; then
          # If an abort reason exists, try to find any related lines mentioning it
          grep -En -F -- "$ar" "$EXP_DIR/control_full.log" 2>/dev/null | sed -n '1,200p' >> "$tmpfile" || true
        fi
      done < <(grep '^RUN:' "$tmpfile" || true)
    fi
    mv "$tmpfile" "$EXP_DIR/control.log" || true
  fi

  return $exit_code
}

trap final_cleanup EXIT INT TERM

echo "Summary: $summary_csv"

ls -lh "$summary_csv"
cat "$summary_csv"
