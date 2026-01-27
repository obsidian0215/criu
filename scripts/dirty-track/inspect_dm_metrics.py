#!/usr/bin/env python3
"""Inspect CRIU dirty-track prediction metrics from a checkpoint run directory.

This script parses per-iteration files (predump_N/dump.log or prediction_stats.txt/deferred_stats.txt)
and prints per-iteration and aggregate prediction metrics.

Behavior notes:
- CRIU emits `[ObsidianPred]` and `[ObsidianDef]` summary lines into each `dump.log` when dirtymap
  is enabled. These summary lines are produced even when per-page telemetry (ObsidianDecision /
  ObsidianDEFER) is disabled (CRIU_DECISION_TELEMETRY=0).
- The driver `checkpoint_run_impl.sh` also writes compact `prediction_stats.txt` and
  `deferred_stats.txt` into each iteration directory; if present, the script prefers those files
  for robust parsing.

Usage:
  scripts/dirty-track/inspect_dm_metrics.py /path/to/run_dir [--json] [--show-final]

Examples:
  python3 scripts/dirty-track/inspect_dm_metrics.py /tmp/exp/run_1
  python3 scripts/dirty-track/inspect_dm_metrics.py /tmp/exp/run_1 --json

"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Dict, List, Optional, Tuple

KV_RE = re.compile(r"([A-Za-z_]+)=([0-9]+(?:\.[0-9]+)?)")


def parse_kv_file(path: str) -> Dict[str, float]:
    d: Dict[str, float] = {}
    try:
        with open(path, "r", errors="ignore") as f:
            for line in f:
                line = line.strip()
                if not line or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                try:
                    if "." in v:
                        dv = float(v)
                    else:
                        dv = int(v)
                except Exception:
                    try:
                        dv = float(v)
                    except Exception:
                        dv = v
                d[k.strip()] = dv
    except FileNotFoundError:
        pass
    return d


def parse_pred_from_dump_log(path: str) -> Tuple[int, int, int, float]:
    """Parse predicted_total/hit/miss/accuracy from a dump.log by summing ObsidianPred lines."""
    total = 0
    hit = 0
    miss = 0
    last_acc: Optional[float] = None
    try:
        with open(path, "r", errors="ignore") as f:
            for line in f:
                if "[ObsidianPred]" not in line:
                    continue
                for m in KV_RE.finditer(line):
                    k = m.group(1)
                    v = m.group(2)
                    if k == "predicted_total":
                        total += int(float(v))
                    elif k == "predicted_hit":
                        hit += int(float(v))
                    elif k == "predicted_miss":
                        miss += int(float(v))
                    elif k == "predicted_accuracy":
                        try:
                            last_acc = float(v)
                        except Exception:
                            pass
    except FileNotFoundError:
        return 0, 0, 0, 0.0

    acc = (hit * 100.0 / total) if total > 0 else (last_acc if last_acc is not None else 0.0)
    return total, hit, miss, round(acc, 2)


def parse_def_from_dump_log(path: str) -> int:
    """Sum deferred_total from ObsidianDef lines in dump.log"""
    d_total = 0
    try:
        with open(path, "r", errors="ignore") as f:
            for line in f:
                if "[ObsidianDef]" not in line:
                    continue
                for m in KV_RE.finditer(line):
                    k = m.group(1)
                    v = m.group(2)
                    if k == "deferred_total":
                        d_total += int(float(v))
    except FileNotFoundError:
        return 0
    return d_total


def parse_def_final(path: str) -> Dict[str, float]:
    """Parse ObsidianDefFinal (final_decision_accuracy, deferred_unique_total, ...)."""
    out: Dict[str, float] = {}
    try:
        with open(path, "r", errors="ignore") as f:
            for line in f:
                if "[ObsidianDefFinal]" not in line:
                    continue
                for m in KV_RE.finditer(line):
                    k = m.group(1)
                    v = m.group(2)
                    try:
                        out[k] = float(v) if ("." in v) else int(v)
                    except Exception:
                        try:
                            out[k] = float(v)
                        except Exception:
                            out[k] = v
    except FileNotFoundError:
        pass
    return out


def find_checkpoint_log_dir(path: str) -> Optional[str]:
    # If path already looks like checkpoint_log or predump dir, use it
    if os.path.isdir(path):
        base = os.path.basename(path.rstrip(os.sep))
        if base == "checkpoint_log" or re.match(r"predump_\d+|pd_log_\d+", base):
            return path
        # If run_dir contains checkpoint_log subdir
        candidate = os.path.join(path, "checkpoint_log")
        if os.path.isdir(candidate):
            return candidate
        # If path directly contains predump_* entries, treat it as checkpoint_log
        for ent in os.listdir(path):
            if re.match(r"predump_\d+|pd_log_\d+", ent):
                return path
    return None


def list_iter_dirs(checkpoint_log: str) -> List[str]:
    iters = []
    if not checkpoint_log or not os.path.isdir(checkpoint_log):
        return []
    for name in os.listdir(checkpoint_log):
        m = re.match(r"(?:predump_|pd_log_)(\d+)$", name)
        if m:
            iters.append((int(m.group(1)), os.path.join(checkpoint_log, name)))
    iters.sort()
    return [p for (_, p) in iters]


def read_iteration_metrics(iter_dir: str) -> Dict:
    # Defaults
    metrics = {
        "iter_dir": os.path.basename(iter_dir),
        "pages": None,
        "duration_ms": None,
        "predicted_total": 0,
        "predicted_hit": 0,
        "predicted_miss": 0,
        "predicted_accuracy": 0.0,
        "deferred_total": 0,
    }

    # pages_transferred.txt / duration_ms.txt
    pt = os.path.join(iter_dir, "pages_transferred.txt")
    if os.path.exists(pt):
        try:
            metrics["pages"] = int(open(pt).read().strip() or 0)
        except Exception:
            pass
    dt = os.path.join(iter_dir, "duration_ms.txt")
    if os.path.exists(dt):
        try:
            metrics["duration_ms"] = int(open(dt).read().strip() or 0)
        except Exception:
            pass

    # Compact stats files (preferred)
    pred_stats = os.path.join(iter_dir, "prediction_stats.txt")
    def_stats = os.path.join(iter_dir, "deferred_stats.txt")
    if os.path.exists(pred_stats):
        kv = parse_kv_file(pred_stats)
        metrics["predicted_total"] = int(kv.get("predicted_total", 0))
        metrics["predicted_hit"] = int(kv.get("predicted_hit", 0))
        metrics["predicted_miss"] = int(kv.get("predicted_miss", 0))
        metrics["predicted_accuracy"] = float(kv.get("predicted_accuracy", 0.0))
    else:
        # fallback to parsing dump.log
        dl = os.path.join(iter_dir, "dump.log")
        if os.path.exists(dl):
            t, h, m, acc = parse_pred_from_dump_log(dl)
            metrics["predicted_total"] = t
            metrics["predicted_hit"] = h
            metrics["predicted_miss"] = m
            metrics["predicted_accuracy"] = acc

    if os.path.exists(def_stats):
        kv = parse_kv_file(def_stats)
        metrics["deferred_total"] = int(kv.get("deferred_total", 0))
    else:
        dl = os.path.join(iter_dir, "dump.log")
        if os.path.exists(dl):
            metrics["deferred_total"] = parse_def_from_dump_log(dl)

    return metrics


def summarize(iter_metrics: List[Dict]) -> Dict:
    out = {
        "iterations": len(iter_metrics),
        "total_predicted_total": 0,
        "total_predicted_hit": 0,
        "total_predicted_miss": 0,
        "aggregate_predicted_accuracy": 0.0,
        "total_deferred": 0,
    }
    for m in iter_metrics:
        out["total_predicted_total"] += int(m.get("predicted_total", 0) or 0)
        out["total_predicted_hit"] += int(m.get("predicted_hit", 0) or 0)
        out["total_predicted_miss"] += int(m.get("predicted_miss", 0) or 0)
        out["total_deferred"] += int(m.get("deferred_total", 0) or 0)

    if out["total_predicted_total"] > 0:
        out["aggregate_predicted_accuracy"] = round(
            out["total_predicted_hit"] * 100.0 / out["total_predicted_total"], 2
        )
    else:
        out["aggregate_predicted_accuracy"] = 0.0

    return out


def print_table(iter_metrics: List[Dict], summary_stats: Dict, final_def: Optional[Dict], out_json: bool = False):
    if out_json:
        out = {"iterations": iter_metrics, "summary": summary_stats}
        if final_def:
            out["final_def"] = final_def
        print(json.dumps(out, indent=2))
        return

    # pretty text
    hdr = (
        "iter", "pages", "dur_ms", "pred_total", "pred_hit", "pred_miss", "pred_acc%", "deferred_total"
    )
    print("\t".join(hdr))
    for m in iter_metrics:
        print(
            f"{m['iter_dir']}	{m.get('pages','-')}	{m.get('duration_ms','-')}	"
            f"{m.get('predicted_total',0)}	{m.get('predicted_hit',0)}	{m.get('predicted_miss',0)}	{m.get('predicted_accuracy',0.0)}	{m.get('deferred_total',0)}"
        )

    print("\nSummary:")
    print(f"Iterations: {summary_stats['iterations']}")
    print(f"Total predicted_total: {summary_stats['total_predicted_total']}")
    print(f"Total predicted_hit: {summary_stats['total_predicted_hit']}")
    print(f"Aggregate predicted_accuracy: {summary_stats['aggregate_predicted_accuracy']}%")
    print(f"Total deferred (sum): {summary_stats['total_deferred']}")
    if final_def:
        print("\nFinal ObsidianDefFinal:")
        for k, v in final_def.items():
            print(f"{k}: {v}")


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(description="Inspect CRIU dirty-track pred/del metrics in a run directory")
    p.add_argument("path", help="Path to run directory or checkpoint_log directory")
    p.add_argument("--json", action="store_true", help="Output JSON")
    p.add_argument("--show-final", action="store_true", help="Parse final dump's ObsidianDefFinal (if present)")
    args = p.parse_args(argv)

    checkpoint_log = find_checkpoint_log_dir(args.path)
    if not checkpoint_log:
        print(f"ERROR: cannot find checkpoint_log under {args.path}")
        return 2

    iter_dirs = list_iter_dirs(checkpoint_log)
    if not iter_dirs:
        print(f"No predump_* or pd_log_* found under {checkpoint_log}")
        return 2

    iter_metrics = []
    for d in iter_dirs:
        iter_metrics.append(read_iteration_metrics(d))

    summary_stats = summarize(iter_metrics)

    final_def = None
    if args.show_final:
        # final dump log is usually at checkpoint_base/image or checkpoint_log/dump_log/dump.log
        # Look for checkpoint_log/../dump_log/dump.log or checkpoint_log/dump_log/dump.log
        # We search common candidate paths
        cand = [os.path.join(checkpoint_log, "../dump_log/dump.log"), os.path.join(checkpoint_log, "dump_log/dump.log")]
        final_path = None
        for c in cand:
            c_abs = os.path.abspath(c)
            if os.path.exists(c_abs):
                final_path = c_abs
                break
        if final_path:
            final_def = parse_def_final(final_path)

    print_table(iter_metrics, summary_stats, final_def, out_json=args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
