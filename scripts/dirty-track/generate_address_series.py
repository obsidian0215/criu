#!/usr/bin/env python3
"""Generate per-address time series for pages that were DEFER then FORCE_SEND.

Usage:
  ./scripts/generate_address_series.py --run <run_dir> [--out <out_dir>] [--top N]

Outputs (under run_dir by default):
 - problem_pages_time_series.csv  (one row per addr per iteration)
 - problem_pages_summary.csv      (one row per problematic addr summary)
"""

import os
import sys
import glob
import csv
import struct
import argparse
from collections import defaultdict

ENTRY_DM_STRUCT = '<QI'  # address (8), write_count (4)
ENTRY_DM_SIZE = struct.calcsize(ENTRY_DM_STRUCT)


def parse_dirtymap_file(path):
    res = {}
    track_ns = 0
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
            for off in range(0, len(data), ENTRY_DM_SIZE):
                addr, writes = struct.unpack_from(ENTRY_DM_STRUCT, data, off)
                res[addr] = writes
    except Exception as e:
        # tolerate missing/invalid files
        return None, {}
    return track_ns, res


def parse_warm_list_file(path):
    res = set()
    if not os.path.exists(path):
        return res
    try:
        with open(path, 'rb') as f:
            data = f.read()
            # ENTRY_WARM_STRUCT = '<Qb'  (8 + 1) but file may be padded
            rec_size = 9
            if len(data) % rec_size != 0:
                rem = len(data) % rec_size
                data = data[:len(data)-rem]
            for off in range(0, len(data), rec_size):
                addr, scount = struct.unpack_from('<Qb', data, off)
                res.add(addr)
    except Exception:
        pass
    return res


def find_latest_dirtymap_for_pid(iter_dir, pid):
    # iter_dir is e.g. /.../checkpoint_log/predump_1
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
            candidates.append((ts, fn, d))
    if not candidates:
        return None, None
    candidates.sort(key=lambda x: x[0])
    return candidates[-1][1], candidates[-1][2]  # file, containing backup dir


def list_iterations(checkpoint_log):
    preds = sorted([d for d in glob.glob(os.path.join(checkpoint_log, 'predump_*')) if os.path.isdir(d)])
    iters = [os.path.basename(p) for p in preds]
    # append dump_log as final if exists
    dumpd = os.path.join(checkpoint_log, 'dump_log')
    if os.path.isdir(dumpd):
        iters.append('dump_log')
    return iters, preds, dumpd


def load_decisions(run_dir):
    dec_file = os.path.join(run_dir, 'decisions.csv')
    if not os.path.exists(dec_file):
        return []
    rows = []
    with open(dec_file, 'r') as f:
        r = csv.DictReader(f)
        for row in r:
            # normalize
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


def build_iter_map(checkpoint_log):
    iters, preds, dumpd = list_iterations(checkpoint_log)
    name_to_idx = {}
    for i, name in enumerate(iters):
        name_to_idx[name] = i
    # also accept 'predump_0' etc -> index mapping
    return iters, name_to_idx


