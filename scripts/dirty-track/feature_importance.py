#!/usr/bin/env python3
"""Simple feature importance analysis for DEFER -> FORCE_DUMP outcomes.

Scans telegrid runs under /tmp/exp, extracts DEFER decision events and labels whether
that same (pid, addr) later had a FORCE_DUMP (or legacy FORCE_SEND) in the same run. Computes univariate
statistics (mean, std) and effect sizes (Cohen's d and point-biserial correlation)
for candidate numeric features.

Usage: scripts/dirty-track/feature_importance.py [--root /tmp/exp] [--out analysis]
"""

import argparse
import csv
import glob
import math
import os
import sys
from collections import defaultdict
from pathlib import Path

NUMERIC_FEATURES = ['heat', 'heat_trend', 'writes_est', 'track_s', 'hist_count', 'hist_mean', 'hist_variance', 'deferred', 'hist_declining', 'p', 'p_model', 'score', 'pthr']


def read_decisions(decisions_file):
    rows = []
    if not os.path.exists(decisions_file):
        return rows
    with open(decisions_file, newline='') as f:
        r = csv.DictReader(f)
        for row in r:
            # normalize address and numeric types
            try:
                row['addr_int'] = int(row.get('addr',''), 16)
            except Exception:
                row['addr_int'] = None

            # integer fields
            for k in ['round','pre_dump','dhm_present','deferred','is_warm','hist_count','hist_is_declining','hist_declining']:
                if k in row and row[k] != '':
                    try:
                        row[k] = int(row[k])
                    except Exception:
                        pass

            # float fields
            for k in ['heat','heat_trend','writes_est','track_s','hist_mean','hist_variance','p','score','pthr']:
                if k in row and row[k] != '':
                    try:
                        row[k] = float(row[k])
                    except Exception:
                        pass

            rows.append(row)
    return rows


def cohen_d(x1, x2):
    n1 = len(x1)
    n2 = len(x2)
    if n1 < 2 or n2 < 2:
        return None
    m1 = sum(x1)/n1
    m2 = sum(x2)/n2
    s1 = math.sqrt(sum((a-m1)**2 for a in x1)/(n1-1))
    s2 = math.sqrt(sum((a-m2)**2 for a in x2)/(n2-1))
    pooled = math.sqrt(((n1-1)*s1*s1 + (n2-1)*s2*s2)/(n1+n2-2))
    if pooled == 0:
        return None
    return (m1 - m2) / pooled


def point_biserial(feature_vals, labels):
    # labels are 0/1
    n = len(feature_vals)
    if n < 2:
        return None
    mean_all = sum(feature_vals)/n
    n1 = sum(labels)
    n0 = n - n1
    if n1 == 0 or n0 == 0:
        return None
    mean1 = sum(v for v,l in zip(feature_vals, labels) if l==1) / n1
    mean0 = sum(v for v,l in zip(feature_vals, labels) if l==0) / n0
    sd_all = math.sqrt(sum((v-mean_all)**2 for v in feature_vals)/ (n-1))
    if sd_all == 0:
        return None
    r_pb = (mean1 - mean0) * math.sqrt((n1 * n0) / (n * (n - 1))) / sd_all
    return r_pb


