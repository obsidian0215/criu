#!/usr/bin/env python3
"""Parse dirtymap/warm_list/deferred_list artifacts and summarize per-iteration stats.

Usage:
  ./scripts/parse_dirtymap.py [--run DIR]

If no run DIR is provided, it finds the latest experiment under /tmp/exp and inspects the first workload's latest run.

Outputs CSV summary files into the run directory: dirtymap_agg.csv and promoted_histories.csv
"""

import os
import sys
import glob
import struct
import argparse
import csv
import math
from statistics import median, mean
from collections import defaultdict, OrderedDict
import re

DECISION_RE = re.compile(r'\[ObsidianDecision\]\s+(.*)')
DEFER_RE = re.compile(r'\[ObsidianDEFER\]\s+pid=(\d+)\s+addr=(0x[0-9a-fA-F]+)\s+count=(\d+)\s+deferred_size=(\d+)(?:\s+last_round=(-?\d+))?(?:\s+protected_until=(-?\d+))?')
ENFORCE_RE = re.compile(r'\[ObsidianEnforceQueue\]\s+pid=(\d+):\s+picked\s+(\d+)\s+pages\s+to\s+force.*skipped_min_count=(\d+)\s+skipped_cooldown_count=(\d+)\s+deferred_sum_after=([0-9.]+)')
DEFER_SKIP_RE = re.compile(r'\[ObsidianDEFER_SKIP\]\s+pid=(\d+)\s+addr=(0x[0-9a-fA-F]+)\s+deferred_count=(\d+)\s+protected_until=(-?\d+)\s+current_round=(-?\d+)\s+skip_min=(\d+)\s+skip_cooldown=(\d+)')

def parse_defer_line(line):
    m = DEFER_RE.search(line)
    if not m:
        return None
    d = {
        'pid': int(m.group(1)),
        'addr': int(m.group(2), 16),
        'count': int(m.group(3)),
        'deferred_size': int(m.group(4)),
        'line': line.strip()
    }
    # optional last_round and protected_until fields
    if m.group(5) is not None:
        try:
            d['last_round'] = int(m.group(5))
        except Exception:
            d['last_round'] = None
    else:
        d['last_round'] = None
    if m.group(6) is not None:
        try:
            d['protected_until'] = int(m.group(6))
        except Exception:
            d['protected_until'] = None
    else:
        d['protected_until'] = None
    return d


def parse_enforce_line(line):
    m = ENFORCE_RE.search(line)
    if not m:
        return None
    try:
        return {
            'pid': int(m.group(1)),
            'picked': int(m.group(2)),
            'skipped_min_count': int(m.group(3)),
            'skipped_cooldown_count': int(m.group(4)),
            'deferred_sum_after': float(m.group(5)),
            'line': line.strip()
        }
    except Exception:
        return None


def parse_defer_skip_line(line):
    m = DEFER_SKIP_RE.search(line)
    if not m:
        return None
    try:
        return {
            'pid': int(m.group(1)),
            'addr': int(m.group(2), 16),
            'deferred_count': int(m.group(3)),
            'protected_until': int(m.group(4)),
            'current_round': int(m.group(5)),
            'skip_min': int(m.group(6)),
            'skip_cooldown': int(m.group(7)),
            'line': line.strip()
        }
    except Exception:
        return None


def parse_decision_line(line):
    m = DECISION_RE.search(line)
    if not m:
        return None
    kv_string = m.group(1)
    tokens = re.findall(r'(\w+)=([^\s]+)', kv_string)
    d = {}
    for k, v in tokens:
        d[k] = v
    # Normalize numeric fields
    if 'pid' in d:
        try:
            d['pid'] = int(d['pid'])
        except:
            pass
    if 'addr' in d:
        try:
            if d['addr'].startswith('0x') or d['addr'].startswith('0X'):
                d['addr'] = int(d['addr'], 16)
            else:
                d['addr'] = int(d['addr'])
        except:
            pass
    for f in ['round','pre_dump','dhm_present','deferred','is_warm','hist_count','hist_is_declining','hist_declining','queued_class']:
        if f in d:
            try:
                d[f] = int(d[f])
            except:
                pass
    for f in ['heat','heat_trend','writes_est','track_s','hist_mean','hist_variance','p','p_model','score','pthr']:
        if f in d:
            try:
                d[f] = float(d[f])
            except:
                pass
    return d


