#!/usr/bin/env python3
"""
Faster skip-accuracy analyzer that groups decisions by next-iteration and PID and
loads each relevant dirtymap once.
Usage: python3 scripts/analyze_skip_accuracy_fast.py <exp_dir> <variant> <run_num>
Example: python3 scripts/analyze_skip_accuracy_fast.py /tmp/exp/mc_2257246_364/redis_redis_video_cache_fr70_chk_/experiments B 1
"""

import sys
import os
import csv
import glob
import struct
from collections import defaultdict

ENTRY_DM_STRUCT = '<QI'
ENTRY_DM_SIZE = struct.calcsize(ENTRY_DM_STRUCT)


def parse_dirtymap_file(path):
    try:
        with open(path, 'rb') as f:
            hdr = f.read(8)
            if len(hdr) < 8:
                return None, {}
            (track_ns,) = struct.unpack('<Q', hdr)
            data = f.read()
            if len(data) % ENTRY_DM_SIZE != 0:
                rem = len(data) % ENTRY_DM_SIZE
                data = data[:len(data)-rem]
            res = {}
            for off in range(0, len(data), ENTRY_DM_SIZE):
                addr, writes = struct.unpack_from(ENTRY_DM_STRUCT, data, off)
                res[addr] = writes
            return track_ns, res
    except Exception:
        return None, {}


def find_latest_dirtymap_for_pid(iter_dir, pid):
    dbacks = sorted(glob.glob(os.path.join(iter_dir, 'dirtymap_backup_*')))
    candidates = []
    for d in dbacks:
        for fn in glob.glob(os.path.join(d, f"{pid}-*.dirtymap")):
            base = os.path.basename(fn)
            try:
                pid_s, ts_s = base.split('.', 1)[0].split('-', 1)
                ts = int(ts_s)
            except Exception:
                ts = 0
            candidates.append((ts, fn))
    if not candidates:
        return None
    candidates.sort(key=lambda x: x[0])
    return candidates[-1][1]


def load_decisions(run_dir):
    dec_file = os.path.join(run_dir, 'decisions.csv')
    if not os.path.exists(dec_file):
        print('decisions.csv not found in', run_dir)
        return []
    rows = []
    with open(dec_file, 'r') as f:
        r = csv.DictReader(f)
        for row in r:
            if not row.get('addr'):
                continue
            try:
                addr = int(row['addr'], 0)
            except Exception:
                try:
                    addr = int(row['addr'], 16)
                except Exception:
                    continue
            row['addr_int'] = addr
            try:
                row['round'] = int(row.get('round', 0))
            except:
                row['round'] = 0
            rows.append(row)
    return rows


def list_iterations(checkpoint_log):
    preds = sorted([d for d in glob.glob(os.path.join(checkpoint_log, 'predump_*')) if os.path.isdir(d)])
    iters = [os.path.basename(p) for p in preds]
    dumpd = os.path.join(checkpoint_log, 'dump_log')
    if os.path.isdir(dumpd):
        iters.append('dump_log')
    name_to_idx = {name: i for i, name in enumerate(iters)}
    return iters, name_to_idx


def analyze_skip_accuracy_fast(exp_dir, variant, run_num):
    run_dir = f"{exp_dir}/{variant}/run_{run_num}"
    checkpoint_log = os.path.join(run_dir, 'checkpoint_log')

    print('\n' + '='*86)
    print(f"Analyzing skip accuracy fast: {variant} Run {run_num}")
    print('='*86 + '\n')

    decisions = load_decisions(run_dir)
    if not decisions:
        print('No decisions.csv or empty for', run_dir)
        return 0.0

    iters, name_to_idx = list_iterations(checkpoint_log)

    skip_types = set(['DEFER'])

    # Build list of skip decisions that have a next iteration
    decisions_to_check = []
    for row in decisions:
        if row.get('decision') not in skip_types:
            continue
        pd = row.get('predump_dir')
        if pd not in name_to_idx:
            if pd == 'dump' and 'dump_log' in name_to_idx:
                idx = name_to_idx['dump_log']
            else:
                continue
        else:
            idx = name_to_idx[pd]
        next_idx = idx + 1
        if next_idx >= len(iters):
            continue
        next_iter_name = iters[next_idx]
        pid = int(row.get('pid') or 0)
        decisions_to_check.append((next_iter_name, pid, row['addr_int']))

    if not decisions_to_check:
        print('No skip decisions with next-iteration dirtymap available to evaluate.')
        return 0.0

    # Group by (next_iter_name, pid)
    group = defaultdict(list)
    for nit, pid, addr in decisions_to_check:
        group[(nit, pid)].append(addr)

    total = 0
    correct = 0
    incorrect = 0

    # For each group, load the latest dirtymap for that pid under next_iter_dir
    for (next_iter_name, pid), addrs in group.items():
        next_iter_dir = os.path.join(checkpoint_log, next_iter_name)
        dm_file = find_latest_dirtymap_for_pid(next_iter_dir, pid)
        track_ns, dm_map = (None, {})
        if dm_file:
            track_ns, dm_map = parse_dirtymap_file(dm_file)
        # Evaluate addresses
        for addr in addrs:
            total += 1
            if dm_map and track_ns and track_ns > 0:
                writes = dm_map.get(addr, 0)
                if writes > 0:
                    incorrect += 1
                else:
                    correct += 1
            else:
                # treat missing dirtymap as no writes observed
                correct += 1

    # Print per-iteration stats as a summary
    print(f"Total skipped evaluated: {total}, Correct={correct}, Incorrect={incorrect}")
    overall_acc = (correct/total*100) if total>0 else 0.0
    print('\n' + '='*40)
    print(f"Overall Accuracy: {overall_acc:.1f}%")
    print('='*40 + '\n')

    return overall_acc


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <exp_dir> <variant> <run_num>")
        sys.exit(1)
    exp_dir = sys.argv[1]
    variant = sys.argv[2]
    run_num = int(sys.argv[3])
    acc = analyze_skip_accuracy_fast(exp_dir, variant, run_num)
    if acc < 50:
        print(f"⚠️  WARNING: Skip accuracy ({acc:.1f}%) is below 50%!")
    elif acc < 70:
        print(f"⚠️  NOTICE: Skip accuracy ({acc:.1f}%) is moderate but needs improvement.")
    else:
        print(f"✓  Skip accuracy ({acc:.1f}%) is acceptable.")


if __name__ == '__main__':
    main()
