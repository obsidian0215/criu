#!/usr/bin/env python3
"""Post-process telegrid outputs and generate detailed analysis:
- decisions summary per run (counts by decision type)
- per-iteration pages (tracked n_pages sum, predump/dump pages)
- skip accuracy per iteration (DEFER correctness only)
- consolidated problem pages (DEFER->FORCE) summary
- anomaly report summarizing runs with large B vs A regressions or many FORCE_SENDs

Usage: python3 scripts/dirty-track/postprocess_grid.py [--root /tmp/exp] [--out analysis]
"""

import argparse
import csv
import glob
import json
import os
import sys
from collections import defaultdict, Counter
from pathlib import Path
import struct

ENTRY_DM_STRUCT = '<QI'
ENTRY_DM_SIZE = struct.calcsize(ENTRY_DM_STRUCT)
ENTRY_DEF_STRUCT = '<QB'   # address (8) + unsigned char count (1)
ENTRY_DEF_SIZE = struct.calcsize(ENTRY_DEF_STRUCT)


def parse_decisions_csv(path):
    if not path.exists():
        return []
    rows = []
    with open(path, 'r', newline='') as f:
        r = csv.DictReader(f)
        for row in r:
            if not row.get('addr'):
                continue
            try:
                row['addr_int'] = int(row['addr'], 0)
            except Exception:
                try:
                    row['addr_int'] = int(row['addr'], 16)
                except Exception:
                    row['addr_int'] = None
            for k in ['round','pre_dump','dhm_present','deferred','is_warm']:
                if k in row and row[k] != '':
                    try:
                        row[k] = int(row[k])
                    except:
                        pass
            for k in ['heat','heat_trend']:
                if k in row and row[k] != '':
                    try:
                        row[k] = float(row[k])
                    except:
                        pass
            rows.append(row)
    return rows


def parse_dirtymap_agg(path):
    # returns list of dict rows from dirtymap_agg.csv
    if not path.exists():
        return []
    rows = []
    with open(path, 'r', newline='') as f:
        r = csv.DictReader(f)
        for row in r:
            # convert numeric fields
            for k in ['track_duration_ns','n_pages','zero_writes','warm_count','deferred_count','promoted_count']:
                if k in row and row[k] != '':
                    try:
                        row[k] = int(row[k])
                    except:
                        pass
            for k in ['promoted_avg_heat_before','writes_min','writes_p50','writes_p75','writes_p90','writes_max','heat_p50','heat_p75','heat_p90','heat_max']:
                if k in row and row[k] != '':
                    try:
                        row[k] = float(row[k])
                    except:
                        pass
            rows.append(row)
    return rows


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
            candidates.append((ts, fn, d))
    if not candidates:
        return None, None
    candidates.sort(key=lambda x: x[0])
    return candidates[-1][1], candidates[-1][2]


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


