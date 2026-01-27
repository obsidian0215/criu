#!/usr/bin/env python3
"""
Bootstrap skip_model for a bundle by running multiple dry-run trials, collecting decisions,
training a logistic model, and exporting skip_model.json.

Example:
  ./bootstrap_skip_model.py --bundle elasticsearch --runs 3 --out /tmp/skip_model.json --install

The script requires `sklearn` and `pandas` to be available in the environment for training.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import glob
from collections import defaultdict

import numpy as np
import pandas as pd
from sklearn.linear_model import LogisticRegression
from sklearn.impute import SimpleImputer
from sklearn.preprocessing import StandardScaler
from sklearn.model_selection import GroupKFold, StratifiedKFold, cross_val_predict
from sklearn.metrics import roc_auc_score, average_precision_score, precision_recall_curve, precision_score, recall_score, f1_score, confusion_matrix
import random
import re

SCRIPTS_DIR = os.path.dirname(__file__)
DEFAULT_MODEL_DIR = os.getenv('CRIU_SKIP_MODEL_DIR', '/var/lib/criu/skip_models')


def normalize_bundle_name(bundle):
    """Normalize a user-provided bundle identifier to a canonical container basename.

    See install_skip_model.py: prefer /runc/containers/<name> when available, strip common _chk suffixes or use prefix before underscore as fallback.
    """
    orig = bundle
    try:
        if os.path.isdir(bundle):
            return os.path.basename(os.path.normpath(bundle))
    except Exception:
        pass
    if bundle.startswith('/runc/containers/'):
        return os.path.basename(bundle.rstrip('/'))
    try:
        if os.path.isdir(os.path.join('/runc/containers', bundle)):
            return bundle
    except Exception:
        pass
    if bundle.endswith('_chk'):
        return bundle[:-4]
    if '_' in bundle:
        return bundle.split('_', 1)[0]
    return os.path.basename(bundle)


def run_bootstrap_runs(bundle, runs, tmp_root, telemetry_sample=1, config_label=None, image_path=None, predump_iters=None, model_file=None):
    """Run sampling runs for a bundle against an explicit image path.

    config_label is a human-readable run label (does NOT select image). image_path if provided will be set as IMAGE_PATH_OVERRIDE. predump_iters can be used to request more predump iterations (sets PREDUMP_ITERS env). model_file if provided will be set as CRIU_SKIP_MODEL_FILE for evaluation.
    """
    os.makedirs(tmp_root, exist_ok=True)
    env = os.environ.copy()
    # Default to no-model telemetry unless a model_file is provided
    if model_file:
        env['CRIU_SKIP_MODEL_FILE'] = str(model_file)
    else:
        env['CRIU_SKIP_MODEL_FILE'] = ''
        no_models_dir = os.path.join(tmp_root, 'no_models')
        os.makedirs(no_models_dir, exist_ok=True)
        env['CRIU_SKIP_MODEL_DIR'] = no_models_dir
    env['CRIU_DECISION_TELEMETRY'] = '2'
    env['CRIU_DECISION_TELEMETRY_SAMPLE'] = str(telemetry_sample)
    env['TEMP_ROOT'] = tmp_root

    # config_label is only a human identifier and does NOT select the image
    if config_label:
        env['VARIANT_NAME'] = str(config_label)
    if image_path:
        env['IMAGE_PATH_OVERRIDE'] = str(image_path)
    if predump_iters:
        env['PREDUMP_ITERS'] = str(predump_iters)

    name = f"bootstrap_{bundle}_{config_label or 'any'}_{int(time.time())}"
    cmd = ['bash', os.path.join(SCRIPTS_DIR, 'chk_rst.sh'), '--name', name, '--bundle', bundle, '--repeats', str(runs)]
    if config_label:
        cmd += ['--variant', str(config_label)]
    if image_path:
        cmd += ['--image-path', str(image_path)]
    print('Running bootstrap runs:', ' '.join(cmd))
    subprocess.check_call(cmd, env=env)
    # collect run directories (ONLY top-level run_<digits> directories; exclude nested backup run_* entries)
    all_runs = sorted([p for p in glob.glob(os.path.join(tmp_root, '**', 'run_*'), recursive=True) if os.path.isdir(p)])
    # filter basenames to exactly run_<digits> to avoid matching nested backup directories named like run_<label>_...
    runs_found = [p for p in all_runs if re.match(r'^run_\d+$', os.path.basename(p))]
    print('Collected runs (dirs):', len(runs_found), 'from', len(all_runs), 'candidates')
    # Parse runs to generate decisions.csv and related artifacts so downstream steps can consume them
    for rd in runs_found:
        try:
            print('Parsing run for decisions:', rd)
            subprocess.run(['python3', os.path.join(SCRIPTS_DIR, 'parse_dirtymap.py'), '--run', rd], check=False, timeout=300)
        except subprocess.TimeoutExpired:
            print('parse_dirtymap.py timed out for', rd)
        except Exception as e:
            print('parse_dirtymap.py failed for', rd, e)
    return runs_found


def gather_decision_runs(run_dirs):
    # return a list of run dirs that contain decisions.csv
    valid = []
    for rd in run_dirs:
        dec = os.path.join(rd, 'decisions.csv')
        if os.path.isfile(dec):
            valid.append(rd)
    return valid


def run_single_checkpoint_direct(bundle, tmp_root, telemetry_sample=1, telemetry='2', image_path=None, predump_iters=None, model_file=None, config_label=None, timeout=3600):
    """Run a single checkpoint invocation directly (no chk_rst.sh wrapper).

    Uses checkpoint_run_impl.sh directly to perform a single restore + predump sequence with
    the provided PREDUMP_ITERS and IMAGE_PATH_OVERRIDE settings. If config_label is provided,
    it will be exported as VARIANT_NAME so the run records and dirty-track enabling follow the
    config_label semantics (baseline => disabled, other => enabled).

    The optional `telemetry` parameter sets `CRIU_DECISION_TELEMETRY` for this run ('0'/'1'/'2').

    Returns list of run dirs.
    """
    os.makedirs(tmp_root, exist_ok=True)
    env = os.environ.copy()
    # allow caller to explicitly request telemetry mode (disabled = '0')
    env['CRIU_DECISION_TELEMETRY'] = str(telemetry) if telemetry is not None else '2'
    env['CRIU_DECISION_TELEMETRY_SAMPLE'] = str(telemetry_sample)
    env['TEMP_ROOT'] = tmp_root
    if model_file:
        env['CRIU_SKIP_MODEL_FILE'] = str(model_file)
    else:
        env['CRIU_SKIP_MODEL_FILE'] = ''
        no_models_dir = os.path.join(tmp_root, 'no_models')
        os.makedirs(no_models_dir, exist_ok=True)
        env['CRIU_SKIP_MODEL_DIR'] = no_models_dir
    if image_path:
        env['IMAGE_PATH_OVERRIDE'] = str(image_path)
    if predump_iters:
        env['PREDUMP_ITERS'] = str(predump_iters)
    if config_label:
        env['VARIANT_NAME'] = str(config_label)

    cmd = ['bash', os.path.join(SCRIPTS_DIR, 'checkpoint_run_impl.sh'), bundle]
    print('Running single checkpoint (direct):', ' '.join(cmd), 'PREDUMP_ITERS=', env.get('PREDUMP_ITERS',''), 'config_label=', env.get('VARIANT_NAME',''), 'telemetry=', env.get('CRIU_DECISION_TELEMETRY'))
    subprocess.check_call(cmd, env=env, timeout=timeout)
    all_runs = sorted([p for p in glob.glob(os.path.join(tmp_root, '**', 'run_*'), recursive=True) if os.path.isdir(p)])
    runs_found = [p for p in all_runs if re.match(r'^run_\d+$', os.path.basename(p))]
    print('Collected runs:', len(runs_found), 'from', len(all_runs), 'candidates')
    # Parse runs to generate decisions.csv and related artifacts so downstream steps can consume them
    for rd in runs_found:
        try:
            print('Parsing run for decisions:', rd)
            subprocess.run(['python3', os.path.join(SCRIPTS_DIR, 'parse_dirtymap.py'), '--run', rd], check=False, timeout=300)
        except subprocess.TimeoutExpired:
            print('parse_dirtymap.py timed out for', rd)
        except Exception as e:
            print('parse_dirtymap.py failed for', rd, e)
    return runs_found


def eval_model_active_vs_disabled(bundle, image_path, predump_iters, model_file, tmp_root_base, config_label=None, telemetry_sample=1, debug_max_rows=100000, debug_compress=True):
    """Evaluate model by running two direct single runs: active (model loaded) and disabled.

    Returns a dict with 'active' and 'disabled' stats (n samples, skip_accuracy, label_dist)
    """
    results = {'active': None, 'disabled': None}

    # Active run
    active_tmp = os.path.join(tmp_root_base, 'eval_active')
    # Active evaluation: keep detailed telemetry to collect decisions for analysis
    runs_active = run_single_checkpoint_direct(bundle, active_tmp, telemetry_sample=telemetry_sample, telemetry='2', image_path=image_path, predump_iters=predump_iters, model_file=model_file, config_label=config_label)
    runs_active_valid = gather_decision_runs(runs_active)
    data_active = build_dataset(runs_active_valid, debug_out_dir=active_tmp, debug_max_rows=debug_max_rows, debug_compress=debug_compress)
    if data_active:
        X_a, y_a, g_a, w_a, meta_a = data_active
        n = len(y_a)
        if n > 0:
            zeros = int((np.array(y_a) == 0).sum())
            ones = int((np.array(y_a) == 1).sum())
            metrics_a = compute_metrics(X_a, y_a, meta_df=meta_a, model_json=model_file, threshold=None, bootstrap_iters=1000)
            results['active'] = {'n': n, 'skip_fraction': float(zeros) / float(n), 'label_dist': {0: zeros, 1: ones}, 'metrics': metrics_a}
            # persist metrics
            try:
                with open(os.path.join(active_tmp, 'metrics_active.json'), 'w') as mf:
                    json.dump(metrics_a, mf, indent=2)
            except Exception:
                pass
        else:
            results['active'] = {'n': 0}
    else:
        results['active'] = None

    # Disabled run: explicitly disable model but start dirty-track (config_label='disabled')
    disabled_tmp = os.path.join(tmp_root_base, 'eval_disabled')
    # Disabled evaluation: explicitly disable telemetry and run with dirty-track enabled (variant label 'disabled')
    runs_disabled = run_single_checkpoint_direct(bundle, disabled_tmp, telemetry_sample=telemetry_sample, telemetry='0', image_path=image_path, predump_iters=predump_iters, model_file=None, config_label='disabled')
    runs_disabled_valid = gather_decision_runs(runs_disabled)
    data_disabled = build_dataset(runs_disabled_valid, debug_out_dir=disabled_tmp, debug_max_rows=debug_max_rows, debug_compress=debug_compress)
    if data_disabled:
        X_d, y_d, g_d, w_d, meta_d = data_disabled
        n = len(y_d)
        if n > 0:
            zeros = int((np.array(y_d) == 0).sum())
            ones = int((np.array(y_d) == 1).sum())
            metrics_d = compute_metrics(X_d, y_d, meta_df=meta_d, model_json=model_file, threshold=None, bootstrap_iters=1000)
            results['disabled'] = {'n': n, 'skip_fraction': float(zeros) / float(n), 'label_dist': {0: zeros, 1: ones}, 'metrics': metrics_d}
            try:
                with open(os.path.join(disabled_tmp, 'metrics_disabled.json'), 'w') as mf:
                    json.dump(metrics_d, mf, indent=2)
            except Exception:
                pass
        else:
            results['disabled'] = {'n': 0}
    else:
        results['disabled'] = None

    return results


def build_dataset(runs, debug_out_dir=None, debug_max_rows=0, debug_compress=True):
    # Builds feature matrix X and label vector y from a list of run dirs
    rows = []
    sampled_debug_rows = []
    debug_rows_seen = 0
    groups = []
    writes_arr = []
    meta_rows = []
    feature_cols = ['heat','heat_trend','writes_est','track_s','hist_count','hist_mean','hist_variance','deferred','p','score','pthr','round']

    def parse_addr(s):
        if not isinstance(s, str):
            return None
        s = s.strip()
        try:
            return int(s, 0)
        except Exception:
            try:
                return int(s, 16)
            except Exception:
                return None

    for rd in runs:
        decf = os.path.join(rd, 'decisions.csv')
        ckpt = os.path.join(rd, 'checkpoint_log')
        if not os.path.isfile(decf) or not os.path.isdir(ckpt):
            continue
        # build iteration index
        pred_dirs = sorted([os.path.basename(p) for p in glob.glob(os.path.join(ckpt, 'predump_*')) if os.path.isdir(p)])
        if os.path.isdir(os.path.join(ckpt, 'dump_log')):
            pred_dirs.append('dump_log')
        name_to_idx = {name:i for i,name in enumerate(pred_dirs)}

        with open(decf) as f:
            import csv
            try:
                csv.field_size_limit(10 * 1024 * 1024)
            except Exception:
                pass
            r = csv.DictReader(f)
            for row in r:
                dec = row.get('decision')
                if dec == 'SEND': dec = 'DUMP'
                elif dec == 'FORCE_SEND': dec = 'FORCE_DUMP'
                if dec != 'DEFER':
                    continue
                pdname = row.get('predump_dir')
                if pdname not in name_to_idx:
                    if pdname == 'dump' and 'dump_log' in name_to_idx:
                        idx = name_to_idx['dump_log']
                    else:
                        continue
                else:
                    idx = name_to_idx[pdname]
                next_idx = idx + 1
                if next_idx >= len(pred_dirs):
                    continue
                next_iter = pred_dirs[next_idx]
                pid = int(row.get('pid') or 0)
                addr = parse_addr(row.get('addr') or '')
                if addr is None:
                    continue
                # Extract run identifier from the 'file' field if present (helps map to run-specific dirtymap backups)
                run_id = None
                file_path = row.get('file') or ''
                try:
                    # Walk path components to find a directory name that starts with 'run_'
                    parts = [p for p in file_path.split(os.sep) if p]
                    for comp in reversed(parts):
                        if comp.startswith('run_'):
                            run_id = comp
                            break
                except Exception:
                    run_id = None

                key = (next_iter, pid, run_id)
                rows.append((rd, key, row))

    # group by (next_iter, pid) and load dirtymaps to produce labels
    grouped = defaultdict(list)
    for rd, key, row in rows:
        grouped[(rd, key)].append(row)

    X_rows = []
    y_rows = []

    ENTRY_DM_STRUCT = '<QI'
    import struct
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

    dm_cache = {}
    skipped_groups = 0
    for (rd, (next_iter_name, pid, run_id)), rowslist in grouped.items():
        next_iter_dir = os.path.join(rd, 'checkpoint_log', next_iter_name)
        dm_map = {}
        track_ns = None
        found_any_dirty = False
        dm_source = None
        chosen_dm_file = ''

        # If parse_dirtymap.py annotated rows with an explicit dirtymap file (dm_file), prefer those
        explicit_files = []
        for r in rowslist:
            dmf = r.get('dm_file') if isinstance(r, dict) else None
            if dmf:
                # accept absolute or relative paths; prefer absolute if present
                p = dmf if os.path.isabs(dmf) else os.path.join(rd, dmf)
                if os.path.isfile(p):
                    explicit_files.append(p)
        if explicit_files:
            combined = {}
            for fpath in explicit_files:
                _, m = parse_dirtymap_file(fpath)
                for a, w in m.items():
                    combined[a] = combined.get(a, 0) + w
            if combined:
                dm_map = combined
                found_any_dirty = True
                dm_source = 'explicit_files'
                chosen_dm_file = ','.join(explicit_files)

        # Prefer explicit index file (created by checkpoint_run_impl.sh) which maps run_* dirs -> dirtymap files
        idx_candidates = []
        idx_candidates += glob.glob(os.path.join(rd, f'dirtymap_index_{next_iter_name}*.json'))
        idx_candidates += glob.glob(os.path.join(next_iter_dir, f'dirtymap_index_{next_iter_name}*.json'))
        idx_candidates += glob.glob(os.path.join(rd, 'dirtymap_index_*.json'))
        idx_path = idx_candidates[0] if idx_candidates else None

        if idx_path and os.path.isfile(idx_path):
            try:
                import json as _json
                with open(idx_path) as jf:
                    idx = _json.load(jf)
                pred_idx = idx.get('predump_index', {})
                chosen_key = None
                # Prefer exact run_id match when available
                if run_id:
                    for k in pred_idx.keys():
                        if run_id in k or k in run_id:
                            chosen_key = k
                            break
                # If we only have a single run_* entry for this iteration, choose it
                if not chosen_key and len(pred_idx) == 1:
                    chosen_key = next(iter(pred_idx.keys()))

                if chosen_key:
                    combined = {}
                    backup_base = os.path.join(next_iter_dir, f'dirtymap_backup_{next_iter_name}')
                    if not os.path.isdir(backup_base):
                        # sometimes the copy lives in rd (legacy layout)
                        backup_base = os.path.join(rd, f'dirtymap_backup_{next_iter_name}')
                    run_subdir = os.path.join(backup_base, chosen_key)
                    if os.path.isdir(run_subdir):
                        for fn in pred_idx.get(chosen_key, []):
                            fpath = os.path.join(run_subdir, fn)
                            if os.path.isfile(fpath):
                                _, m = parse_dirtymap_file(fpath)
                                for a, w in m.items():
                                    combined[a] = combined.get(a, 0) + w
                        if combined:
                            dm_map = combined
                            found_any_dirty = True
                            dm_source = 'index'
                            chosen_dm_file = run_subdir
            except Exception:
                # index parse failed or unreadable; fall back to pid-based lookup
                found_any_dirty = False

        if not found_any_dirty:
            # Try per-pid dirtymap first (fast, preferred)
            dm_file = find_latest_dirtymap_for_pid(next_iter_dir, pid)
            if dm_file:
                track_ns, dm_map = parse_dirtymap_file(dm_file)
                if dm_map:
                    found_any_dirty = True
                    dm_source = 'pid_dirtymap'
                    chosen_dm_file = dm_file

        if not found_any_dirty:
            # Look for run-specific subdirs under dirtymap_backup_* (prefer those matching run_id)
            if (next_iter_dir, run_id) not in dm_cache:
                combined = {}
                for d in sorted(glob.glob(os.path.join(next_iter_dir, 'dirtymap_backup_*'))):
                    # Prefer run_* directories that match run_id
                    matched = []
                    if run_id:
                        for rsub in sorted(glob.glob(os.path.join(d, f'*{run_id}*'))):
                            matched.append(rsub)
                    if not matched:
                        matched = sorted(glob.glob(os.path.join(d, 'run_*')))
                    for subdir in matched:
                        for fn in glob.glob(os.path.join(subdir, '*.dirtymap')):
                            _, m = parse_dirtymap_file(fn)
                            for a, w in m.items():
                                combined[a] = combined.get(a, 0) + w
                dm_cache[(next_iter_dir, run_id)] = combined
            dm_map = dm_cache.get((next_iter_dir, run_id), {})
            if dm_map:
                found_any_dirty = True
                dm_source = 'backup_aggregate'
                chosen_dm_file = 'aggregate'

        # If still no dirtymap information available for this run/iteration, skip these samples to avoid noisy labels
        if not found_any_dirty:
            skipped_groups += 1
            continue

        for row in rowslist:
            addr = parse_addr(row.get('addr') or '')
            if addr is None:
                continue
            writes = dm_map.get(addr, 0) if dm_map else 0
            label = 1 if writes > 0 else 0
            feats = {}
            for c in feature_cols:
                v = row.get(c) if c in row else None
                try:
                    feats[c] = float(v) if (v is not None and v != '') else np.nan
                except Exception:
                    feats[c] = np.nan
            X_rows.append(feats)
            y_rows.append(label)
            group_id = run_id if run_id else os.path.basename(rd)
            groups.append(group_id)
            writes_arr.append(writes)
            meta_rows.append({
                'run_dir': rd,
                'group': group_id,
                'next_iter': next_iter_name,
                'iter_idx': name_to_idx.get(next_iter_name, None),
                'pid': pid,
                'addr': addr,
                'writes': writes,
                'label': label
            })
            # record debug information (reservoir sampled to limit size)
            if debug_max_rows and debug_max_rows > 0:
                debug_rows_seen += 1
                dbg_row = {
                    'run_dir': rd,
                    'next_iter': next_iter_name,
                    'pid': pid,
                    'addr': addr,
                    'addr_hex': hex(addr) if addr is not None else '',
                    'predump_dir': row.get('predump_dir',''),
                    'file': row.get('file',''),
                    'line': row.get('line',''),
                    'dm_source': dm_source if dm_source else '',
                    'dm_file': chosen_dm_file,
                    'writes': writes,
                    'label': label
                }
                # simple reservoir sampling
                if len(sampled_debug_rows) < debug_max_rows:
                    sampled_debug_rows.append(dbg_row)
                else:
                    replace_idx = random.randrange(debug_rows_seen)
                    if replace_idx < debug_max_rows:
                        sampled_debug_rows[replace_idx] = dbg_row

    if skipped_groups:
        print(f"Skipped {skipped_groups} groups due to missing run-specific dirtymap artifacts (improves label quality)")

    if debug_out_dir:
        import csv as _csv
        keys = ['run_dir','next_iter','pid','addr','addr_hex','predump_dir','file','line','dm_source','dm_file','writes','label']
        # write sampled debug rows (if enabled via debug_max_rows)
        if debug_max_rows and debug_max_rows > 0 and sampled_debug_rows:
            if debug_compress:
                dbg_fname = 'label_debug.csv.gz'
                import gzip as _gzip
                dbg_path = os.path.join(debug_out_dir, dbg_fname)
                with _gzip.open(dbg_path, 'wt', newline='') as df:
                    w = _csv.DictWriter(df, fieldnames=keys)
                    w.writeheader()
                    for r in sampled_debug_rows:
                        w.writerow({k: r.get(k, '') for k in keys})
            else:
                dbg_path = os.path.join(debug_out_dir, 'label_debug.csv')
                with open(dbg_path, 'w', newline='') as df:
                    w = _csv.DictWriter(df, fieldnames=keys)
                    w.writeheader()
                    for r in sampled_debug_rows:
                        w.writerow({k: r.get(k, '') for k in keys})
            print(f'Wrote label debug to {dbg_path} ({len(sampled_debug_rows)} rows sampled from {debug_rows_seen} total)')
            # show label distribution
            try:
                from collections import Counter
                dist = Counter([r['label'] for r in sampled_debug_rows])
                print('Label distribution (debug samples):', dict(dist))
            except Exception:
                pass
        else:
            if debug_max_rows and debug_max_rows > 0:
                print('No sampled debug rows to write')

    if not X_rows:
        print('No training samples found')
        return None

    X = pd.DataFrame(X_rows)
    y = np.array(y_rows)
    groups_arr = np.array(groups)
    writes_arr_np = np.array(writes_arr)
    meta_df = pd.DataFrame(meta_rows) if meta_rows else pd.DataFrame(columns=['run_dir','group','next_iter','iter_idx','pid','addr','writes','label'])
    return X, y, groups_arr, writes_arr_np, meta_df


def apply_model_json(model, X_df):
    # model: dict loaded from JSON with fields 'features', 'weights', 'intercept', 'impute_median'
    import math as _math
    feats = model.get('features', [])
    weights = model.get('weights', [])
    intercept = float(model.get('intercept', 0.0))
    impute = model.get('impute_median', {})
    # Ensure features present and impute missing values
    Xc = X_df.copy()
    for f in feats:
        if f not in Xc.columns:
            Xc[f] = impute.get(f, 0.0)
        else:
            Xc[f] = Xc[f].fillna(impute.get(f, 0.0))
    # compute linear score in original feature scale using stored weights
    # weights correspond to feats order; use vectorized dot product
    w_arr = np.array(weights, dtype=float)
    Xmat = Xc[feats].astype(float).fillna(0.0)
    z = intercept + Xmat.dot(w_arr)
    # z is numeric array/Series
    p = 1.0 / (1.0 + np.exp(-z))
    return p


def bootstrap_ci(values, iters=1000, alpha=0.05):
    # values: list or 1D array of per-run scalar metrics
    # returns (lower, upper) percentiles for the mean via bootstrap resampling
    if values is None or len(values) == 0:
        return None, None
    arr = np.array(values)
    n = len(arr)
    if n == 1:
        return float(arr[0]), float(arr[0])
    sims = []
    for i in range(iters):
        sample = np.random.choice(arr, size=n, replace=True)
        sims.append(sample.mean())
    lo = float(np.percentile(sims, 100.0 * (alpha / 2.0)))
    hi = float(np.percentile(sims, 100.0 * (1.0 - alpha / 2.0)))
    return lo, hi


def compute_metrics(X_df, y, meta_df=None, model_json=None, threshold=None, bootstrap_iters=1000):
    # Returns a dictionary with aggregated metrics, per-run metrics and per-iteration skip curves
    res = {}
    df = X_df.copy()
    df['label'] = list(y)
    if meta_df is not None and not meta_df.empty:
        # align meta_df index with df; drop any duplicate 'label' column from meta to
        # avoid creating duplicated 'label' columns which would make df['label'] a
        # DataFrame (comparison then yields a DataFrame and .sum() returns a Series).
        meta = meta_df.reset_index(drop=True).copy()
        if 'label' in meta.columns:
            meta = meta.drop(columns=['label'])
        df = pd.concat([df.reset_index(drop=True), meta.reset_index(drop=True)], axis=1)
    else:
        df['run_dir'] = 'global'
        df['next_iter'] = None
        df['iter_idx'] = None

    # model predictions
    if model_json is not None:
        if isinstance(model_json, str):
            try:
                with open(model_json) as mf:
                    model = json.load(mf)
            except Exception:
                model = model_json
        else:
            model = model_json
        probs = apply_model_json(model, X_df)
        df['prob'] = probs.values if hasattr(probs, 'values') else probs
        use_thr = threshold if threshold is not None else (model.get('best_threshold', 0.5) if isinstance(model, dict) else 0.5)
        df['pred'] = (df['prob'] >= float(use_thr)).astype(int)
    else:
        df['prob'] = np.nan
        df['pred'] = np.nan

    # overall metrics
    total_n = len(df)
    total_safe = int((df['label'] == 0).sum())
    total_miss = int((df['label'] == 1).sum())
    res['n'] = int(total_n)
    res['skip_fraction'] = float(total_safe / total_n) if total_n > 0 else None
    res['label_dist'] = {0: int(total_safe), 1: int(total_miss)}

    # model metrics overall (if model present)
    def safe_call_metric(fn, y_true, y_pred_or_prob, prob=False):
        try:
            if prob:
                return float(fn(y_true, y_pred_or_prob))
            else:
                return float(fn(y_true, y_pred_or_prob, zero_division=0))
        except Exception:
            return None

    if model_json is not None:
        # binary metrics require both classes present
        try:
            if len(np.unique(df['label'])) > 1:
                res['roc_auc'] = safe_call_metric(roc_auc_score, df['label'], df['prob'], prob=True)
                res['pr_auc'] = safe_call_metric(average_precision_score, df['label'], df['prob'], prob=True)
                y_pred = df['pred'].fillna(0).astype(int)
                res['precision'] = safe_call_metric(precision_score, df['label'], y_pred)
                res['recall'] = safe_call_metric(recall_score, df['label'], y_pred)
                res['f1'] = safe_call_metric(f1_score, df['label'], y_pred)
                try:
                    tn, fp, fn, tp = confusion_matrix(df['label'], y_pred).ravel()
                    res['confusion'] = {'tn': int(tn), 'fp': int(fp), 'fn': int(fn), 'tp': int(tp)}
                except Exception:
                    res['confusion'] = None
            else:
                res['roc_auc'] = None
                res['pr_auc'] = None
                res['precision'] = None
                res['recall'] = None
                res['f1'] = None
                res['confusion'] = None
        except Exception:
            res['roc_auc'] = res['pr_auc'] = res['precision'] = res['recall'] = res['f1'] = None
            res['confusion'] = None

    # per-run metrics
    per_run = []
    runs = sorted(df['run_dir'].unique())
    for r in runs:
        sub = df[df['run_dir'] == r]
        n = len(sub)
        safe = int((sub['label'] == 0).sum())
        miss = int((sub['label'] == 1).sum())
        runm = {'run_dir': r, 'n': int(n), 'skip_fraction': float(safe / n) if n>0 else None, 'label_dist': {0: safe, 1: miss}}
        if model_json is not None and len(np.unique(sub['label'])) > 1:
            y_pred = sub['pred'].fillna(0).astype(int)
            runm['precision'] = safe_call_metric(precision_score, sub['label'], y_pred)
            runm['recall'] = safe_call_metric(recall_score, sub['label'], y_pred)
            runm['f1'] = safe_call_metric(f1_score, sub['label'], y_pred)
            try:
                tn, fp, fn, tp = confusion_matrix(sub['label'], y_pred).ravel()
                runm['confusion'] = {'tn': int(tn), 'fp': int(fp), 'fn': int(fn), 'tp': int(tp)}
            except Exception:
                runm['confusion'] = None
            # per-run auc/pr
            runm['roc_auc'] = safe_call_metric(roc_auc_score, sub['label'], sub['prob'], prob=True)
            runm['pr_auc'] = safe_call_metric(average_precision_score, sub['label'], sub['prob'], prob=True)
        per_run.append(runm)
    res['per_run'] = per_run

    # bootstrap CI for per-run metrics (mean across runs)
    if len(per_run) > 0:
        precs = [p.get('precision') for p in per_run if p.get('precision') is not None]
        recs = [p.get('recall') for p in per_run if p.get('recall') is not None]
        f1s = [p.get('f1') for p in per_run if p.get('f1') is not None]
        misses = [p['label_dist'][1]/p['n'] for p in per_run if p['n']>0]
        res['bootstrap_ci'] = {}
        if len(precs) > 0:
            res['bootstrap_ci']['precision'] = bootstrap_ci(precs, iters=bootstrap_iters)
        if len(recs) > 0:
            res['bootstrap_ci']['recall'] = bootstrap_ci(recs, iters=bootstrap_iters)
        if len(f1s) > 0:
            res['bootstrap_ci']['f1'] = bootstrap_ci(f1s, iters=bootstrap_iters)
        if len(misses) > 0:
            res['bootstrap_ci']['miss_rate'] = bootstrap_ci(misses, iters=bootstrap_iters)

    # per-iteration skip_fraction curve (aggregated across runs)
    try:
        iter_grp = df.groupby('iter_idx')
        iter_rows = []
        for idx, g in sorted(iter_grp, key=lambda x: (x[0] if x[0] is not None else -1)):
            n = len(g)
            safe = int((g['label'] == 0).sum())
            iter_rows.append({'iter_idx': idx, 'n': int(n), 'skip_fraction': float(safe / n) if n>0 else None})
        res['per_iter'] = iter_rows
    except Exception:
        res['per_iter'] = []

    return res


def train_and_export_model(X, y, groups, out_path, writes=None):
    feature_cols = ['heat','heat_trend','writes_est','track_s','hist_count','hist_mean','hist_variance','deferred','p','score','pthr','round']
    # fill missing entirely-NaN columns with 0
    for c in feature_cols:
        if c not in X.columns:
            X[c] = 0.0
        if X[c].isna().all():
            X[c] = 0.0

    imp = SimpleImputer(strategy='median')
    X_imp = pd.DataFrame(imp.fit_transform(X[feature_cols]), columns=feature_cols)
    sc = StandardScaler()
    X_scaled = pd.DataFrame(sc.fit_transform(X_imp), columns=feature_cols)

    # Ensure labels contain both classes
    if len(np.unique(y)) < 2:
        print('ERROR: Labels contain only one class; cannot train logistic model (need both 0 and 1).')
        return None

    # Prefer GroupKFold when multiple groups are available; otherwise fall back to StratifiedKFold.
    n_groups = len(np.unique(groups)) if groups is not None else 0
    if n_groups >= 2:
        n_splits = min(5, n_groups)
        cv = GroupKFold(n_splits=n_splits)
        group_arg = groups
        print(f'Using GroupKFold with {n_splits} splits (groups={n_groups})')
    else:
        print('Warning: Not enough distinct groups for GroupKFold (need at least 2 runs/groups). Falling back to StratifiedKFold (may cause leakage across runs).')
        n_splits = min(5, len(y)) if len(y) >= 2 else 0
        if n_splits < 2:
            print('ERROR: Not enough samples to perform CV.')
            return None
        cv = StratifiedKFold(n_splits=n_splits, shuffle=True, random_state=42)
        group_arg = None
        print(f'Using StratifiedKFold with {n_splits} splits')

    lr = LogisticRegression(solver='saga', max_iter=2000)
    try:
        preds = cross_val_predict(lr, X_scaled, y, cv=cv, groups=group_arg, method='predict_proba')[:,1]
    except Exception as e:
        print('ERROR: cross_val_predict failed:', e)
        return None

    auc = roc_auc_score(y, preds)
    ap = average_precision_score(y, preds)

    # choose best threshold by F1 on CV preds
    prec, rec, thr = precision_recall_curve(y, preds)
    f1s = 2 * prec * rec / (prec + rec + 1e-12)
    best_idx = int(np.nanargmax(f1s))
    best_thr = float(thr[best_idx]) if best_idx < len(thr) else 0.5

    y_pred = (preds >= best_thr).astype(int)
    precision_v = float(precision_score(y, y_pred, zero_division=0))
    recall_v = float(recall_score(y, y_pred, zero_division=0))
    f1_v = float(f1_score(y, y_pred, zero_division=0))
    try:
        tn, fp, fn, tp = confusion_matrix(y, y_pred).ravel()
    except Exception:
        cm = confusion_matrix(y, y_pred)
        if cm.size == 4:
            tn, fp, fn, tp = cm.ravel()
        else:
            tn = fp = fn = tp = 0

    # risk metrics: proportion of predicted-skipped samples that actually had writes
    preds_skip_mask = (y_pred == 0)
    num_pred_skips = int(preds_skip_mask.sum())
    risk = None
    risk_writes = None
    if num_pred_skips > 0:
        risk = float((y[preds_skip_mask] == 1).sum()) / float(num_pred_skips)
        if writes is not None and len(writes) == len(y):
            w_arr = np.array(writes)
            pred_skip_writes_sum = float(w_arr[preds_skip_mask].sum())
            if pred_skip_writes_sum > 0:
                missed_writes_sum = float(w_arr[preds_skip_mask & (y == 1)].sum())
                risk_writes = missed_writes_sum / pred_skip_writes_sum
            else:
                risk_writes = 0.0

    # fit final model on all data
    lr.fit(X_scaled, y)

    # convert coefficients back to original scale: w_orig = w_scaled / std
    coefs_scaled = lr.coef_[0].tolist()
    scales = sc.scale_.tolist()
    means = sc.mean_.tolist()
    weights_orig = [float(c / s) for c, s in zip(coefs_scaled, scales)]
    intercept_scaled = float(lr.intercept_[0])
    intercept_orig = intercept_scaled - sum((c * m) / s for c, m, s in zip(coefs_scaled, means, scales))

    model = {
        'features': feature_cols,
        'weights': weights_orig,
        'intercept': intercept_orig,
        'impute_median': dict(zip(feature_cols, imp.statistics_.tolist())),
        'scale_mean': dict(zip(feature_cols, means)),
        'scale_std': dict(zip(feature_cols, scales)),
        'best_threshold': best_thr,
        'cv_logistic_auc': float(auc),
        'cv_logistic_pr': float(ap),
        'cv_precision': precision_v,
        'cv_recall': recall_v,
        'cv_f1': f1_v,
        'cv_confusion': {'tn': int(tn), 'fp': int(fp), 'fn': int(fn), 'tp': int(tp)},
        'cv_risk': float(risk) if risk is not None else None,
        'cv_risk_writes': float(risk_writes) if risk_writes is not None else None,
        'trained_at': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    }

    with open(out_path, 'w') as f:
        json.dump(model, f, indent=2)
    print('Wrote model to', out_path, 'auc=', auc, 'pr=', ap, 'best_thr=', best_thr)
    print('CV metrics: precision=', precision_v, 'recall=', recall_v, 'f1=', f1_v, 'risk=', risk, 'risk_writes=', risk_writes)
    return model


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--bundle', required=True, help='Bundle/container name (container basename; image-variant names will be normalized)')
    p.add_argument('--image-path', default=None, help='Path to image directory to use (must contain descriptors.json).')
    p.add_argument('--config-label', default=None, help='Configuration label (human runtime name). Use "baseline" to disable dirty-track; any other label enables dirty-track. Defaults to "sample" to avoid accidental baseline-only runs.')
    p.add_argument('--predump-iters', type=int, default=None, help='Number of predump iterations to request (sets PREDUMP_ITERS in env). Prefer multiple predump iterations within a run for dry-run sampling.')
    p.add_argument('--runs', type=int, default=1, help='Number of repeated runs (default 1). Consider using higher --predump-iters rather than many runs.')
    p.add_argument('--tmp-root', default=None)
    p.add_argument('--out', default=None)
    p.add_argument('--install', action='store_true', help='Install generated model into system model dir (current.json)')
    p.add_argument('--eval', action='store_true', help='After training, run evaluation comparing model active vs disabled on the same image (no install).')
    p.add_argument('--model-dir', default=DEFAULT_MODEL_DIR)
    p.add_argument('--debug-max-rows', type=int, default=0, help='Maximum number of debug rows to write to label_debug.csv (0 to disable).')
    p.add_argument('--no-debug-compress', dest='debug_compress', action='store_false', help='Disable gzip compression for debug files (enabled by default).')
    p.add_argument('--skip-sampling', dest='skip_sampling', action='store_true', help='Skip executing sampling runs and use existing run directories under --tmp-root if present.')
    p.add_argument('--metrics-only', dest='metrics_only', action='store_true', help='Compute metrics for existing runs under --tmp-root and exit (no training).')
    p.add_argument('--model', dest='model', default=None, help='Optional model file to use when computing metrics.')
    p.add_argument('--metrics-out', dest='metrics_out', default=None, help='Path to write computed metrics JSON (defaults to <tmp-root>/metrics.json).')
    p.add_argument('--bootstrap-iters', dest='bootstrap_iters', type=int, default=1000, help='Bootstrap iterations for CI (default 1000).')
    args = p.parse_args()

    bundle_norm = normalize_bundle_name(args.bundle)
    if bundle_norm != args.bundle:
        print(f'Normalized bundle "{args.bundle}" -> "{bundle_norm}"')
    tmp_root = args.tmp_root or tempfile.mkdtemp(prefix=f'bootstrap_{bundle_norm}_')
    print('Using tmp root:', tmp_root)

    # Metrics-only mode: compute metrics from existing runs and exit
    if args.metrics_only:
        print('Metrics-only mode: computing metrics from runs under', tmp_root)
        all_runs = sorted([p for p in glob.glob(os.path.join(tmp_root, '**', 'run_*'), recursive=True) if os.path.isdir(p)])
        runs_found = [p for p in all_runs if re.match(r'^run_\d+$', os.path.basename(p))]
        print('Found run dirs:', len(runs_found), 'from', len(all_runs), 'candidates')
        for rd in runs_found:
            try:
                print('Parsing run for decisions:', rd)
                subprocess.run(['python3', os.path.join(SCRIPTS_DIR, 'parse_dirtymap.py'), '--run', rd], check=False, timeout=300)
            except subprocess.TimeoutExpired:
                print('parse_dirtymap.py timed out for', rd)
            except Exception as e:
                print('parse_dirtymap.py failed for', rd, e)
        runs_valid = gather_decision_runs(runs_found)
        if not runs_valid:
            print('No valid runs with decisions.csv found under', tmp_root)
            sys.exit(2)
        data = build_dataset(runs_valid, debug_out_dir=tmp_root, debug_max_rows=args.debug_max_rows, debug_compress=args.debug_compress)
        if not data:
            print('No training data built')
            sys.exit(3)
        X, y, groups, writes, meta = data
        metrics = compute_metrics(X, y, meta_df=meta, model_json=args.model, threshold=None, bootstrap_iters=args.bootstrap_iters)
        metrics_out_path = args.metrics_out or os.path.join(tmp_root, 'metrics.json')
        try:
            with open(metrics_out_path, 'w') as mf:
                json.dump(metrics, mf, indent=2)
            print('Wrote metrics to', metrics_out_path)
        except Exception as e:
            print('Failed to write metrics to', metrics_out_path, e)
        print('Metrics summary:', {k: metrics.get(k) for k in ['n', 'skip_fraction', 'precision', 'recall', 'f1', 'pr_auc', 'roc_auc']})
        sys.exit(0)

    # Validate explicit image selection (prefer --image-path).
    image_path = args.image_path
    if image_path:
        if not os.path.isdir(image_path) or not os.path.exists(os.path.join(image_path, 'descriptors.json')):
            print(f"ERROR: image path '{image_path}' missing or invalid (descriptors.json not found).")
            sys.exit(2)
    else:
        print('ERROR: must specify --image-path.')
        sys.exit(2)

    # Run sampling runs. For single-run training prefer a direct single checkpoint invocation
    # that exercises multiple predump iterations (better dry-run sampling inside one run) instead
    # of reusing the chk_rst.sh multi-run wrapper.
    # Determine config_label for this run (defaults to 'sample' when not provided to avoid baseline-only runs)
    cfg = args.config_label if args.config_label else 'sample'
    # Safety: explicitly refuse accidental baseline sampling which produces no dirtymap artifacts
    if cfg == 'baseline' and not args.skip_sampling:
        print("ERROR: config_label 'baseline' disables dirty-track and will not produce sampling data. Use --config-label to select a non-baseline variant or pass --skip-sampling to use existing runs.")
        sys.exit(2)


    if args.skip_sampling:
        print('Skipping sampling; using existing run dirs under tmp_root')
        all_runs = sorted([p for p in glob.glob(os.path.join(tmp_root, '**', 'run_*'), recursive=True) if os.path.isdir(p)])
        runs_found = [p for p in all_runs if re.match(r'^run_\d+$', os.path.basename(p))]
        print('Found existing run dirs:', len(runs_found), 'from', len(all_runs), 'candidates')
        for rd in runs_found:
            try:
                subprocess.run(['python3', os.path.join(SCRIPTS_DIR, 'parse_dirtymap.py'), '--run', rd], check=False, timeout=300)
            except subprocess.TimeoutExpired:
                print('parse_dirtymap.py timed out for', rd)
            except Exception as e:
                print('parse_dirtymap.py failed for', rd, e)
        run_dirs = runs_found
    else:
        if args.runs == 1:
            print('Running single direct checkpoint (no chk_rst.sh) using image_path and predump iterations; config_label=', cfg)
            run_dirs = run_single_checkpoint_direct(bundle_norm, tmp_root, telemetry_sample=1, image_path=image_path, predump_iters=args.predump_iters, config_label=cfg)
        else:
            # For multi-run wrapper, pass config_label through
            run_dirs = run_bootstrap_runs(bundle_norm, args.runs, tmp_root, config_label=cfg, image_path=image_path, predump_iters=args.predump_iters)

    runs_valid = gather_decision_runs(run_dirs)
    if not runs_valid:
        print('No valid runs with decisions.csv found under', tmp_root)
        sys.exit(2)

    # Verify sampled runs actually contain checkpoint/dirtymap artifacts (avoid baseline-only runs)
    has_dirty = False
    for rd in runs_valid:
        ck = os.path.join(rd, 'checkpoint_log')
        if os.path.isdir(ck):
            # quick checks: dirtymap_backup_* or any .dirtymap files
            if glob.glob(os.path.join(ck, 'dirtymap_backup_*')):
                has_dirty = True
                break
            for root, dirs, files in os.walk(ck):
                for fn in files:
                    if fn.endswith('.dirtymap'):
                        has_dirty = True
                        break
                if has_dirty:
                    break
        if has_dirty:
            break
    if not has_dirty:
        print('ERROR: Collected runs do not contain checkpoint/dirtymap artifacts; ensure variant produces dirtymap (not baseline).')
        sys.exit(4)

    data = build_dataset(runs_valid, debug_out_dir=tmp_root, debug_max_rows=args.debug_max_rows, debug_compress=args.debug_compress)
    if not data:
        print('No training data built')
        sys.exit(3)
    X, y, groups, writes, meta = data

    # Save training set aggregate metrics (no eval model applied here)
    try:
        training_metrics = compute_metrics(X, y, meta_df=meta, model_json=None)
        with open(os.path.join(tmp_root, 'training_metrics.json'), 'w') as tf:
            json.dump(training_metrics, tf, indent=2)
        print('Wrote training metrics to', os.path.join(tmp_root, 'training_metrics.json'))
    except Exception as e:
        print('Warning: failed to compute/save training metrics', e)

    out_path = args.out or os.path.join(tmp_root, f'skip_model_{args.bundle}.json')
    model = train_and_export_model(X, y, groups, out_path, writes=writes)
    if model is None:
        print('No model produced due to insufficient label variety; skipping install.')
        sys.exit(5)

    if args.install:
        print('Installing model into', args.model_dir)
        cmd = ['python3', os.path.join(SCRIPTS_DIR, 'install_skip_model.py'), '--src', out_path, '--bundle', bundle_norm, '--make-current']
        subprocess.check_call(cmd)

    print('Done. model at', out_path)

    # Optional evaluation: run an active vs disabled comparison on the same image
    if args.eval:
        print('Running evaluation: active model vs disabled model (single direct runs)')
        eval_res = eval_model_active_vs_disabled(bundle_norm, image_path, args.predump_iters, out_path, tmp_root, config_label=cfg, debug_max_rows=args.debug_max_rows, debug_compress=args.debug_compress)
        print('Evaluation results:')
        print('Active run:', eval_res.get('active'))
        print('Disabled run:', eval_res.get('disabled'))
        # Summarize comparison
        a = eval_res.get('active')
        d = eval_res.get('disabled')
        if a and d and a.get('n') and d.get('n'):
            print(f"Skip fraction: active={a.get('skip_fraction')} (n={a.get('n')}) vs disabled={d.get('skip_fraction')} (n={d.get('n')})")
        else:
            print('Note: evaluation incomplete (missing data)')


if __name__ == '__main__':
    main()