def generate_series_for_run(run_dir, out_dir=None, top=None):
    out_dir = out_dir or run_dir
    checkpoint_log = os.path.join(run_dir, 'checkpoint_log')
    if not os.path.isdir(checkpoint_log):
        print('No checkpoint_log in', run_dir)
        return
    decisions = load_decisions(run_dir)
    if not decisions:
        print('No decisions.csv or empty for', run_dir)
        return

    iters, name_to_idx = build_iter_map(checkpoint_log)
    # group decisions per address
    addr_events = defaultdict(list)
    for d in decisions:
        addr_events[d['addr_int']].append(d)

    # find addresses with DEFER then FORCE_DUMP (or legacy FORCE_SEND) (with later index)
    problem_addrs = []
    for addr, events in addr_events.items():
        defer_idxs = []
        force_idxs = []
        for ev in events:
            pd = ev.get('predump_dir')
            if pd not in name_to_idx:
                # try common alternative
                if pd == 'dump' and 'dump_log' in name_to_idx:
                    idx = name_to_idx['dump_log']
                else:
                    continue
            else:
                idx = name_to_idx[pd]
            if ev.get('decision') == 'DEFER':
                defer_idxs.append(idx)
            if ev.get('decision') in ('FORCE_SEND','FORCE_DUMP'):
                force_idxs.append(idx)
        if defer_idxs and force_idxs and max(force_idxs) >= min(defer_idxs):
            problem_addrs.append(addr)

    if not problem_addrs:
        print('No DEFER->FORCE addresses found in', run_dir)
        return

    # limit if requested
    if top:
        problem_addrs = problem_addrs[:top]

    # prepare time series output
    ts_out = os.path.join(out_dir, 'problem_pages_time_series.csv')
    sum_out = os.path.join(out_dir, 'problem_pages_summary.csv')

    with open(ts_out, 'w', newline='') as fts, open(sum_out, 'w', newline='') as fs:
        tw = csv.writer(fts)
        tw.writerow(['addr','addr_hex','pid','iter_idx','iter_name','dm_ts','track_duration_ns','writes','heat','in_warm','deferred_count','decision','decision_reason','decision_heat','decision_is_warm','decision_p','decision_score','decision_pthr'])
        sw = csv.writer(fs)
        sw.writerow(['addr','addr_hex','pid','defer_count','force_count','first_defer_iter','first_force_iter','avg_heat_at_defer','avg_heat_at_force','mean_heat_timeseries','notes'])

        for addr in problem_addrs:
            addr_hex = hex(addr)
            events = sorted(addr_events[addr], key=lambda x: (name_to_idx.get(x.get('predump_dir'), 9999), x.get('round',0)))
            # pick pid from first event
            pid = events[0].get('pid')
            if pid is None or pid == '':
                # fallback to any present
                pid = events[0].get('pid')
            try:
                pid_i = int(pid)
            except:
                pid_i = None
            defer_count = sum(1 for e in events if e.get('decision')=='DEFER')
            force_count = sum(1 for e in events if e.get('decision') in ('FORCE_SEND','FORCE_DUMP'))
            first_defer_iter = None
            first_force_iter = None
            defer_heats = []
            force_heats = []
            timeseries_heats = []

            # iterate over iterations and collect dm/warm/deferred
            for iter_idx, iter_name in enumerate(iters):
                iter_dir = os.path.join(checkpoint_log, iter_name)
                dm_file, backup_dir = find_latest_dirtymap_for_pid(iter_dir, pid_i) if pid_i else (None, None)
                track_ns, dm_map = (0, {})
                if dm_file:
                    track_ns, dm_map = parse_dirtymap_file(dm_file)
                writes = dm_map.get(addr, 0)
                heat = 0.0
                if track_ns and track_ns > 0:
                    heat = writes / (track_ns / 1e9)
                timeseries_heats.append(heat)
                # warm / deferred
                warm_file = None
                deferred_cnt = 0
                # warm list typically in same backup_dir as dm_file (if found)
                if backup_dir:
                    # warm_list.<pid> if exists
                    wl = os.path.join(backup_dir, f'warm_list.{pid_i}')
                    if os.path.exists(wl):
                        in_warm = addr in parse_warm_list_file(wl)
                    else:
                        in_warm = False
                    dl = os.path.join(backup_dir, f'deferred_list.{pid_i}')
                    if os.path.exists(dl):
                        # deferred entries are (addr, count) -> we try to read and find count
                        try:
                            with open(dl, 'rb') as df:
                                data = df.read()
                                rec = 9
                                found = False
                                for off in range(0, len(data), rec):
                                    a, c = struct.unpack_from('<QB', data, off)
                                    if a == addr:
                                        deferred_cnt = c
                                        found = True
                                        break
                        except Exception:
                            deferred_cnt = 0
                else:
                    in_warm = False

                # find decisions (maybe multiple) for this iter
                decision = ''
                decision_reason = ''
                decision_heat = ''
                decision_delta = ''
                decision_is_warm = ''
                decision_p = ''
                decision_score = ''
                decision_pthr = ''
                for e in events:
                    ev_iter = e.get('predump_dir')
                    if ev_iter not in name_to_idx:
                        if ev_iter == 'dump' and 'dump_log' in name_to_idx:
                            ev_idx = name_to_idx['dump_log']
                        else:
                            continue
                    else:
                        ev_idx = name_to_idx[ev_iter]
                    if ev_idx == iter_idx:
                        # annotate first decision of this iter (there may be multiple)
                        decision = e.get('decision')
                        decision_reason = e.get('reason')
                        decision_heat = e.get('heat')
                        decision_is_warm = e.get('is_warm')
                        decision_p = e.get('p') if e.get('p') is not None else ''
                        decision_score = e.get('score') if e.get('score') is not None else ''
                        decision_pthr = e.get('pthr') if e.get('pthr') is not None else ''
                        if decision == 'DEFER' and first_defer_iter is None:
                            first_defer_iter = iter_idx
                            try:
                                defer_heats.append(float(e.get('heat') or 0.0))
                            except Exception:
                                pass
                        if decision in ('FORCE_SEND','FORCE_DUMP') and first_force_iter is None:
                            first_force_iter = iter_idx
                            try:
                                force_heats.append(float(e.get('heat') or 0.0))
                            except Exception:
                                pass
                        # don't break; capture last decision in iter if later ones exist

                tw.writerow([addr, addr_hex, pid_i, iter_idx, iter_name, os.path.basename(dm_file) if dm_file else '', track_ns or '', writes, "%.6f" % heat, 1 if in_warm else 0, deferred_cnt, decision, decision_reason, decision_heat, decision_delta, decision_is_warm, decision_p, decision_score, decision_pthr])

            mean_ts_heat = sum(timeseries_heats)/len(timeseries_heats) if timeseries_heats else 0.0
            avg_def = sum(defer_heats)/len(defer_heats) if defer_heats else 0.0
            avg_for = sum(force_heats)/len(force_heats) if force_heats else 0.0
            notes = ''
            if avg_def > 0 and avg_for > 0 and avg_for >= avg_def:
                notes = 'still_hot_on_force'
            sw.writerow([addr, addr_hex, pid_i, defer_count, force_count, first_defer_iter if first_defer_iter is not None else '', first_force_iter if first_force_iter is not None else '', "%.3f" % avg_def, "%.3f" % avg_for, "%.6f" % mean_ts_heat, notes])

    print('Wrote:', ts_out)
    print('Wrote:', sum_out)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description='Generate address time series for DEFER->FORCE pages')
    p.add_argument('--run', '-r', required=True, help='Path to run directory (e.g., /tmp/exp/.../B/run_1)')
    p.add_argument('--out', help='Output directory (default: run dir)')
    p.add_argument('--top', type=int, help='Limit to top N problematic addresses')
    args = p.parse_args()
    generate_series_for_run(args.run, args.out, args.top)