def analyze_run(run_dir):
    run_dir = Path(run_dir)
    out = {}
    decisions = parse_decisions_csv(run_dir / 'decisions.csv')

    # Fallback: if no per-page decision logs, synthesize DEFER entries from deferred artifacts
    checkpoint_log = run_dir / 'checkpoint_log'
    synthesized = []
    if not decisions:
        # Try deferred_events.csv (text CSV produced by parse_dirtymap.py)
        def_csv = run_dir / 'deferred_events.csv'
        if def_csv.exists():
            try:
                with open(def_csv, 'r', newline='') as df:
                    rdr = csv.DictReader(df)
                    for row in rdr:
                        if not row.get('addr'):
                            continue
                        try:
                            addr_int = int(row.get('addr'), 0)
                        except Exception:
                            try:
                                addr_int = int(row.get('addr'), 16)
                            except Exception:
                                addr_int = None
                        if addr_int is None:
                            continue
                        try:
                            pid = int(row.get('pid') or 0)
                        except Exception:
                            pid = 0
                        pd = row.get('predump_dir') or ''
                        # unify: treat synthesized deferred events as DEFER
                        synthesized.append({'predump_dir': pd, 'pid': pid, 'addr_int': addr_int, 'decision': 'DEFER', 'reason': 'deferred_events_csv'})
            except Exception:
                pass

        # If still nothing, search for binary deferred_list.* files under checkpoint_log
        if not synthesized and checkpoint_log.exists():
            for pred in sorted([p for p in checkpoint_log.glob('predump_*') if p.is_dir()]):
                for fn in glob.glob(os.path.join(str(pred), '**', 'deferred_list.*'), recursive=True):
                    try:
                        with open(fn, 'rb') as fh:
                            data = fh.read()
                            if not data or len(data) == 0:
                                continue
                            rem = len(data) % ENTRY_DEF_SIZE
                            if rem:
                                data = data[:len(data)-rem]
                            for off in range(0, len(data), ENTRY_DEF_SIZE):
                                addr, count = struct.unpack_from(ENTRY_DEF_STRUCT, data, off)
                                base = os.path.basename(fn)
                                pid = 0
                                try:
                                    parts = base.split('.', 1)
                                    if len(parts) > 1:
                                        pid = int(parts[1])
                                except Exception:
                                    pid = 0
                                # unify: treat deferred_list binary entries as DEFER
                            synthesized.append({'predump_dir': os.path.basename(pred), 'pid': pid, 'addr_int': addr, 'decision': 'DEFER', 'reason': 'deferred_list_bin', 'count': count})
                    except Exception:
                        continue

    if synthesized:
        decisions = synthesized
        out['synthesized_from_deferred'] = True
    else:
        out['synthesized_from_deferred'] = False

    # Normalize deprecated decision labels (map to canonical 'DEFER')
    for d in decisions:
        dec = d.get('decision') or 'UNKNOWN'
        if dec == 'SKIP_PARENT' or dec == 'SKIP_HOT':
            d['decision'] = 'DEFER'

    out['decision_counts'] = Counter([d.get('decision') or 'UNKNOWN' for d in decisions])

    # per-iteration decisions
    iters = []
    checkpoint_log = run_dir / 'checkpoint_log'
    if checkpoint_log.exists():
        preds = sorted([p for p in checkpoint_log.glob('predump_*') if p.is_dir()])
        iters = [p.name for p in preds]
        if (checkpoint_log / 'dump_log').exists():
            iters.append('dump_log')
    out['iters'] = iters

    # per-iteration decision counts
    per_iter_counts = defaultdict(Counter)
    for d in decisions:
        dec = d.get('decision') or 'UNKNOWN'
        pd = d.get('predump_dir')
        per_iter_counts[pd][dec] += 1
    out['per_iter_counts'] = per_iter_counts

    # per-iteration skip accuracy (DEFER only) -> evaluate against next iter dirtymap
    skip_types = set(['DEFER'])
    per_iter_skip_eval = {}
    name_to_idx = {n:i for i,n in enumerate(iters)}

    # Track per-run unique SKIP/SEND (by pid+addr) and last predump index for each skipped addr
    unique_skips = set()
    unique_sends = set()
    last_predump_idx = {}
    # build unique per-iter skip entries
    skips_by_iter = defaultdict(set)
    for d in decisions:
        dec = d.get('decision')
        pid = int(d.get('pid') or 0)
        addr = d.get('addr_int')
        pd = d.get('predump_dir')
        if addr is None:
            continue
        if dec == 'SEND':
            unique_sends.add((pid, addr))
        if dec in skip_types:
            unique_skips.add((pid, addr))
            if pd not in name_to_idx:
                if pd == 'dump' and 'dump_log' in name_to_idx:
                    idx = name_to_idx['dump_log']
                else:
                    continue
            else:
                idx = name_to_idx[pd]
            # record last seen predump index for (pid,addr)
            key = (pid, addr)
            if last_predump_idx.get(key, -1) < idx:
                last_predump_idx[key] = idx
            # map this skip to the next iteration for per-iter checking
            next_idx = idx + 1
            if next_idx >= len(iters):
                continue
            next_iter_name = iters[next_idx]
            skips_by_iter[(pd, pid, next_iter_name)].add(addr)

    # evaluate each skip group by loading next_iter dirtymap per pid (unique addrs only)
    for (pd, pid, next_iter_name), addrs in skips_by_iter.items():
        next_iter_dir = checkpoint_log / next_iter_name
        dm_file, backup_dir = find_latest_dirtymap_for_pid(str(next_iter_dir), pid)
        track_ns, dm_map = (None, {})
        if dm_file:
            track_ns, dm_map = parse_dirtymap_file(dm_file)
        addrs_set = set([a for a in addrs if a is not None])
        total = len(addrs_set)
        correct = 0
        incorrect = 0
        for addr in addrs_set:
            if dm_map and track_ns and track_ns > 0:
                writes = dm_map.get(addr, 0)
                if writes > 0:
                    incorrect += 1
                else:
                    correct += 1
            else:
                # missing dirtymap treated as correct (missing typically implies heat=0)
                correct += 1
        per_iter_skip_eval[(pd, next_iter_name)] = {'total': total, 'correct': correct, 'incorrect': incorrect, 'accuracy': (correct/total*100.0 if total>0 else None)}

    # Per-run predicted saved pages: examine each unique skipped (pid,addr) at its last skip and
    # check subsequent iterations for writes. By default missing dirtymaps are treated as no writes
    # (assume saved); also compute a strict count that requires at least one valid dirtymap
    saved_assume_missing_zero = 0
    saved_strict = 0
    unknown_addrs = 0
    for (pid, addr), last_idx in last_predump_idx.items():
        start_idx = last_idx + 1
        if start_idx >= len(iters):
            # no subsequent iterations -> saved
            saved_assume_missing_zero += 1
            saved_strict += 1
            continue
        found_write = False
        seen_valid_dm = False
        for ni in range(start_idx, len(iters)):
            iter_name = iters[ni]
            iter_dir = checkpoint_log / iter_name
            dm_file, backup_dir = find_latest_dirtymap_for_pid(str(iter_dir), pid)
            if not dm_file:
                # missing dirtymap -> treat as no writes for assume_missing_zero
                continue
            track_ns, dm_map = parse_dirtymap_file(dm_file)
            if not dm_map or track_ns is None:
                # parsed but invalid: treat like missing
                continue
            seen_valid_dm = True
            if dm_map.get(addr, 0) > 0:
                found_write = True
                break
        if found_write:
            continue
        if seen_valid_dm:
            saved_assume_missing_zero += 1
            saved_strict += 1
        else:
            # no valid dirtymaps at all after this skip
            # per default behavior, treat as saved (missing->0); for strict mode this would be unknown
            saved_assume_missing_zero += 1
            unknown_addrs += 0

    out['per_iter_skip_eval'] = per_iter_skip_eval
    out['unique_skips'] = len(unique_skips)
    out['unique_sends'] = len(unique_sends)
    out['saved_assume_missing_zero'] = saved_assume_missing_zero
    out['saved_strict'] = saved_strict
    out['unknown_addrs'] = unknown_addrs
    out['skip_rate_unique'] = (float(len(unique_skips))/(len(unique_skips)+len(unique_sends))) if (len(unique_skips)+len(unique_sends))>0 else None
    out['per_iter_skip_eval'] = per_iter_skip_eval

    it_metrics = run_dir / 'iteration_metrics.csv'
    it_metrics_rows = []
    if it_metrics.exists():
        with open(it_metrics) as f:
            r = csv.reader(f)
            header = next(r)
            for row in r:
                it_metrics_rows.append(dict(zip(header, row)))
    out['iteration_metrics'] = it_metrics_rows

    # Prefer iteration_metrics per-iteration page counts when present; otherwise fall back to dirtymap_agg
    pages_by_iter = defaultdict(int)
    heat_stats_by_iter = defaultdict(list)
    if it_metrics_rows:
        for row in it_metrics_rows:
            stage = row.get('stage','').strip()
            iter_name = row.get('iter','').strip()
            pages = None
            if row.get('pages_transferred') and row.get('pages_transferred').strip() != '':
                try:
                    pages = int(float(row['pages_transferred']))
                except Exception:
                    pages = None
            if stage == 'predump' and iter_name.isdigit():
                pages_by_iter[f'predump_{iter_name}'] = pages or 0
            elif stage == 'dump':
                # treat dump/final as dump_log
                pages_by_iter['dump_log'] = pages or 0
    else:
        dmag = parse_dirtymap_agg(run_dir / 'dirtymap_agg.csv')
        for r in dmag:
            pd = r.get('predump_dir')
            n = r.get('n_pages') or 0
            pages_by_iter[pd] += n
            if 'heat_max' in r and r['heat_max'] is not None:
                heat_stats_by_iter[pd].append(r['heat_max'])

    # authoritative per-run totals (prefer these for run-level summaries)
    tpp = run_dir / 'total_predump_pages.txt'
    out['total_predump_pages'] = None
    if tpp.exists():
        txt = tpp.read_text().strip()
        if txt.isdigit():
            out['total_predump_pages'] = int(txt)
    tp = run_dir / 'total_pages.txt'
    out['total_pages'] = None
    if tp.exists():
        txt = tp.read_text().strip()
        if txt.isdigit():
            out['total_pages'] = int(txt)

    out['pages_by_iter'] = pages_by_iter
    out['heat_stats_by_iter'] = {k: (max(v) if v else 0.0) for k,v in heat_stats_by_iter.items()}

    # dump pages
    dp = run_dir / 'dump_pages.txt'
    out['dump_pages'] = int(dp.read_text().strip()) if dp.exists() and dp.read_text().strip().isdigit() else None

    # iteration_metrics parsing moved earlier (used for authoritative iteration page counts)

    # prediction_summary
    ps = run_dir / 'prediction_summary.txt'
    pred = {}
    if ps.exists():
        for line in ps.read_text().splitlines():
            if '=' in line:
                k,v=line.split('=',1)
                pred[k.strip()]=v.strip()
    out['prediction_summary'] = pred

    # problem pages
    pps = run_dir / 'problem_pages_summary.csv'
    problem_rows = []
    if pps.exists():
        with open(pps) as f:
            r = csv.DictReader(f)
            for row in r:
                problem_rows.append(row)
    out['problem_pages'] = problem_rows


    return out