def analyze(root, outdir, run_dir=None, telegrid=None):
    events = []  # each event is a dict with decision and numeric features

    if run_dir:
        run_path = Path(run_dir)
        decisions_file = run_path / 'decisions.csv'
        if not decisions_file.exists():
            print('No decisions.csv in', run_path)
            return
        decisions = read_decisions(str(decisions_file))
        for d in decisions:
            if int(d.get('pre_dump') or 0) == 0:
                continue
            dec = d.get('decision')
            if dec == 'SEND':
                dec = 'DUMP'
            elif dec == 'FORCE_SEND':
                dec = 'FORCE_DUMP'
            ev = {'telegrid': run_path.parent.parent.parent.name if run_path.parent.parent.parent else run_path.name, 'config': run_path.parent.parent.name if run_path.parent.parent else run_path.name, 'variant': run_path.parent.name, 'run': run_path.name, 'decision': dec}
            for f in NUMERIC_FEATURES:
                v = d.get(f)
                ev[f] = float(v) if v not in (None, '') else None
            events.append(ev)
        # default outdir to run_dir if not explicitly provided
        if not outdir:
            outdir = str(run_path)
    else:
        root = Path(root)
        # We'll collect decision events across runs and analyze by decision label
        if telegrid:
            tgs = [Path(telegrid)]
        else:
            tgs = sorted(root.glob('telegrid_*'))
        for tg in tgs:
            exps = tg / 'experiments'
            if not exps.exists():
                continue
            for variant_dir in sorted(exps.iterdir()):
                if not variant_dir.is_dir():
                    continue
                for run_dir in sorted(variant_dir.glob('run_*')):
                    decisions_file = run_dir / 'decisions.csv'
                    if not decisions_file.exists():
                        continue
                    decisions = read_decisions(str(decisions_file))
                    # focus on pre-dump decisions (page selection)
                    for d in decisions:
                        if int(d.get('pre_dump') or 0) == 0:
                            continue
                        dec = d.get('decision')
                        if dec == 'SEND':
                            dec = 'DUMP'
                        elif dec == 'FORCE_SEND':
                            dec = 'FORCE_DUMP'
                        ev = {'telegrid': tg.name, 'config': tg.name.split('_')[1] if '_' in tg.name else tg.name, 'variant': variant_dir.name, 'run': run_dir.name, 'decision': dec}
                        for f in NUMERIC_FEATURES:
                            v = d.get(f)
                            ev[f] = float(v) if v not in (None, '') else None
                        events.append(ev)

    # group by decision label
    by_decision = defaultdict(list)
    for ev in events:
        by_decision[ev['decision']].append(ev)

    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    # Write per-feature per-decision stats
    stats_csv = outdir / 'feature_by_decision.csv'
    with open(stats_csv, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['feature','decision','n','mean','std'])
        for feat in NUMERIC_FEATURES:
            for dec, items in sorted(by_decision.items()):
                vals = [it[feat] for it in items if it[feat] is not None]
                if not vals:
                    continue
                mean_v = sum(vals)/len(vals)
                std_v = math.sqrt(sum((x-mean_v)**2 for x in vals)/(len(vals)-1)) if len(vals)>1 else 0.0
                w.writerow([feat, dec, len(vals), mean_v, std_v])

    # Focused pairwise comparisons: DUMP vs DEFER (common in current data), DEFER vs FORCE_DUMP
    comparisons = [('DUMP','DEFER'), ('DEFER','FORCE_DUMP')]
    comp_csv = outdir / 'feature_pairwise_effects.csv'
    with open(comp_csv, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['feature','cmp','n_a','n_b','mean_a','mean_b','cohen_d','r_pb'])
        for a,b in comparisons:
            items_a = by_decision.get(a, [])
            items_b = by_decision.get(b, [])
            for feat in NUMERIC_FEATURES:
                vals_a = [it[feat] for it in items_a if it[feat] is not None]
                vals_b = [it[feat] for it in items_b if it[feat] is not None]
                if not vals_a or not vals_b:
                    continue
                d = cohen_d(vals_a, vals_b)
                rpb = point_biserial(vals_a+vals_b, [1]*len(vals_a) + [0]*len(vals_b))
                w.writerow([feat, f"{a}_vs_{b}", len(vals_a), len(vals_b), sum(vals_a)/len(vals_a), sum(vals_b)/len(vals_b), d, rpb])

    print('Wrote per-decision feature stats ->', stats_csv)
    print('Wrote pairwise effects ->', comp_csv)

    # Print top distinguishing features for SEND vs DEFER
    def print_top_cmp(a,b, topn=10):
        rows = []
        items_a = by_decision.get(a, [])
        items_b = by_decision.get(b, [])
        for feat in NUMERIC_FEATURES:
            vals_a = [it[feat] for it in items_a if it[feat] is not None]
            vals_b = [it[feat] for it in items_b if it[feat] is not None]
            if not vals_a or not vals_b:
                continue
            d = cohen_d(vals_a, vals_b) or 0
            rows.append((abs(d), feat, d))
        rows.sort(reverse=True)
        print(f"Top features distinguishing {a} vs {b}:")
        for _,feat,d in rows[:topn]:
            print(f"  {feat}: d={d}")

    for a,b in comparisons:
        print_top_cmp(a,b)

    # Build an overall feature importance based on SEND vs DEFER (if available)
    base_a = by_decision.get('SEND', [])
    base_b = by_decision.get('DEFER', [])
    by_feat = {}
    for feat in NUMERIC_FEATURES:
        vals_a = [it[feat] for it in base_a if it[feat] is not None]
        vals_b = [it[feat] for it in base_b if it[feat] is not None]
        if not vals_a or not vals_b:
            continue
        mean_a = sum(vals_a)/len(vals_a)
        mean_b = sum(vals_b)/len(vals_b)
        d = cohen_d(vals_a, vals_b)
        rpb = point_biserial(vals_a+vals_b, [1]*len(vals_a) + [0]*len(vals_b))
        by_feat[feat] = {'mean_pos': mean_a, 'mean_neg': mean_b, 'n_pos': len(vals_a), 'n_neg': len(vals_b), 'd': d, 'rpb': rpb}

    csv_out = outdir / 'feature_importance.csv'
    with open(csv_out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['feature','n_pos','n_neg','mean_pos','mean_neg','cohen_d','point_biserial'])
        for feat, info in sorted(by_feat.items(), key=lambda x: abs(x[1].get('d') or 0), reverse=True):
            w.writerow([feat, info['n_pos'], info['n_neg'], info['mean_pos'], info['mean_neg'], info['d'], info['rpb']])
    print('Wrote feature importance ->', csv_out)
    # print brief ranking
    print('\nTop features by |Cohen d| (SEND vs DEFER):')
    for feat, info in sorted(by_feat.items(), key=lambda x: abs(x[1].get('d') or 0), reverse=True)[:10]:
        print(f"  {feat}: d={info['d']}, rpb={info['rpb']}, send_n={info['n_pos']}, skip_n={info['n_neg']}")


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--root', default='/tmp/exp', help='root telegrid dir to scan')
    p.add_argument('--telegrid', default=None, help='Path to a specific telegrid dir to scan')
    p.add_argument('--out', default='analysis', help='output directory for aggregated results (ignored when --run is used)')
    p.add_argument('--run', default=None, help='path to a single run dir (e.g., /tmp/exp/telegrid_*/experiments/B/run_1)')
    args = p.parse_args()
    if args.run:
        # derive telegrid name from run path if possible
        run_path = Path(args.run)
        tg_name = None
        for p in run_path.parents:
            if p.name.startswith('telegrid_') or p.name.startswith('mc_') or p.name.startswith('run_'):
                tg_name = p.name
                break
        tg_name = tg_name or run_path.name
        outdir = Path(args.out) / tg_name
        outdir.mkdir(parents=True, exist_ok=True)
        analyze(args.root, str(outdir), run_dir=args.run)
    elif args.telegrid:
        outdir = Path(args.out) / Path(args.telegrid).name
        outdir.mkdir(parents=True, exist_ok=True)
        analyze(args.root, str(outdir), run_dir=None, telegrid=args.telegrid)
    else:
        print('ERROR: must specify --run or --telegrid to avoid scanning all telegrids')
        sys.exit(2)