def parse_decision_logs(pred_dir):
    res = []
    res_defers = []
    res_enforce = []
    res_deferskips = []
    for root, dirs, files in os.walk(pred_dir):
        for fn in files:
            if not fn.endswith('.log'):
                continue
            path = os.path.join(root, fn)
            try:
                with open(path, 'r', errors='ignore') as f:
                    for ln in f:
                        parsed = parse_decision_line(ln)
                        if parsed:
                            parsed['file'] = path
                            parsed['line'] = ln.strip()
                            parsed['predump_dir'] = os.path.basename(pred_dir)
                            res.append(parsed)
                        parsed_d = parse_defer_line(ln)
                        if parsed_d:
                            parsed_d['file'] = path
                            parsed_d['predump_dir'] = os.path.basename(pred_dir)
                            res_defers.append(parsed_d)
                        parsed_e = parse_enforce_line(ln)
                        if parsed_e:
                            parsed_e['file'] = path
                            parsed_e['predump_dir'] = os.path.basename(pred_dir)
                            res_enforce.append(parsed_e)
                        parsed_ds = parse_defer_skip_line(ln)
                        if parsed_ds:
                            parsed_ds['file'] = path
                            parsed_ds['predump_dir'] = os.path.basename(pred_dir)
                            res_deferskips.append(parsed_ds)
            except Exception:
                continue
    return res, res_defers, res_enforce, res_deferskips

ENTRY_DM_STRUCT = '<QI'  # unsigned long (8) address, unsigned int (4) write_count
ENTRY_DM_SIZE = struct.calcsize(ENTRY_DM_STRUCT)  # 12
ENTRY_WARM_STRUCT = '<Qb'  # address (8) + signed char s_count (1)
ENTRY_WARM_SIZE = struct.calcsize(ENTRY_WARM_STRUCT)  # 9
ENTRY_DEF_STRUCT = '<QB'   # address (8) + unsigned char count (1)
ENTRY_DEF_SIZE = struct.calcsize(ENTRY_DEF_STRUCT)  # 9


def find_latest_run(base_dir=None):
    # If caller provided a run directory directly, accept it
    if base_dir and os.path.isdir(base_dir) and os.path.isdir(os.path.join(base_dir, 'checkpoint_log')):
        return base_dir

    # If a base_dir is provided and is a directory (e.g., telegrid or mc run), search for runs under it
    if base_dir and os.path.isdir(base_dir):
        # search for run_* recursively under base_dir
        runs = sorted(glob.glob(os.path.join(base_dir, '**', 'run_*'), recursive=True))
        if runs:
            return runs[-1]
        raise RuntimeError('No runs found under %s' % base_dir)

    # Discover global experiment directories under /tmp/exp (telegrid_*, mc_*, run_*) and pick the latest
    exps = sorted(glob.glob('/tmp/exp/telegrid_*') + glob.glob('/tmp/exp/mc_*') + glob.glob('/tmp/exp/run_*'))
    if not exps:
        raise RuntimeError('No experiment folders found under /tmp/exp')
    latest = exps[-1]
    # Search for any run_* under the latest experiment
    runs = sorted(glob.glob(os.path.join(latest, '**', 'run_*'), recursive=True))
    if runs:
        return runs[-1]
    raise RuntimeError('No runs found in %s' % latest)


def parse_dirtymap_file(path):
    """Return (track_duration_ns, dict addr->write_count)"""
    res = {}
    with open(path, 'rb') as f:
        hdr = f.read(8)
        if len(hdr) < 8:
            raise RuntimeError('dirtymap header too small: %s' % path)
        (track_duration_ns,) = struct.unpack('<Q', hdr)
        data = f.read()
        if len(data) % ENTRY_DM_SIZE != 0:
            # tolerate trailing padding
            rem = len(data) % ENTRY_DM_SIZE
            data = data[:len(data)-rem]
        for off in range(0, len(data), ENTRY_DM_SIZE):
            addr, writes = struct.unpack_from(ENTRY_DM_STRUCT, data, off)
            res[addr] = writes
    return track_duration_ns, res


def parse_warm_list_file(path):
    res = {}
    if not os.path.exists(path):
        return res
    with open(path, 'rb') as f:
        data = f.read()
        if len(data) % ENTRY_WARM_SIZE != 0:
            rem = len(data) % ENTRY_WARM_SIZE
            data = data[:len(data)-rem]
        for off in range(0, len(data), ENTRY_WARM_SIZE):
            addr, scount = struct.unpack_from(ENTRY_WARM_STRUCT, data, off)
            res[addr] = int(scount)
    return res