def find_telegrids(root):
    return sorted([p for p in Path(root).glob('telegrid_*') if p.is_dir()])


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--root', default='/tmp/exp')
    p.add_argument('--telegrid', default=None, help='Path to a specific telegrid dir to analyze (e.g., /tmp/exp/telegrid_encourage_defer_...)')
    p.add_argument('--all', action='store_true', help='Process all telegrids under --root')
    p.add_argument('--combine', action='store_true', help='Also produce combined CSVs across telegrids (default: off)')
    p.add_argument('--out', default='analysis')
    p.add_argument('--regen-decisions', action='store_true', help='Regenerate decision summary CSV for targeted telegrid(s)')
    p.add_argument('--regen-skip-accuracy', action='store_true', help='Regenerate skip accuracy CSV for targeted telegrid(s)')
    p.add_argument('--regen-iter-pages', action='store_true', help='Regenerate iteration pages CSV for targeted telegrid(s)')
    p.add_argument('--regen-run-timing', action='store_true', help='Regenerate run timing CSV for targeted telegrid(s)')
    p.add_argument('--no-skip-variant', action='store_true', help='Do not skip entire variant when any run failed; instead skip only failed runs')
    args = p.parse_args()
    root = Path(args.root)
    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    # Require explicit target to avoid accidental full-folder scans
    if args.telegrid:
        tg_path = Path(args.telegrid)
        if not tg_path.exists():
            print('ERROR: specified telegrid not found:', args.telegrid)
            sys.exit(1)
        telegrids = [tg_path]
    elif args.all:
        telegrids = find_telegrids(root)
        if not telegrids:
            print('No telegrids found under', root)
            sys.exit(0)
    else:
        print('ERROR: must specify --telegrid <path> to analyze a specific test or --all to process all telegrids')
        sys.exit(2)
    # Process each telegrid individually and write per-telegrid outputs under outdir/<telegrid.name>/
    for tg in telegrids:
        parts = tg.name.split('_')
        container = 'unknown'
        config = tg.name
        if parts[0] == 'telegrid' and len(parts) >= 5:
            # Format: telegrid_{config}_{container}_{ts}_{pid}
            container = parts[-3]
            config = '_'.join(parts[1:-3])
        elif parts[0] == 'telegrid' and len(parts) >= 4:
            # legacy format without container: telegrid_{config}_{ts}_{pid}
            config = '_'.join(parts[1:-2])

        # prepare per-telegrid output dir
        out_tg_dir = outdir / tg.name
        out_tg_dir.mkdir(parents=True, exist_ok=True)

        # Per-telegrid files
        decision_summary_csv = out_tg_dir / 'grid_decision_summary.csv'
        iter_pages_csv = out_tg_dir / 'grid_iteration_pages.csv'
        skip_acc_csv = out_tg_dir / 'grid_skip_accuracy.csv'
        skip_summary_csv = out_tg_dir / 'grid_skip_summary.csv'
        problem_pages_file = out_tg_dir / 'grid_problem_pages_combined.csv'
        anomalies_file = out_tg_dir / 'grid_anomaly_report.txt'
        run_timing_csv = out_tg_dir / 'grid_run_timing.csv'

        # regeneration mode: when specific --regen flags are provided, run in selective mode
        selective_regen = (args.regen_decisions or args.regen_skip_accuracy or args.regen_iter_pages or args.regen_run_timing)
        gen_decisions = args.regen_decisions or not selective_regen
        gen_skip_acc = args.regen_skip_accuracy or not selective_regen
        # when selective_regen is True, only regenerate requested artifacts
        gen_iter_pages = args.regen_iter_pages or not selective_regen
        gen_run_timing = args.regen_run_timing or not selective_regen

        # Open only the CSVs requested (selective regeneration when flags provided); otherwise overwrite all
        dsf = None; dsw = None
        ipf = None; ipw = None
        sacf = None; saw = None
        ssumf = None; ssumw = None
        ppf = None; ppw = None
        anf = None; rtf = None; rtw = None
        try:
            if gen_decisions:
                dsf = open(decision_summary_csv, 'w', newline='')
                dsw = csv.writer(dsf)
                dsw.writerow(['telegrid','container','config','variant','run','mode','decision','count'])
            if gen_iter_pages:
                ipf = open(iter_pages_csv, 'w', newline='')
                ipw = csv.writer(ipf)
                ipw.writerow(['telegrid','container','config','variant','run','mode','iter','pages_tracked','heat_max','dump_pages','total_checkpoint_ms'])
            if gen_skip_acc:
                sacf = open(skip_acc_csv, 'w', newline='')
                saw = csv.writer(sacf)
                saw.writerow(['telegrid','container','config','variant','run','mode','predump_dir','next_iter','total_skipped','correct','incorrect','accuracy'])
                # per-run skip summary (unique-address based)
                ssumf = open(skip_summary_csv, 'w', newline='')
                ssumw = csv.writer(ssumf)
                ssumw.writerow(['telegrid','container','config','variant','run','mode','unique_skips','unique_sends','skip_rate_unique','saved_assume_missing_zero','saved_strict','unknown_addrs','predump_pages','dump_pages','total_pages'])
            # run-level timing summary (only when requested)
            if gen_run_timing:
                rtf = open(run_timing_csv, 'w', newline='')
                rtw = csv.writer(rtf)
                rtw.writerow(['telegrid','container','config','variant','run','mode','total_predump_ms','dump_ms','total_transfer_ms','total_migration_ms','downtime_ms','bandwidth_mbps','total_pages','predump_pages','dump_pages'])
            # problem pages and anomalies: only write when not in selective_regen mode
            if not selective_regen:
                ppf = open(problem_pages_file, 'w', newline='')
                anf = open(anomalies_file, 'w')
            else:
                # in selective mode we avoid touching problem/anomaly files
                ppf = None
                anf = None
            ppw = None

            # iterate experiments and runs inside this telegrid only
            # Support legacy layout (telegrid/experiments/<variant>/run_*) and new layout (telegrid/<container>/<variant>/run_*)
            exps = tg / 'experiments'
            variant_roots = []
            if exps.exists():
                # legacy: telegrid/experiments/<variant>
                for variant_dir in sorted(exps.iterdir()):
                    if not variant_dir.is_dir():
                        continue
                    variant_roots.append((None, variant_dir))
            else:
                # new: telegrid/<container>/<variant>
                for container_dir in sorted([p for p in tg.iterdir() if p.is_dir()]):
                    for variant_dir in sorted([d for d in container_dir.iterdir() if d.is_dir()]):
                        variant_roots.append((container_dir.name, variant_dir))

            # Optionally load telegrid-level run metadata (if present) for faster mapping of runs -> container/variant
            meta_file = tg / 'run_meta.json'
            meta_map = {}
            if meta_file.exists():
                try:
                    entries = json.loads(meta_file.read_text())
                    for e in entries:
                        try:
                            key = str(Path(e.get('run_dir','')).resolve())
                        except Exception:
                            key = e.get('run_dir','')
                        meta_map[key] = e
                except Exception as ex:
                    print('Warning: failed to parse run_meta.json:', ex)

            for container_name, variant_dir in variant_roots:
                variant = variant_dir.name
                # Process runs individually. If a run failed (abort_reason present or non-zero exitcode),
                # skip that run only rather than skipping the entire variant to avoid losing partial results.
                # Expected layout: <variant>/<config>/run_*
                run_paths = []
                # direct run_* under variant (legacy support)
                run_paths.extend(sorted(variant_dir.glob('run_*')))
                # config subdirectories under variant (preferred)
                for cfg_dir in sorted([d for d in variant_dir.iterdir() if d.is_dir() and not d.name.startswith('run_')]):
                    run_paths.extend(sorted(cfg_dir.glob('run_*')))
                for run_dir in sorted(run_paths):
                    # Skip an individual run if it failed
                    run_failed = False
                    if (run_dir / 'abort_reason.txt').exists():
                        run_failed = True
                    elif (run_dir / 'exitcode.txt').exists():
                        try:
                            if (run_dir / 'exitcode.txt').read_text().strip() != '0':
                                run_failed = True
                        except Exception:
                            run_failed = True
                    if run_failed:
                        print(f"Skipping run {run_dir.name} in variant {variant} due to failed run")
                        continue
                    # prefer run-local or telegrid meta mapping for metadata
                    run_container = None
                    run_config = None
                    try:
                        meta_key = str(run_dir.resolve())
                        if meta_key in meta_map:
                            meta_entry = meta_map[meta_key]
                            run_container = meta_entry.get('container')
                            run_config = meta_entry.get('config_label')
                    except Exception:
                        pass

                    if not run_container:
                        if (run_dir / 'container.txt').exists():
                            run_container = (run_dir / 'container.txt').read_text().strip()
                        else:
                            run_container = (container_name if container_name else container)

                    if not run_config:
                        if (run_dir / 'config_label.txt').exists():
                            run_config = (run_dir / 'config_label.txt').read_text().strip()
                        else:
                            run_config = config

                    # Read explicit image path for auditing if present
                    run_image = None
                    if (run_dir / 'image_path.txt').exists():
                        run_image = (run_dir / 'image_path.txt').read_text().strip()

                    res = analyze_run(run_dir)
                    # Determine the authoritative variant folder (the directory under telegrid root that contains this run)
                    try:
                        run_variant = next(p.name for p in run_dir.parents if p.parent == tg)
                    except StopIteration:
                        run_variant = variant_dir.name

                    # Normalize run mode and base config (eval_<mode> -> config=<image_variant>, mode=<mode>)
                    run_mode = ''
                    config_base = run_config
                    if run_config and run_config.startswith('eval_'):
                        run_mode = run_config.split('eval_',1)[1]
                        ivf = run_dir / 'image_variant.txt'
                        if ivf.exists():
                            try:
                                config_base = ivf.read_text().strip()
                            except Exception:
                                pass

                    # decision summary
                    if dsw:
                        for dec, cnt in res['decision_counts'].items():
                            dsw.writerow([tg.name, run_container, config_base, run_variant, run_dir.name, run_mode, dec, cnt])
                    # iter pages
                    if ipw:
                        for it, pages in res['pages_by_iter'].items():
                            heat_max = res['heat_stats_by_iter'].get(it, 0.0)
                            dump_pages_val = res.get('dump_pages') if it == 'dump_log' else ''
                            ipw.writerow([tg.name, run_container, config_base, run_variant, run_dir.name, run_mode, it, pages, heat_max, dump_pages_val, ''])
                    # per-iter skip accuracy
                    if saw:
                        for (pd,nextiter), vals in res['per_iter_skip_eval'].items():
                            saw.writerow([tg.name, run_container, config_base, run_variant, run_dir.name, run_mode, pd, nextiter, vals['total'], vals['correct'], vals['incorrect'], vals['accuracy']])
                    # per-run skip summary will be written later together with page counts to ensure correct column ordering
                    # (skip summary row is emitted after predump/dump/total pages are determined)
                    # problem pages append
                    if res['problem_pages'] and ppf is not None:
                        if ppw is None:
                            ppw = csv.writer(ppf)
                            headers = ['telegrid','container','config','variant','run','mode'] + list(res['problem_pages'][0].keys())
                            ppw.writerow(headers)
                        for r in res['problem_pages']:
                            ppw.writerow([tg.name, run_container, config_base, run_variant, run_dir.name, run_mode] + [r.get(h,'') for h in r.keys()])
                    # Anomaly detection heuristics
                    force_count = res['decision_counts'].get('FORCE_SEND',0) + res['decision_counts'].get('FORCE_DUMP',0)
                    if force_count > 1000 or force_count > (sum(res['decision_counts'].values())*0.05):
                        if anf:
                            anf.write(f"ANOMALY: {tg.name} {run_container} {config_base} {variant} {run_dir.name} has many FORCE_SEND/FORCE_DUMP: {force_count}\n")
                    if res['decision_counts'].get('DEFER',0) > 10000:
                        if anf:
                            anf.write(f"ANOMALY: {tg.name} {run_container} {config_base} {variant} {run_dir.name} has many DEFER: {res['decision_counts'].get('DEFER')}\n")

                    # Run-level timing summary (may be missing if older runs)
                    def read_int_file(p):
                        try:
                            if p.exists():
                                txt = p.read_text().strip()
                                return int(txt) if txt and txt.isdigit() else ''
                        except Exception:
                            pass
                        return ''

                    total_predump_ms = read_int_file(run_dir / 'total_predump_ms.txt')
                    dump_ms = read_int_file(run_dir / 'dump_ms.txt')
                    total_transfer_ms = read_int_file(run_dir / 'total_transfer_ms.txt')
                    total_migration_ms = read_int_file(run_dir / 'total_migration_ms.txt')
                    downtime_ms = read_int_file(run_dir / 'downtime_ms.txt')
                    bandwidth_mbps = ''
                    try:
                        bfile = run_dir / 'bandwidth_used.txt'
                        if bfile.exists():
                            bandwidth_mbps = bfile.read_text().strip()
                    except Exception:
                        bandwidth_mbps = ''

                    # compute authoritative per-run page totals
                    predump_pages_val = None
                    if (run_dir / 'total_predump_pages.txt').exists():
                        ttxt = (run_dir / 'total_predump_pages.txt').read_text().strip()
                        if ttxt.isdigit():
                            predump_pages_val = int(ttxt)
                    if predump_pages_val is None:
                        psum = sum(v for k,v in res.get('pages_by_iter', {}).items() if k.startswith('predump_') and isinstance(v, int))
                        predump_pages_val = psum if psum > 0 else None
                    dump_pages_val = res.get('dump_pages') if res.get('dump_pages') is not None else res.get('pages_by_iter', {}).get('dump_log', None)
                    total_pages_val = None
                    if (run_dir / 'total_pages.txt').exists():
                        ttxt = (run_dir / 'total_pages.txt').read_text().strip()
                        if ttxt.isdigit():
                            total_pages_val = int(ttxt)
                    if total_pages_val is None:
                        if predump_pages_val is not None or dump_pages_val is not None:
                            total_pages_val = (predump_pages_val or 0) + (dump_pages_val or 0)
                    if rtw:
                        rtw.writerow([tg.name, run_container, config_base, variant, run_dir.name, run_mode, total_predump_ms, dump_ms, total_transfer_ms, total_migration_ms, downtime_ms, bandwidth_mbps, total_pages_val if total_pages_val is not None else '', predump_pages_val if predump_pages_val is not None else '', dump_pages_val if dump_pages_val is not None else ''])
                    if ssumw:
                        ssumw.writerow([tg.name, run_container, config_base, variant, run_dir.name, run_mode, res.get('unique_skips',''), res.get('unique_sends',''), ('%.4f' % res.get('skip_rate_unique')) if res.get('skip_rate_unique') is not None else '', res.get('saved_assume_missing_zero',''), res.get('saved_strict',''), res.get('unknown_addrs',''), predump_pages_val if predump_pages_val is not None else '', dump_pages_val if dump_pages_val is not None else '', total_pages_val if total_pages_val is not None else ''])

        finally:
            # Close any opened file handles
            for fh in (dsf, ipf, sacf, ssumf, ppf, anf, rtf):
                try:
                    if fh:
                        fh.close()
                except Exception:
                    pass

        # Optionally add to combined CSVs if user requested aggregation across telegrids
        if args.combine:
            combined_dir = outdir
            def append_csv(src_path, dest_path):
                if not src_path.exists():
                    return
                write_header = not dest_path.exists()
                with open(src_path, 'r', newline='') as s, open(dest_path, 'a', newline='') as d:
                    r = csv.reader(s)
                    w = csv.writer(d)
                    hdr = next(r, None)
                    if write_header and hdr:
                        w.writerow(hdr)
                    for row in r:
                        w.writerow(row)
            # append per-telegrid results into combined files under outdir
            append_csv(decision_summary_csv, combined_dir / 'grid_decision_summary.csv')
            append_csv(iter_pages_csv, combined_dir / 'grid_iteration_pages.csv')
            append_csv(skip_acc_csv, combined_dir / 'grid_skip_accuracy.csv')
            append_csv(skip_summary_csv, combined_dir / 'grid_skip_summary.csv')
            append_csv(run_timing_csv, combined_dir / 'grid_run_timing.csv')
            append_csv(problem_pages_file, combined_dir / 'grid_problem_pages_combined.csv')
            if anomalies_file.exists():
                with open(anomalies_file, 'r') as af, open(combined_dir / 'grid_anomaly_report.txt', 'a') as caf:
                    for ln in af:
                        caf.write(ln)
    # Aggregate skip accuracy metrics by container and config for per-workload discussion
    agg = defaultdict(list)
    # Scan all per-telegrid skip accuracy files under the output directory
    for skip_path in outdir.glob('telegrid_*/grid_skip_accuracy.csv'):
        try:
            with open(skip_path, 'r', newline='') as f:
                r = csv.reader(f)
                hdr = next(r, None)
                for row in r:
                    if not row or len(row) < 11:
                        continue
                    try:
                        acc = float(row[10])
                    except:
                        continue
                    container = row[1] if len(row) > 1 else 'unknown'
                    cfg = row[2] if len(row) > 2 else ''
                    agg[(container, cfg)].append(acc)
        except Exception:
            continue
    agg_csv = outdir / 'grid_skip_accuracy_by_container.csv'
    with open(agg_csv, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['container','config','n_rows','mean_accuracy','below95','below90'])
        for (container, cfg), vals in sorted(agg.items()):
            n = len(vals)
            mean_acc = sum(vals)/n if n else 0
            below95 = sum(1 for v in vals if v < 95.0)
            below90 = sum(1 for v in vals if v < 90.0)
            w.writerow([container, cfg, n, mean_acc, below95, below90])

    print('Wrote decision summary:', decision_summary_csv)
    print('Wrote iteration pages:', iter_pages_csv)
    print('Wrote skip accuracies:', skip_acc_csv)
    print('Wrote skip summary:', skip_summary_csv)
    print('Wrote combined problem pages:', problem_pages_file)
    print('Wrote anomalies:', anomalies_file)
    print('Wrote skip accuracy by container:', agg_csv)


if __name__ == '__main__':
    main()
