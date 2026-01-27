#!/usr/bin/env python3
"""Run a small grid of dirty-track AB experiments safely and collect analysis outputs.

Usage: scripts/dirty-track/run_grid.py [--workload elasticsearch] [--outdir /tmp/exp] [--dry-run]

This script:
 - accepts a list of configs (hardcoded defaults or via JSON file)
 - for each config: creates a TEMP_ROOT, sets env vars, runs `chk_rst.sh` per config (baseline is run once per container/image)
 - runs parsers and analyzers in scripts/dirty-track to produce per-run artifacts
 - copies selected outputs into a top-level analysis/ directory

Designed to be robust (uses Python subprocess with timeouts and clear logging), avoiding extremely long inline shell invocations that destabilize zsh.
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import re
import time
import concurrent.futures
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / 'scripts' / 'dirty-track'
CHK_RST = REPO_ROOT / 'scripts' / 'dirty-track' / 'chk_rst.sh'
ANALYSIS_DIR = REPO_ROOT / 'analysis'

# Global toggles (set from CLI in main())
RUN_FEATURE_IMPORTANCE = False
PRESERVE_CHECKPOINTS = False

DEFAULT_TIMEOUT = 45 * 60  # 30 minutes per config
DEFAULT_PREDUMP_ITERS = 8
DEFAULT_PARSE_WORKERS = min(4, max(1, os.cpu_count() or 4))

DEFAULT_GRID = [
    # Baseline: no dirty-map
    { 'name': 'baseline' },
    # Disabled: dirty-map enabled, but no skip_model file loaded
    { 'name': 'disabled', 'env': { 'CRIU_SKIP_MODEL_FILE': '', 'CRIU_SKIP_MODEL_DIR': '{TEMP_ROOT}/no_models' } },
    # Active: dirty-map enabled, use pre-trained model if available
    { 'name': 'active' },
]

# Default mapping of container -> variants (images under /root/chk_images/<variant>/image)
CONTAINER_VARIANTS = {
    'redis': ['redis_video_cache__fr70_chk', 'redis_sensoragg_chk', 'redis_cartelem_chk'],
    'influxdb': ['influxdb_sensoragg_chk', 'influxdb_cartelem_chk'],
    'elasticsearch': ['elasticsearch_chk'],
}

DEFAULT_CONTAINERS = ['redis', 'elasticsearch']


def check_prereqs():
    required = [CHK_RST, SCRIPTS_DIR / 'checkpoint_run_impl.sh', SCRIPTS_DIR / 'parse_dirtymap.py', SCRIPTS_DIR / 'analyze_skip_accuracy_fast.py', SCRIPTS_DIR / 'generate_address_series.py']
    missing = [str(p) for p in required if not p.exists()]
    if missing:
        print('ERROR: Missing required scripts or files:')
        for m in missing:
            print('  ', m)
        sys.exit(2)


def run_checkpoint(config, container, workload=None, predump_iters=DEFAULT_PREDUMP_ITERS, workload_duration=None, repeats=None, timeout=DEFAULT_TIMEOUT, dry_run=False, bundle=None, image_override=None, telemetry=False, bandwidth_mb=50, variant_name=None, image_variant=None, shared_temp_root=None, predump_env=None):
    # prepare TEMP_ROOT and env (use shared_temp_root when provided so a single telegrid contains all variants)
    now = int(time.time())
    if shared_temp_root:
        temp_root = Path(shared_temp_root)
    else:
        temp_root = Path(f"/tmp/exp/telegrid_{config['name']}_{container}_{now}_{os.getpid()}")
    temp_root.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    # Do NOT set ROUNDS here: ROUNDS controls the number of outer 'round' runs
    # (chk_rst.sh/checkpoint_run_impl.sh uses ROUNDS for outer loop).  Mapping
    # predump iterations into ROUNDS causes the outer loop to run predump times
    # (creating many containers).  Use PREDUMP_ITERS for inner pre-dump tuning.
    env.update({
        'TEMP_ROOT': str(temp_root),
        'PREDUMP_ITERS': str(predump_iters),  # predump iterations parameter
        'CRIU_DECISION_TELEMETRY': str(telemetry),
        'CRIU_TEST_CONTAINER': container,
        'TRANSFER_BANDWIDTH_MBPS': str(bandwidth_mb),
    })
    if workload_duration:
        env['WORKLOAD_MIGRATION_DURATION'] = str(workload_duration)
    # Expose image variant to downstream scripts so they can create structured directories
    if image_variant:
        env['IMAGE_VARIANT'] = str(image_variant)
    else:
        if image_override:
            env['IMAGE_VARIANT'] = str(Path(image_override).parent.name)
        else:
            env['IMAGE_VARIANT'] = str(container)

    # Determine how many independent repeats (outer repeats) to request from chk_rst.sh
    # Do not infer repeats from predump iteration count; default to 1 when not provided.
    repeats_arg = repeats if repeats is not None else 1
    # Merge any predump tuning parameters provided by caller
    if predump_env:
        for pk, pv in predump_env.items():
            env[str(pk)] = str(pv)

    # Apply per-config environment overrides (e.g., disabled/active model behavior)
    cfg_env = config.get('env') if isinstance(config, dict) else None
    if cfg_env:
        for ek, ev in cfg_env.items():
            if ev is None:
                env.pop(str(ek), None)
                continue
            val = str(ev)
            if '{TEMP_ROOT}' in val:
                val = val.replace('{TEMP_ROOT}', str(temp_root))
            env[str(ek)] = val
        # Ensure no-model dir exists when provided
        nm_dir = env.get('CRIU_SKIP_MODEL_DIR')
        if nm_dir:
            Path(nm_dir).mkdir(parents=True, exist_ok=True)

    # For active runs, select an explicit model file if none provided
    if config.get('name') == 'active':
        if not env.get('CRIU_SKIP_MODEL_FILE'):
            if bundle:
                bundle_name = Path(bundle).name
                candidate = f"/var/lib/criu/skip_models/{bundle_name}/current.json"
                if os.path.exists(candidate):
                    env['CRIU_SKIP_MODEL_FILE'] = candidate
                else:
                    print(f"WARNING: active requested but model file not found at {candidate}; active will run without model")

    # Determine variant name for this run (used to layout outputs)
    vname = variant_name if variant_name else config.get('name', 'B')
    # Pass explicit variant indicators so downstream scripts know which single variant is being executed
    env['SINGLE_VARIANT'] = vname
    env['VARIANT_NAME'] = vname

    if bundle:
        env['BUNDLE'] = bundle
    if image_override:
        env['IMAGE_PATH_OVERRIDE'] = image_override

    # Determine variant name for this run (used to layout outputs)
    vname = variant_name if variant_name else config.get('name', 'B')
    name_arg = config.get('name', vname)
    cmd = ['bash', str(CHK_RST), '--name', name_arg, '--variant', vname, '--repeats', str(repeats_arg), '--bundle', bundle or '', '--bandwidth', str(bandwidth_mb)]

    print(f"Running config {config['name']} (variant={vname}) workload={workload or ''} bundle={bundle or ''} image={image_override or ''} -> {temp_root}  cmd={' '.join(cmd)}")
    if dry_run:
        return temp_root  # just return intended temp dir

    # run the experiment
    try:
        with open(temp_root / 'run.log', 'wb') as out:
            p = subprocess.run(cmd, cwd=REPO_ROOT, env=env, stdout=out, stderr=subprocess.STDOUT, timeout=timeout)
            if p.returncode != 0:
                # Inspect per-run exitcodes and control logs to decide if this non-zero exit is fatal
                exp_root = temp_root / 'experiments'
                failures = False
                if exp_root.exists():
                    for variant_dir in exp_root.iterdir():
                        if not variant_dir.is_dir():
                            continue
                        for run_dir in sorted(variant_dir.glob('run_*')):
                            exitfile = run_dir / 'exitcode.txt'
                            if not exitfile.exists() or exitfile.read_text().strip() != '0':
                                failures = True
                                break
                        if failures:
                            break
                    # check control logs for ERROR markers
                    if not failures:
                        for variant_dir in exp_root.iterdir():
                            ctl = variant_dir / 'control.log'
                            if ctl.exists():
                                txt = ctl.read_text(errors='ignore')
                                if 'ERROR:' in txt or 'Pre-dumping FAILED' in txt:
                                    failures = True
                                    break
                if not failures:
                    print(f"WARNING: chk_rst.sh returned exit {p.returncode} for config {config['name']}, but per-run exitcodes indicate success; see {temp_root}/run.log (non-fatal)")
                else:
                    print(f"ERROR: chk_rst.sh returned exit {p.returncode} for config {config['name']}, see {temp_root}/run.log and experiments/*/control.log")
    except subprocess.TimeoutExpired:
        print(f"ERROR: Run timed out after {timeout}s for config {config['name']}")
    except Exception as e:
        print(f"ERROR: Exception running checkpoint for {config['name']}: {e}")
    return temp_root


def parse_and_analyze(temp_root, parse_timeout):
    # go through experiments/*/run_* and run parse + analysis
    exp_root = temp_root / 'experiments'
    if not exp_root.exists():
        print('No experiments found under', temp_root)
        return
    for variant_dir in exp_root.iterdir():
        if not variant_dir.is_dir():
            continue
        for run_dir in sorted(variant_dir.glob('run_*')):
            print('Processing', run_dir)
            # Skip runs that failed (abort reason present or non-zero exitcode)
            abort_file = run_dir / 'abort_reason.txt'
            exitfile = run_dir / 'exitcode.txt'
            if abort_file.exists():
                print(f"Skipping analysis for {run_dir} due to abort reason: {abort_file.read_text().strip()}")
                continue
            if exitfile.exists():
                try:
                    if exitfile.read_text().strip() != '0':
                        print(f"Skipping analysis for {run_dir} due to non-zero exitcode")
                        continue
                except Exception:
                    print(f"Skipping analysis for {run_dir} because exitcode is unreadable")
                    continue

            # Sanity checks: verify initial restore and workload start markers exist
            if not (run_dir / 'initial_restore_done').exists():
                print(f"Warning: initial_restore_done missing for {run_dir}; this run may not have performed initial restore")
            if not (run_dir / 'workload_started').exists():
                print(f"Note: workload_started marker missing for {run_dir}; workload may not have been started or worked as expected")
            # run parse_dirtymap
            try:
                subprocess.run(['python3', str(SCRIPTS_DIR / 'parse_dirtymap.py'), '--run', str(run_dir)], check=False, timeout=parse_timeout)
            except subprocess.TimeoutExpired as e:
                print('parse_dirtymap.py timed out:', e)
                try:
                    (run_dir / 'parse_dirtymap_timeout.txt').write_text('timed out\n')
                except Exception:
                    pass
            except Exception as e:
                print('parse_dirtymap.py failed:', e)
                try:
                    (run_dir / 'parse_error.txt').write_text('parse error: ' + str(e))
                except Exception:
                    pass

            # Decide whether heavy analyses are meaningful (presence of decision records)
            decisions_file = run_dir / 'decisions.csv'
            has_decisions = False
            try:
                # Primary: non-empty decisions.csv (header + rows)
                if decisions_file.exists():
                    with decisions_file.open('r') as df:
                        line_count = sum(1 for _ in df)
                    if line_count > 1:
                        has_decisions = True

                # Fallback 1: deferred_events.csv produced by parse_dirtymap.py
                if not has_decisions:
                    deferred_csv = run_dir / 'deferred_events.csv'
                    if deferred_csv.exists():
                        with deferred_csv.open('r') as df:
                            dlines = sum(1 for _ in df)
                        if dlines > 1:
                            has_decisions = True
                            print(f"Note: enabling skip-accuracy analysis for {run_dir} via deferred_events.csv fallback")

                # Fallback 2: any non-empty binary deferred_list.* present under the run dir
                if not has_decisions:
                    found_nonzero = False
                    for p in run_dir.rglob('**/deferred_list.*'):
                        try:
                            if p.is_file() and p.stat().st_size > 0:
                                found_nonzero = True
                                break
                        except Exception:
                            continue
                    if found_nonzero:
                        has_decisions = True
                        print(f"Note: enabling skip-accuracy analysis for {run_dir} via deferred_list binary fallback")

            except Exception as e:
                print('Warning: unable to inspect decisions/deferred artifacts for', run_dir, e)

            # analyze non-baseline runs only when decisions exist
            if variant_dir.name != 'baseline' and has_decisions:
                # skip accuracy
                if RUN_SKIP_ACCURACY:
                    try:
                        subprocess.run(['python3', str(SCRIPTS_DIR / 'analyze_skip_accuracy_fast.py'), str(exp_root), variant_dir.name, run_dir.name.split('_')[-1]], check=False, timeout=parse_timeout)
                    except subprocess.TimeoutExpired as e:
                        print('analyze_skip_accuracy_fast.py timed out:', e)
                        try:
                            (run_dir / 'analyze_skip_timeout.txt').write_text('timed out\n')
                        except Exception:
                            pass
                    except Exception as e:
                        print('analyze_skip_accuracy_fast.py failed:', e)
                else:
                    print(f"Skipping skip-accuracy for {run_dir} (disabled by CLIscripts/dirty-track/run_grid.py)")

                # generate per-address series
                if RUN_GENERATE_ADDRESS_SERIES:
                    try:
                        subprocess.run(['python3', str(SCRIPTS_DIR / 'generate_address_series.py'), '--run', str(run_dir), '--out', str(run_dir), '--top', '200'], check=False, timeout=parse_timeout)
                    except subprocess.TimeoutExpired as e:
                        print('generate_address_series.py timed out:', e)
                        try:
                            (run_dir / 'generate_address_series_timeout.txt').write_text('timed out\n')
                        except Exception:
                            pass
                    except Exception as e:
                        print('generate_address_series.py failed:', e)
                else:
                    print(f"Skipping address-series for {run_dir} (disabled by CLI)")
            else:
                if variant_dir.name != 'baseline' and not has_decisions:
                    print(f"Skipping skip-accuracy & address-series for {run_dir} (no decisions found)")
                    try:
                        (run_dir / 'no_decisions_skip_analysis.txt').write_text('no decisions present\n')
                    except Exception:
                        pass
                else:
                    # Baseline: often lacks decisions; skip non-essential analyses
                    print(f"Skipping non-essential analysis for baseline {run_dir}")

            # run feature importance only if decisions are present and enabled
            if has_decisions and RUN_FEATURE_IMPORTANCE:
                try:
                    subprocess.run(['python3', str(SCRIPTS_DIR / 'feature_importance.py'), '--run', str(run_dir), '--out', str(run_dir)], check=False, timeout=parse_timeout)
                except subprocess.TimeoutExpired as e:
                    print('feature_importance.py timed out for run', run_dir, e)
                    try:
                        (run_dir / 'feature_importance_timeout.txt').write_text('timed out\n')
                    except Exception:
                        pass
                except Exception as e:
                    print('feature_importance.py failed for run', run_dir, e)
            else:
                if RUN_FEATURE_IMPORTANCE:
                    print(f"Skipping feature_importance for {run_dir} (no decisions)")

            # copy key artifacts to a per-test analysis subdir for easier inspection
            # Prefer explicit image variant metadata; else infer from structure
            if (run_dir / 'image_variant.txt').exists():
                image_var = (run_dir / 'image_variant.txt').read_text().strip()
            elif len(run_dir.parents) >= 3:
                image_var = run_dir.parents[2].name
            else:
                image_var = variant_dir.name
            dest_dir = ANALYSIS_DIR / temp_root.name / image_var / variant_dir.name / run_dir.name
            dest_dir.mkdir(parents=True, exist_ok=True)
            for f in ['decisions.csv', 'dirtymap_agg.csv', 'problem_pages_time_series.csv', 'problem_pages_summary.csv', 'skip_acc.txt', 'feature_by_decision.csv', 'feature_pairwise_effects.csv', 'feature_importance.csv', 'iteration_metrics.csv', 'time_breakdown.txt', 'total_transfer_ms.txt', 'total_migration_ms.txt', 'downtime_ms.txt', 'bandwidth_used.txt', 'exitcode.txt', 'abort_reason.txt', 'parse_dirtymap_timeout.txt', 'parse_error.txt', 'analyze_skip_timeout.txt', 'generate_address_series_timeout.txt', 'feature_importance_timeout.txt']:
                sf = run_dir / f
                if sf.exists():
                    shutil.copy(sf, dest_dir / f)


def parse_and_analyze_run(run_dir, parse_timeout):
    """Parse and analyze a single run directory (designed to run in a background worker)."""
    run_dir = Path(run_dir)
    print('Background parse/analyze', run_dir)
    abort_file = run_dir / 'abort_reason.txt'
    exitfile = run_dir / 'exitcode.txt'
    if abort_file.exists():
        print(f"Skipping analysis for {run_dir} due to abort reason: {abort_file.read_text().strip()}")
        return
    if exitfile.exists():
        try:
            if exitfile.read_text().strip() != '0':
                print(f"Skipping analysis for {run_dir} due to non-zero exitcode")
                return
        except Exception:
            print(f"Skipping analysis for {run_dir} because exitcode is unreadable")
            return

    # run parse_dirtymap
    try:
        subprocess.run(['python3', str(SCRIPTS_DIR / 'parse_dirtymap.py'), '--run', str(run_dir)], check=False, timeout=parse_timeout)
    except subprocess.TimeoutExpired as e:
        print('parse_dirtymap.py timed out for run', run_dir, e)
        try:
            (run_dir / 'parse_dirtymap_timeout.txt').write_text('timed out\n')
        except Exception:
            pass
    except Exception as e:
        print('parse_dirtymap.py failed for run', run_dir, e)
        try:
            (run_dir / 'parse_error.txt').write_text('parse error: ' + str(e))
        except Exception:
            pass

    # Decide whether heavy analyses are meaningful (presence of decision records)
    decisions_file = run_dir / 'decisions.csv'
    has_decisions = False
    try:
        if decisions_file.exists():
            with decisions_file.open('r') as df:
                line_count = sum(1 for _ in df)
            if line_count > 1:
                has_decisions = True
    except Exception as e:
        print('Warning: unable to inspect decisions.csv for', run_dir, e)

    # Non-baseline analyses only when decisions exist
    variant_dir = run_dir.parent
    if variant_dir.name != 'baseline' and has_decisions:
        run_num = run_dir.name.split('_')[-1]
        # exp_dir_for_analysis should be the parent that contains the variant (works for both layouts)
        exp_dir_for_analysis = str(run_dir.parents[1])
        if RUN_SKIP_ACCURACY:
            try:
                subprocess.run(['python3', str(SCRIPTS_DIR / 'analyze_skip_accuracy_fast.py'), exp_dir_for_analysis, variant_dir.name, run_num], check=False, timeout=parse_timeout)
            except subprocess.TimeoutExpired as e:
                print('analyze_skip_accuracy_fast.py timed out for run', run_dir, e)
                try:
                    (run_dir / 'analyze_skip_timeout.txt').write_text('timed out\n')
                except Exception:
                    pass
            except Exception as e:
                print('analyze_skip_accuracy_fast.py failed for run', run_dir, e)
        else:
            print(f"Skipping skip-accuracy for {run_dir} (disabled by CLI)")

        if RUN_GENERATE_ADDRESS_SERIES:
            try:
                subprocess.run(['python3', str(SCRIPTS_DIR / 'generate_address_series.py'), '--run', str(run_dir), '--out', str(run_dir), '--top', '200'], check=False, timeout=parse_timeout)
            except subprocess.TimeoutExpired as e:
                print('generate_address_series.py timed out for run', run_dir, e)
                try:
                    (run_dir / 'generate_address_series_timeout.txt').write_text('timed out\n')
                except Exception:
                    pass
            except Exception as e:
                print('generate_address_series.py failed for run', run_dir, e)
        else:
            print(f"Skipping address-series for {run_dir} (disabled by CLI)")
    else:
        if variant_dir.name != 'baseline' and not has_decisions:
            print(f"Skipping skip-accuracy & address-series for {run_dir} (no decisions found)")
            try:
                (run_dir / 'no_decisions_skip_analysis.txt').write_text('no decisions present\n')
            except Exception:
                pass
        else:
            # Baseline: often lacks decisions; skip non-essential analyses
            print(f"Skipping non-essential analysis for baseline {run_dir}")

    # run feature importance only if decisions are present and enabled
    if has_decisions and RUN_FEATURE_IMPORTANCE:
        try:
            subprocess.run(['python3', str(SCRIPTS_DIR / 'feature_importance.py'), '--run', str(run_dir), '--out', str(run_dir)], check=False, timeout=parse_timeout)
        except subprocess.TimeoutExpired as e:
            print('feature_importance.py timed out for run', run_dir, e)
            try:
                (run_dir / 'feature_importance_timeout.txt').write_text('timed out\n')
            except Exception:
                pass
        except Exception as e:
            print('feature_importance.py failed for run', run_dir, e)
    else:
        if RUN_FEATURE_IMPORTANCE:
            print(f"Skipping feature_importance for {run_dir} (no decisions)")

    # Copy artifacts into the central analysis area using robust detection of telegrid/container
    try:
        if len(run_dir.parents) >= 3 and run_dir.parents[2].name.startswith('telegrid_'):
            telegrid = run_dir.parents[2].name
        elif len(run_dir.parents) >= 4 and run_dir.parents[3].name.startswith('telegrid_'):
            telegrid = run_dir.parents[3].name
        else:
            telegrid = run_dir.parents[-1].name
        # Prefer explicit image variant metadata; else infer from directory structure
        if (run_dir / 'image_variant.txt').exists():
            image_var = (run_dir / 'image_variant.txt').read_text().strip()
        elif len(run_dir.parents) >= 3:
            # telegrid/<image_variant>/<variant>/run
            image_var = run_dir.parents[2].name
        elif (run_dir / 'container.txt').exists():
            image_var = (run_dir / 'container.txt').read_text().strip()
        else:
            image_var = 'unknown'
        dest_dir = ANALYSIS_DIR / telegrid / image_var / variant_dir.name / run_dir.name
        dest_dir.mkdir(parents=True, exist_ok=True)
        for f in ['decisions.csv', 'dirtymap_agg.csv', 'problem_pages_time_series.csv', 'problem_pages_summary.csv', 'skip_acc.txt', 'feature_by_decision.csv', 'feature_pairwise_effects.csv', 'feature_importance.csv', 'iteration_metrics.csv', 'time_breakdown.txt', 'total_transfer_ms.txt', 'total_migration_ms.txt', 'downtime_ms.txt', 'bandwidth_used.txt', 'exitcode.txt', 'abort_reason.txt', 'parse_dirtymap_timeout.txt', 'parse_error.txt', 'analyze_skip_timeout.txt', 'generate_address_series_timeout.txt', 'feature_importance_timeout.txt']:
            sf = run_dir / f
            if sf.exists():
                shutil.copy(sf, dest_dir / f)
    except Exception as e:
        print('Failed to copy artifacts for run', run_dir, e)

    # Tidy up checkpoint artifacts for successful runs to save disk, unless preservation requested
    if not PRESERVE_CHECKPOINTS:
        try:
            def find_telegrid_root(pth):
                for a in pth.parents:
                    if a.name.startswith('telegrid_'):
                        return a
                return None
            tele_root = find_telegrid_root(run_dir)
            if tele_root:
                ckpt_dir = tele_root / 'ckpt'
                # If this run succeeded, remove checkpoint dirs that encode this run's config/run
                exitcode = ''
                if (run_dir / 'exitcode.txt').exists():
                    exitcode = (run_dir / 'exitcode.txt').read_text().strip()
                if exitcode == '0' and ckpt_dir.exists():
                    cfg = ''
                    if (run_dir / 'config_label.txt').exists():
                        cfg = (run_dir / 'config_label.txt').read_text().strip()
                    safe_cfg = re.sub(r'[^a-zA-Z0-9_.-]', '_', cfg)
                    m = re.search(r'run_(\d+)$', run_dir.name)
                    runnum = m.group(1) if m else None
                    patterns = []
                    if runnum:
                        patterns.append(f'*cfg_{safe_cfg}_run_{runnum}_*')
                        patterns.append(f'*run_{runnum}_*')
                    else:
                        patterns.append(f'*cfg_{safe_cfg}_*')
                    for pat in patterns:
                        for d in ckpt_dir.glob(pat):
                            if not d.is_dir():
                                continue
                            try:
                                shutil.rmtree(d)
                                print(f"Removed ckpt subdir {d} for successful run {run_dir}")
                            except Exception as ex:
                                print('Warning: failed to remove ckpt subdir', d, ex)
        except Exception as e:
            print('Warning: failed to cleanup ckpt for run', run_dir, e)


def schedule_parses_for_temp_root(temp_root, parse_timeout, executor, scheduled_runs, parse_futures):
    temp_root = Path(temp_root)
    # Find run_* directories recursively (supports legacy and nested config-subdir layouts)
    for run_dir in sorted([p for p in temp_root.rglob('run_*') if p.is_dir()]):
        rd = str(run_dir.resolve())
        if rd in scheduled_runs:
            continue
        scheduled_runs.add(rd)
        f = executor.submit(parse_and_analyze_run, run_dir, parse_timeout)
        parse_futures.append(f)


def main():
    p = argparse.ArgumentParser(description='Run dirty-track AB grid and collect analysis outputs')
    p.add_argument('--workload', default=None, help='single workload name (deprecated if --containers used)')
    p.add_argument('--containers', default=','.join(DEFAULT_CONTAINERS), help='comma-separated list of containers to run (default: redis,elasticsearch)')
    p.add_argument('--all', action='store_true', help='run all known containers')
    p.add_argument('--telemetry', type=int, choices=[0,1,2], default=0, help='enable decision telemetry (0=off,1=light,2=full; default 0)')
    p.add_argument('--repeats', type=int, default=None, help='Number of independent repeats (outer loop passed to chk_rst via --repeats). Defaults to 1 when omitted (independent from predump-iters)')
    p.add_argument('--timeout', type=int, default=DEFAULT_TIMEOUT)
    p.add_argument('--parse-timeout', type=int, default=600, help='timeout for per-run parsing/analysis scripts (seconds)')
    p.add_argument('--parse-workers', type=int, default=DEFAULT_PARSE_WORKERS, help='number of concurrent parse workers (default: min(4, cpu_count))')
    p.add_argument('--grid-file', help='optional JSON file describing grid (list of configs)')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--configs', help='JSON array for grid inline')
    p.add_argument('--bandwidth-mbps', type=float, default=50.0, help='Transfer bandwidth in Mbps (default 50)')
    p.add_argument('--workload-duration', type=int, default=None, help='Workload duration in seconds (sets WORKLOAD_MIGRATION_DURATION)')
    p.add_argument('--model-file', default=None, help='Optional skip_model file to load for active runs')
    p.add_argument('--preserve-checkpoints', action='store_true', help='Preserve checkpoint artifacts in telegrid/ckpt (default: delete if all runs succeeded)')
    p.add_argument('--feature-importance', action='store_true', default=False, help='Enable per-run feature importance analysis (disabled by default)')
    p.add_argument('--no-address-series', action='store_true', help='Skip generating per-address time series (saves time)')
    p.add_argument('--no-skip-accuracy', action='store_true', help='Skip running skip-accuracy analysis (saves time)')
    p.add_argument('--no-postprocess', action='store_true', help='Do not run postprocess_grid at the end of the grid (saves time)')

    # Adaptive predump iteration tuning (can be enabled via --predump-adaptive)
    p.add_argument('--predump-adaptive', action='store_true', help='Deprecated: DM-based adaptive stopping is automatic when dirtymap is enabled; flag retained for compatibility.')
    p.add_argument('--predump-iters', type=int, default=8, help='Initial number of predump iterations (default 8)')
    p.add_argument('--predump-min-gain-ratio', type=float, default=0.01, help='Gain ratio threshold to consider low progress (default 0.01)')
    p.add_argument('--predump-converge-rounds', type=int, default=2, help='Rounds of low gain to consider converged (default 2)')
    p.add_argument('--predump-confirm-rounds', type=int, default=0, help='Extra confirmation rounds after convergence detected (default 0)')
    p.add_argument('--predump-min-pages', type=int, default=64, help='Absolute page count threshold considered small enough to treat as converged (default 64)')
    p.add_argument('--predump-incr-ratio', type=float, default=1.02, help='Increase ratio to detect non-converging growth (default 1.02)')
    p.add_argument('--predump-max-iters', type=int, default=16, help='Maximum allowed predump iterations when adaptive (default 16)')
    p.add_argument('--predump-extend-ratio', type=float, default=0.10, help='If improvement ratio > this, extend predump iterations (default 0.10)')
    p.add_argument('--predump-extend-add', type=int, default=2, help='Number of additional iterations to add when extending (default 2)')
    args = p.parse_args()

    # Configure global toggles from CLI args so background parse workers can consult them
    global RUN_FEATURE_IMPORTANCE, PRESERVE_CHECKPOINTS, RUN_GENERATE_ADDRESS_SERIES, RUN_SKIP_ACCURACY, RUN_POSTPROCESS, RUN_PREDUMP_ADAPTIVE
    RUN_FEATURE_IMPORTANCE = bool(args.feature_importance)
    PRESERVE_CHECKPOINTS = bool(args.preserve_checkpoints)
    RUN_GENERATE_ADDRESS_SERIES = not bool(args.no_address_series)
    RUN_SKIP_ACCURACY = not bool(args.no_skip_accuracy)
    RUN_POSTPROCESS = not bool(args.no_postprocess)
    RUN_PREDUMP_ADAPTIVE = bool(args.predump_adaptive)

    # Build predump env that will be passed into each run so checkpoint_run_impl.sh can be tuned
    predump_env = {
        'PREDUMP_ITERS': str(args.predump_iters),
        'PREDUMP_MIN_GAIN_RATIO': str(args.predump_min_gain_ratio),
        'PREDUMP_CONVERGE_ROUNDS': str(args.predump_converge_rounds),
        'PREDUMP_CONFIRM_ROUNDS': str(args.predump_confirm_rounds),
        'PREDUMP_MIN_PAGES_ABS': str(args.predump_min_pages),
        'PREDUMP_INCR_RATIO': str(args.predump_incr_ratio),
        'PREDUMP_MAX_ITERS': str(args.predump_max_iters),
        'PREDUMP_EXTEND_RATIO': str(args.predump_extend_ratio),
        'PREDUMP_EXTEND_ADD': str(args.predump_extend_add),
    }

    check_prereqs()
    ANALYSIS_DIR.mkdir(parents=True, exist_ok=True)

    # Background parse worker pool to allow parsing/analyzing runs in parallel with ongoing runs
    parse_executor = concurrent.futures.ThreadPoolExecutor(max_workers=args.parse_workers)
    parse_futures = []
    scheduled_runs = set()

    if args.grid_file:
        grid = json.loads(Path(args.grid_file).read_text())
    elif args.configs:
        grid = json.loads(args.configs)
    else:
        grid = DEFAULT_GRID

    # If a model file is provided, wire it into the 'active' config
    if args.model_file:
        for cfg in grid:
            if cfg.get('name') == 'active':
                cfg_env = dict(cfg.get('env', {}))
                cfg_env['CRIU_SKIP_MODEL_FILE'] = str(args.model_file)
                cfg['env'] = cfg_env

    # Normalize config list: preserve per-config target containers if provided.
    expanded_grid = []
    for cfg in grid:
        target_containers = None
        if 'containers' in cfg:
            target_containers = [c.strip() for c in cfg['containers']]
        cfg_copy = dict(cfg)
        if target_containers:
            cfg_copy['_target_containers'] = target_containers
        expanded_grid.append(cfg_copy)
    grid = expanded_grid

    # Determine containers to run
    if args.all:
        containers = list(CONTAINER_VARIANTS.keys())
    else:
        containers = [c.strip() for c in args.containers.split(',') if c.strip()]

    # Backwards compatibility: if a single workload is provided (legacy) and no containers, use it
    if args.workload and (not args.containers):
        containers = [args.workload]

    # Create a single top-level telegrid for this grid run (first-level: experiment)
    now = int(time.time())
    container_str = '_'.join(sorted(containers)) if containers else 'mixed'
    top_temp_root = Path(f"/tmp/exp/telegrid_{container_str}_{now}_{os.getpid()}")
    top_temp_root.mkdir(parents=True, exist_ok=True)
    print(f"Using shared TEMP_ROOT for this grid run: {top_temp_root}")

    for cfg in grid:
        cfg_containers = cfg.get('_target_containers')
        for container in containers:
            # Respect per-config container restriction, if any
            if cfg_containers and container not in cfg_containers:
                print(f"Skipping config {cfg.get('name')} for container {container} (not in target list)")
                continue
            if container not in CONTAINER_VARIANTS:
                print(f"WARNING: unknown container '{container}', skipping")
                continue
            for variant in CONTAINER_VARIANTS[container]:
                image_path = Path('/root/chk_images') / variant / 'image'
                image_override = str(image_path) if image_path.exists() else None
                bundle = f"/runc/containers/{container}"

                # Run the requested config (single-variant repeated runs)
                cfg_name = cfg.get('name', 'config')
                # Variant name is just the config name (image/service is represented by the parent directory)
                cfg_variant_name = cfg_name
                temp_root = run_checkpoint(cfg, variant, workload=None, predump_iters=args.predump_iters, workload_duration=args.workload_duration, repeats=args.repeats, timeout=args.timeout, dry_run=args.dry_run, bundle=bundle, image_override=image_override, telemetry=args.telemetry, bandwidth_mb=args.bandwidth_mbps, variant_name=cfg_variant_name, image_variant=variant, shared_temp_root=str(top_temp_root), predump_env=predump_env)
                schedule_parses_for_temp_root(temp_root, args.parse_timeout, parse_executor, scheduled_runs, parse_futures)

    # Wait for any background parse/analyze tasks to finish before aggregating results
    if parse_futures:
        print("Waiting for background parse/analyze tasks to finish...")
        concurrent.futures.wait(parse_futures, return_when=concurrent.futures.ALL_COMPLETED)
        parse_executor.shutdown(wait=True)
        print("All parse/analyze tasks completed.")

    # Consolidate run_meta.json -> run_meta.json for easier consumption by analysis tools
    meta_file = Path(top_temp_root) / 'run_meta.json'
    if meta_file.exists():
        try:
            raw_lines = [l for l in meta_file.read_text().splitlines() if l.strip()]
            entries = [json.loads(l) for l in raw_lines]
            # Deduplicate entries by (container, variant, round). Keep the earliest start_time when duplicates exist.
            dedup = {}
            for e in entries:
                key = (e.get('container'), e.get('variant'), e.get('round'))
                if key not in dedup or e.get('start_time', '') < dedup[key].get('start_time', ''):
                    dedup[key] = e
            entries = sorted(dedup.values(), key=lambda x: (x.get('container',''), x.get('variant',''), x.get('round',0)))
            meta_out = Path(top_temp_root) / 'run_meta.json'
            meta_out.write_text(json.dumps(entries, indent=2))
            dest = ANALYSIS_DIR / Path(top_temp_root).name / 'run_meta.json'
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(str(meta_out), str(dest))
        except Exception as e:
            print('Failed to consolidate run_meta.json:', e)

    # Aggregate per-variant evaluation metrics into a single comparison CSV
    try:
        comp_rows = []
        models_dir = ANALYSIS_DIR / Path(top_temp_root).name / 'models'
        if models_dir.exists():
            for f in sorted(models_dir.glob('*_metrics.json')):
                try:
                    data = json.loads(f.read_text())
                except Exception:
                    print('Warning: failed to load metrics JSON', f)
                    continue
                # Parse filename: expected <container>_<variant>_<mode>_metrics.json
                name = f.name
                base = name[:-len('_metrics.json')]
                parts = base.split('_')
                if len(parts) < 3:
                    print('Skipping unexpected metrics file name:', name)
                    continue
                mode = parts[-1]
                container_name = parts[0]
                variant_name = '_'.join(parts[1:-1])
                row = {
                    'container': container_name,
                    'variant': variant_name,
                    'mode': mode,
                    'n': data.get('n'),
                    'skip_fraction': data.get('skip_fraction'),
                    'label0': data.get('label_dist', {}).get('0'),
                    'label1': data.get('label_dist', {}).get('1'),
                    'roc_auc': data.get('roc_auc'),
                    'pr_auc': data.get('pr_auc'),
                    'precision': data.get('precision'),
                    'recall': data.get('recall'),
                    'f1': data.get('f1'),
                    'bootstrap_ci': data.get('bootstrap_ci', {}),
                    'metrics_file': str(f)
                }
                comp_rows.append(row)
        # write CSV
        if comp_rows:
            import csv as _csv
            comp_file = ANALYSIS_DIR / Path(top_temp_root).name / 'comparison.csv'
            comp_file.parent.mkdir(parents=True, exist_ok=True)
            fieldnames = ['container','variant','mode','n','skip_fraction','label0','label1','roc_auc','pr_auc','precision','recall','f1','metrics_file']
            with comp_file.open('w', newline='') as cf:
                w = _csv.DictWriter(cf, fieldnames=fieldnames)
                w.writeheader()
                for r in comp_rows:
                    # flatten basic fields only for CSV readability
                    w.writerow({k: r.get(k) for k in fieldnames})
            print('Wrote comparison summary to', comp_file)
    except Exception as e:
        print('Failed to aggregate comparison metrics:', e)

    # regenerate mc summary if possible
    try:
        subprocess.run(['python3', str(SCRIPTS_DIR / 'aggregate_mc_results.py')], check=False)
    except Exception:
        pass

    # Postprocess the telegrid we just created into per-telegrid analysis dir
    if RUN_POSTPROCESS:
        try:
            subprocess.run(['python3', str(SCRIPTS_DIR / 'postprocess_grid.py'), '--telegrid', str(top_temp_root), '--out', str(ANALYSIS_DIR)], check=False)
        except Exception:
            pass
    else:
        print(f"Skipping postprocess for telegrid {top_temp_root} (disabled by CLI)")

    # Checkpoint cleanup: by default preserve only FAILED runs' checkpoint artifacts to save disk.
    if not args.preserve_checkpoints:
        exp_root = top_temp_root / 'experiments'
        failed_runs = set()
        if exp_root.exists():
            for variant_dir in exp_root.iterdir():
                if not variant_dir.is_dir():
                    continue
                for run_dir in sorted(variant_dir.glob('run_*')):
                    failed = False
                    exitfile = run_dir / 'exitcode.txt'
                    if exitfile.exists():
                        try:
                            if exitfile.read_text().strip() != '0':
                                failed = True
                        except Exception:
                            failed = True
                    # Scan run-specific logs for obvious failures if exitcode was 0
                    if not failed:
                        for f in run_dir.rglob('*.log'):
                            if not f.is_file():
                                continue
                            try:
                                txt = f.read_text(errors='ignore')
                            except Exception:
                                continue
                            if 'Pre-dumping FAILED' in txt or 'ERROR:' in txt or 'SKIP_ACCURACY_ABORT' in txt or 'failed to start' in txt.lower() or 'failed to ensure temporary recvtty' in txt.lower():
                                failed = True
                                break
                    if failed:
                        failed_runs.add(str(run_dir.resolve()))
        ckpt_dir = top_temp_root / 'ckpt'
        if not ckpt_dir.exists():
            pass
        else:
            # New behavior: only keep per-run checkpoint subdirs that unambiguously map to failed runs.
            # Legacy whole-dir preservation is removed; unknown-origin ckpts will be deleted.
            per_run_dirs = [d for d in ckpt_dir.iterdir() if d.is_dir()]
            if not per_run_dirs:
                # Legacy layout (no per-run subdirs): remove whole ckpt dir unless user explicitly asked to preserve
                if not args.preserve_checkpoints:
                    try:
                        shutil.rmtree(ckpt_dir)
                        print(f"Removed legacy checkpoint directory {ckpt_dir}")
                    except Exception as e:
                        print(f"Warning: failed to remove legacy ckpt dir: {e}")
            else:
                # Build a mapping (config_label, run_num) -> run_dir path for quick lookups
                run_map = {}
                for run_candidate in top_temp_root.rglob('run_*'):
                    try:
                        cfg = run_candidate.joinpath('config_label.txt').read_text().strip() if (run_candidate.joinpath('config_label.txt')).exists() else ''
                    except Exception:
                        cfg = ''
                    m = re.search(r'run_(\d+)$', run_candidate.name)
                    if m:
                        run_num = m.group(1)
                        run_map[(cfg, run_num)] = str(run_candidate.resolve())
                # Decide per-ckpt dir whether to keep it
                for d in per_run_dirs:
                    keep = False
                    nm = d.name
                    # Expect ckpt dirs to encode 'cfg_<config>_run_<N>' (created by checkpoint_run_impl.sh)
                    m = re.search(r'cfg_([^_]+)_run_(\d+)', nm)
                    if m:
                        cfg = m.group(1)
                        runnum = m.group(2)
                        mapped = run_map.get((cfg, runnum))
                        if mapped and mapped in failed_runs:
                            keep = True
                    else:
                        # fallback: match run_<N> anywhere in name and check if corresponding run is failed
                        m2 = re.search(r'run_(\d+)', nm)
                        if m2:
                            runnum = m2.group(1)
                            candidates = [v for (c, r), v in run_map.items() if r == runnum]
                            if any(cand in failed_runs for cand in candidates):
                                keep = True
                    if keep:
                        print(f"Preserving checkpoint for failed run at {d}")
                    else:
                        try:
                            shutil.rmtree(d)
                            print(f"Removed checkpoint {d} (no associated failed run)")
                        except Exception as e:
                            print(f"Warning: failed to remove ckpt subdir {d}: {e}")
                try:
                    if not any(ckpt_dir.iterdir()):
                        shutil.rmtree(ckpt_dir)
                except Exception:
                    pass

    print('Grid complete. Check', ANALYSIS_DIR)


if __name__ == '__main__':
    main()
