#!/usr/bin/env python3
"""
Aggregate multi-container experiment results under a single test root (e.g., /tmp/exp/mc_*).
Produces a summary CSV `analysis/mc_summary.csv` and prints a short human summary.
"""
import os
import glob
import csv
import subprocess
from statistics import mean
import argparse

p = argparse.ArgumentParser(description='Aggregate multi-container experiment results')
p.add_argument('--root', default=os.environ.get('TEST_ROOT', '/tmp/exp'), help='root directory containing mc_* experiment directories')
args = p.parse_args()
TEST_ROOT = args.root
MC_DIRS = sorted(glob.glob(os.path.join(TEST_ROOT, 'mc_*')))
OUT_DIR = 'analysis'
os.makedirs(OUT_DIR, exist_ok=True)

rows = []

for mc in MC_DIRS:
    # for each variant folder under this MC run
    for variant_dir in sorted(glob.glob(os.path.join(mc, '*'))):
        name = os.path.basename(variant_dir)
        exp_root = os.path.join(variant_dir, 'experiments')
        if not os.path.isdir(exp_root):
            continue
        for v in ['A', 'B']:
            run_dirs = sorted(glob.glob(os.path.join(exp_root, v, 'run_*')))
            if not run_dirs:
                continue
            # If any run in this policy failed, exclude the entire policy/variant from aggregation
            failed = False
            good_run_dirs = []
            for rdir in run_dirs:
                abort = os.path.join(rdir, 'abort_reason.txt')
                exitf = os.path.join(rdir, 'exitcode.txt')
                if os.path.exists(abort):
                    failed = True
                    break
                if os.path.exists(exitf):
                    try:
                        with open(exitf) as ef:
                            if ef.read().strip() != '0':
                                failed = True
                                break
                    except Exception:
                        failed = True
                        break
                good_run_dirs.append(rdir)
            if failed or not good_run_dirs:
                print(f"Skipping aggregation for policy {v} in variant {name} due to failed runs or no successful runs")
                continue
            dump_pages = []
            accuracies = []
            migration_times = []
            processed_runs = 0
            for rdir in good_run_dirs:
                # dump pages
                dp = os.path.join(rdir, 'dump_pages.txt')
                if os.path.isfile(dp):
                    try:
                        with open(dp) as f:
                            dump_pages.append(int(f.read().strip()))
                    except Exception:
                        pass
                # migration time (if available)
                mf = os.path.join(rdir, 'total_migration_ms.txt')
                if os.path.isfile(mf):
                    try:
                        with open(mf) as f:
                            migration_times.append(int(f.read().strip()))
                    except Exception:
                        pass
                # run fast analyzer for this run (only B has decisions usually)
                try:
                    p = subprocess.run(['python3', 'scripts/dirty-track/analyze_skip_accuracy_fast.py', exp_root, v, rdir.split('/')[-1].split('_')[-1]], capture_output=True, text=True, timeout=120)
                    out = p.stdout
                    for line in out.splitlines():
                        if 'Overall Accuracy' in line:
                            # extract accuracy float
                            acc = float(line.split(':')[-1].strip().rstrip('%'))
                            accuracies.append(acc)
                except Exception:
                    pass
            row = {
                'mc_run': os.path.basename(mc),
                'variant': name,
                'policy': v,
                'n_runs': len(run_dirs),
                'avg_pages': mean(dump_pages) if dump_pages else '',
                'pages_list': ';'.join(map(str, dump_pages)) if dump_pages else '',
                'avg_skip_acc': mean(accuracies) if accuracies else '',
                'acc_list': ';'.join([f"{x:.1f}" for x in accuracies]) if accuracies else '',
                'avg_migration_ms': mean(migration_times) if migration_times else ''
            }
            rows.append(row)

out_csv = os.path.join(OUT_DIR, 'mc_summary.csv')
with open(out_csv, 'w', newline='') as f:
    fieldnames = ['mc_run','variant','policy','n_runs','avg_pages','pages_list','avg_skip_acc','acc_list','avg_migration_ms']
    w = csv.DictWriter(f, fieldnames=fieldnames)
    w.writeheader()
    for r in rows:
        w.writerow(r)

print('Wrote summary to', out_csv)

# Print a short human summary
print('\nHuman summary per variant (avg pages A vs B, avg skip acc for B):\n')
by_variant = {}
for r in rows:
    key = r['variant']
    by_variant.setdefault(key, {})[r['policy']] = r

for k, v in by_variant.items():
    a = v.get('A')
    b = v.get('B')
    a_pages = a['avg_pages'] if a else ''
    b_pages = b['avg_pages'] if b else ''
    b_acc = f"{b['avg_skip_acc']:.1f}%" if b and b['avg_skip_acc'] != '' else 'N/A'
    print(f"{k}: A avg_pages={a_pages}, B avg_pages={b_pages}, B avg_skip_acc={b_acc}")