def parse_deferred_file(path):
    res = {}
    if not os.path.exists(path):
        return res
    with open(path, 'rb') as f:
        data = f.read()
        if len(data) % ENTRY_DEF_SIZE != 0:
            rem = len(data) % ENTRY_DEF_SIZE
            data = data[:len(data)-rem]
        for off in range(0, len(data), ENTRY_DEF_SIZE):
            addr, count = struct.unpack_from(ENTRY_DEF_STRUCT, data, off)
            res[addr] = int(count)
    return res


def summarize_run(run_dir, out_dir=None, limit_top=200):
    """Analyze all predump/dump dirtymap backups under run_dir/checkpoint_log"""
    checkpoint_log = os.path.join(run_dir, 'checkpoint_log')
    if not os.path.isdir(checkpoint_log):
        raise RuntimeError('No checkpoint_log in %s' % run_dir)

    predump_dirs = sorted(glob.glob(os.path.join(checkpoint_log, 'predump_*')))
    dump_dir = os.path.join(checkpoint_log, 'dump_log')
    if os.path.isdir(dump_dir):
        predump_dirs.append(dump_dir)

    # Per-iteration aggregate rows
    agg_rows = []
    # Promoted histories table: each promoted page will have a small time-series
    promoted_histories = []
    # Per-page decision logs parsed from dump.log [ObsidianDecision]
    decisions_rows = []
    deferred_rows = []
    # enforcement summaries (from ObsidianEnforceQueue messages)
    enforce_rows = []
    # defer-skip summaries (from ObsidianDEFER_SKIP)
    defer_skip_rows = []

    # Keep previous warm sets to detect promotions
    prev_warm = {}

    # iterate with index so we can reference the next iteration's dirtymaps when mapping decisions
    for idx, pred in enumerate(predump_dirs):
        name = os.path.basename(pred)
        # parse per-page decisions for this predump (if any)
        pred_decisions, pred_defers, pred_enforces, pred_deferskips = parse_decision_logs(pred)
        for d in pred_decisions:
            decisions_rows.append(d)
        for dd in pred_defers:
            deferred_rows.append(dd)
        for e in pred_enforces:
            enforce_rows.append(e)
        for ds in pred_deferskips:
            defer_skip_rows.append(ds)
        backup_glob = os.path.join(pred, 'dirtymap_backup_*')
        dbacks = sorted(glob.glob(backup_glob))
        if not dbacks:
            # still attempt to map decisions to next iter if possible
            # but skip heavy processing
            continue
        # For simplicity analyze each dirtymap_backup_<iter> directory separately
        for db in dbacks:
            iter_name = os.path.basename(db)
            # find pid-specific files
            files = os.listdir(db)
            # group by pid
            pid_groups = defaultdict(lambda: {'dirtymaps': [], 'warm': None, 'deferred': None, 'timestamp': None})
            for fn in files:
                p = os.path.join(db, fn)
                if fn.endswith('.dirtymap'):
                    # parse pid from filename like 2033893-1768630702277.dirtymap
                    try:
                        pid_s, ts_s = fn.split('.', 1)[0].split('-', 1)
                        pid = int(pid_s)
                    except Exception:
                        continue
                    pid_groups[pid]['dirtymaps'].append(p)
                # warm_list artifacts removed; skip
                elif fn.startswith('deferred_list.'):
                    try:
                        pid = int(fn.split('.', 1)[1])
                    except Exception:
                        continue
                    pid_groups[pid]['deferred'] = p
                elif fn.startswith('timestamp_list.'):
                    # skip
                    pass
            # process each pid
            for pid, g in pid_groups.items():
                dirtymaps = sorted(g['dirtymaps'])
                if not dirtymaps:
                    continue
                # pick latest DM file(s); we will process the last one as 'latest'
                dms = []
                parsed_dms = []  # list of (ts, track_ns, map)
                for dm in dirtymaps:
                    # parse timestamp from name
                    try:
                        base = os.path.basename(dm)
                        pid_s, ts_s = base.split('.', 1)[0].split('-', 1)
                        ts = int(ts_s)
                    except Exception:
                        ts = 0
                    try:
                        track_ns, m = parse_dirtymap_file(dm)
                        parsed_dms.append((ts, track_ns, m))
                    except Exception as e:
                        print('WARN: failed to parse dirtymap', dm, e)
                # sort by timestamp
                parsed_dms.sort(key=lambda x: x[0])
        # After processing the dirtymap_backups for this predump iteration, attempt to map decisions in
        # this predump to the dirtymap snapshot from the next iteration (if available). This adds an
        # explicit 'dm_file' field to decision rows which downstream build steps can use to deterministically
        # label samples. This avoids ambiguous cross-run aggregation and improves reproducibility.
        if idx + 1 < len(predump_dirs):
            next_iter = os.path.basename(predump_dirs[idx + 1])
            next_iter_dir = os.path.join(checkpoint_log, next_iter)

            # helper: find latest dirtymap file for a given pid within an iteration dir
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

            # gather all pids present in decisions for this predump
            pids = set([d.get('pid') for d in decisions_rows if d.get('predump_dir') == name and d.get('pid')])
            # map each pid to a dirtymap path if available and annotate matching decision rows
            for pid in pids:
                dm_file = find_latest_dirtymap_for_pid(next_iter_dir, pid)
                if not dm_file:
                    # fallback: search for any file under next_iter_dir matching pid
                    candidates = sorted(glob.glob(os.path.join(next_iter_dir, '**', f"{pid}-*.dirtymap"), recursive=True))
                    dm_file = candidates[-1] if candidates else None
                for d in decisions_rows:
                    if d.get('predump_dir') == name and d.get('pid') == pid:
                        d['dm_file'] = dm_file or ''

                # build per-file stats and optionally per-page CSV
                for (idx, (ts, track_ns, m)) in enumerate(parsed_dms):
                    counts = list(m.values())
                    heats = [ (c / (track_ns / 1e9)) if track_ns > 0 else 0.0 for c in counts ]
                    n_pages = len(counts)
                    zeros = sum(1 for c in counts if c == 0)
                    if n_pages == 0:
                        continue
                    # quantiles
                    counts_sorted = sorted(counts)
                    heats_sorted = sorted(heats)
                    def q(sorted_list, qp):
                        if not sorted_list:
                            return 0
                        k = (len(sorted_list)-1) * qp
                        f = math.floor(k)
                        c = math.ceil(k)
                        if f == c:
                            return sorted_list[int(k)]
                        d0 = sorted_list[int(f)] * (c-k)
                        d1 = sorted_list[int(c)] * (k-f)
                        return (d0 + d1)

                    row = {
                        'iter_dir': iter_name,
                        'predump_dir': os.path.basename(pred),
                        'pid': pid,
                        'timestamp': ts,
                        'track_duration_ns': track_ns,
                        'n_pages': n_pages,
                        'zero_writes': zeros,
                        'writes_min': counts_sorted[0],
                        'writes_p50': median(counts_sorted),
                        'writes_p75': q(counts_sorted, 0.75),
                        'writes_p90': q(counts_sorted, 0.90),
                        'writes_max': counts_sorted[-1],
                        'heat_p50': q(heats_sorted, 0.5),
                        'heat_p75': q(heats_sorted, 0.75),
                        'heat_p90': q(heats_sorted, 0.90),
                        'heat_max': heats_sorted[-1]
                    }

                    # warm and deferred context
                    warm_map = parse_warm_list_file(g.get('warm') or '') if g.get('warm') else {}
                    def_map = parse_deferred_file(g.get('deferred') or '') if g.get('deferred') else {}
                    row['warm_count'] = len(warm_map)
                    row['deferred_count'] = len(def_map)

                    # detect promoted pages by diffing previous warm set
                    prev = prev_warm.get(pid, set())
                    cur_warm = set(warm_map.keys())
                    promoted = cur_warm - prev
                    row['promoted_count'] = len(promoted)

                    # compute average heat among promoted before promotion (if available in earlier parsed_dms)
                    if promoted and idx > 0:
                        prev_ts, prev_track, prev_map = parsed_dms[idx-1]
                        promoted_heats = []
                        for a in promoted:
                            if a in prev_map:
                                promoted_heats.append(prev_map[a] / (prev_track / 1e9) if prev_track>0 else 0.0)
                        row['promoted_avg_heat_before'] = mean(promoted_heats) if promoted_heats else 0.0
                    else:
                        row['promoted_avg_heat_before'] = 0.0

                    agg_rows.append(row)

                    # record promoted histories (collect small time series across last few parsed_dms)
                    if promoted:
                        for a in promoted:
                            hist = {'pid': pid, 'addr': a, 'promoted_at': ts, 'series': []}
                            # collect up to 5 previous samples
                            start_idx = max(0, idx-5)
                            for j in range(start_idx, idx+1):
                                jts, jtrack, jmap = parsed_dms[j]
                                val = jmap.get(a, 0)
                                heat_val = val / (jtrack / 1e9) if jtrack>0 else 0.0
                                hist['series'].append((jts, val, heat_val))
                            promoted_histories.append(hist)

                    # update prev_warm for pid after processing last parsed_dm for this db
                    prev_warm[pid] = cur_warm

    # write CSV outputs
    out_dir = out_dir or run_dir
    agg_csv = os.path.join(out_dir, 'dirtymap_agg.csv')
    keys = ['predump_dir','iter_dir','pid','timestamp','track_duration_ns','n_pages','zero_writes','warm_count','deferred_count','promoted_count','promoted_avg_heat_before','writes_min','writes_p50','writes_p75','writes_p90','writes_max','heat_p50','heat_p75','heat_p90','heat_max']
    with open(agg_csv, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in agg_rows:
            w.writerow({k: r.get(k, '') for k in keys})

    hist_csv = os.path.join(out_dir, 'promoted_histories.csv')
    with open(hist_csv, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['pid','addr','promoted_at','series_ts_vals'])
        for h in promoted_histories:
            series_str = ';'.join([f"{t}:{v}:{heat:.3f}" for (t,v,heat) in h['series']])
            w.writerow([h['pid'], hex(h['addr']), h['promoted_at'], series_str])

    # write per-page decision logs CSV
    decisions_csv = os.path.join(out_dir, 'decisions.csv')

    # Filter out trivial DUMP decisions (heat==0 and heat_trend==0) to reduce noise and improve analysis accuracy.
    filtered_decisions = []
    filtered_count = 0
    for r in decisions_rows:
        if r.get('decision') == 'DUMP':
            h = r.get('heat')
            ht = r.get('heat_trend')
            try:
                if (h is not None and ht is not None) and float(h) == 0.0 and float(ht) == 0.0:
                    filtered_count += 1
                    continue
            except Exception:
                pass
        filtered_decisions.append(r)
    if filtered_count > 0:
        print(f"Filtered {filtered_count} trivial DUMP rows from decisions (heat/heat_trend == 0.0)")

    decisions_rows = filtered_decisions

    # Determine union of keys present in parsed decision rows and include optional telemetry fields
    union_keys = set()
    for r in decisions_rows:
        union_keys.update(r.keys())
    # Preferred ordering of fields (include telemetry fields p/score/pthr). The legacy
    # 'force_guarded' telemetry field has been removed from the runtime and is no
    # longer emitted — do not include it in newly generated CSV outputs even if
    # present in historic logs.
    preferred_order = ['predump_dir','pid','round','pre_dump','addr','decision','reason','dhm_present','heat','heat_trend','writes_est','track_s','deferred','is_warm','hist_count','hist_is_declining','hist_declining','hist_mean','hist_variance','p','p_model','score','pthr','thresholds','file','dm_file','line']

    # If legacy 'force_guarded' tokens are present in parsed decision rows, drop
    # them to avoid propagating removed telemetry into new analysis artifacts.
    if 'force_guarded' in union_keys:
        union_keys.discard('force_guarded')

    keys = [k for k in preferred_order if k in union_keys]
    # Append any remaining keys not in preferred_order
    for k in sorted(union_keys):
        if k not in keys:
            keys.append(k)
    with open(decisions_csv, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in decisions_rows:
            row = {}
            for k in keys:
                if k == 'addr' and r.get('addr') is not None:
                    row[k] = hex(r.get('addr'))
                else:
                    row[k] = r.get(k, '')
            w.writerow(row)

    # Write deferred events CSV
    defer_csv = os.path.join(out_dir, 'deferred_events.csv')
    with open(defer_csv, 'w', newline='') as f:
        keys = ['predump_dir','pid','addr','count','deferred_size','protected_until','file','line']
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for d in deferred_rows:
            row = { 'predump_dir': d.get('predump_dir',''), 'pid': d.get('pid',''), 'addr': hex(d.get('addr')) if d.get('addr') else '', 'count': d.get('count',''), 'deferred_size': d.get('deferred_size',''), 'protected_until': d.get('protected_until',''), 'file': d.get('file',''), 'line': d.get('line','') }
            w.writerow(row)

    # Write enforcement summary CSV (captures skipped_min / skipped_cooldown counts)
    enforce_csv = os.path.join(out_dir, 'enforce_summary.csv')
    with open(enforce_csv, 'w', newline='') as f:
        keys = ['predump_dir','pid','picked','skipped_min_count','skipped_cooldown_count','deferred_sum_after','file','line']
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for e in enforce_rows:
            row = { 'predump_dir': e.get('predump_dir',''), 'pid': e.get('pid',''), 'picked': e.get('picked',''), 'skipped_min_count': e.get('skipped_min_count',''), 'skipped_cooldown_count': e.get('skipped_cooldown_count',''), 'deferred_sum_after': e.get('deferred_sum_after',''), 'file': e.get('file',''), 'line': e.get('line','') }
            w.writerow(row)

    # Write deferred-skip summary CSV (captures pages skipped due to defer cooldown/min-count)
    defer_skip_csv = os.path.join(out_dir, 'deferred_skips.csv')
    with open(defer_skip_csv, 'w', newline='') as f:
        keys = ['predump_dir','pid','addr','deferred_count','last_round','current_round','skip_min','skip_cooldown','file','line']
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for ds in defer_skip_rows:
            row = { 'predump_dir': ds.get('predump_dir',''), 'pid': ds.get('pid',''), 'addr': hex(ds.get('addr')) if ds.get('addr') else '', 'deferred_count': ds.get('deferred_count',''), 'last_round': ds.get('last_round',''), 'current_round': ds.get('current_round',''), 'skip_min': ds.get('skip_min',''), 'skip_cooldown': ds.get('skip_cooldown',''), 'file': ds.get('file',''), 'line': ds.get('line','') }
            w.writerow(row)

    # Write enforcement summary CSV (captures skipped_min / skipped_cooldown counts)
    enforce_csv = os.path.join(out_dir, 'enforce_summary.csv')
    with open(enforce_csv, 'w', newline='') as f:
        keys = ['predump_dir','pid','picked','skipped_min_count','skipped_cooldown_count','deferred_sum_after','file','line']
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for e in enforce_rows:
            row = { 'predump_dir': e.get('predump_dir',''), 'pid': e.get('pid',''), 'picked': e.get('picked',''), 'skipped_min_count': e.get('skipped_min_count',''), 'skipped_cooldown_count': e.get('skipped_cooldown_count',''), 'deferred_sum_after': e.get('deferred_sum_after',''), 'file': e.get('file',''), 'line': e.get('line','') }
            w.writerow(row)

    # Write deferred-skip summary CSV (captures pages skipped due to defer cooldown/min-count)
    defer_skip_csv = os.path.join(out_dir, 'deferred_skips.csv')
    with open(defer_skip_csv, 'w', newline='') as f:
        keys = ['predump_dir','pid','addr','deferred_count','protected_until','current_round','skip_min','skip_cooldown','file','line']
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for ds in defer_skip_rows:
            row = { 'predump_dir': ds.get('predump_dir',''), 'pid': ds.get('pid',''), 'addr': hex(ds.get('addr')) if ds.get('addr') else '', 'deferred_count': ds.get('deferred_count',''), 'protected_until': ds.get('protected_until',''), 'current_round': ds.get('current_round',''), 'skip_min': ds.get('skip_min',''), 'skip_cooldown': ds.get('skip_cooldown',''), 'file': ds.get('file',''), 'line': ds.get('line','') }
            w.writerow(row)

    print('Wrote:', agg_csv)
    print('Wrote:', hist_csv)
    print('Wrote:', decisions_csv)
    print('Wrote:', defer_csv)
    print('Wrote:', enforce_csv)
    print('Wrote:', defer_skip_csv)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description='Parse dirtymap/warm/deferred artifacts and summarize')
    p.add_argument('--run', '-r', help='Path to run directory (e.g., /tmp/exp/.../experiments/B/run_1)')
    p.add_argument('--telegrid', '-t', help='Path to a telegrid directory (e.g., /tmp/exp/telegrid_encourage_defer_...) to analyze the latest run under it')
    args = p.parse_args()

    try:
        if args.run:
            run_dir = find_latest_run(args.run)
        elif args.telegrid:
            run_dir = find_latest_run(args.telegrid)
        else:
            print('ERROR: must specify --run or --telegrid to target a specific test (avoids scanning all /tmp/exp)')
            sys.exit(2)
    except Exception as e:
        print('ERROR:', e)
        sys.exit(1)

    print('Analyzing run:', run_dir)
    try:
        summarize_run(run_dir)
    except Exception as e:
        print('ERROR during analysis:', e)
        raise
