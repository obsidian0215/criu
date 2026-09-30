#!/usr/bin/env python3
"""Summarize complete paired observations without suppressing failures."""
import json
from pathlib import Path
import statistics
import sys

root = Path(sys.argv[1])
rows = [json.loads(line) for line in (root / 'results.jsonl').read_text().splitlines()]
measured = [row for row in rows if not row['warmup']]
summary = {'measured_runs': len(measured),
           'failures': [row for row in measured if row['status'] != 'passed'],
           'conditions': []}


def metrics(row):
    return {'all_rounds_destination_ready_ms': sum(x['destination_ready_ms'] for x in row['rounds']),
            'final_destination_ready_ms': row['rounds'][-1]['destination_ready_ms'],
            'all_rounds_tcp_data_bytes_sent': sum(x['tcp']['sender_tcp_data_bytes_sent'] + x['tcp']['receiver_tcp_data_bytes_sent'] for x in row['rounds']),
            'final_payload_bytes': row['rounds'][-1]['target']['page_payload_bytes'],
            'all_rounds_payload_bytes': sum(x['target']['page_payload_bytes'] for x in row['rounds']),
            'all_rounds_child_cpu_s': sum(x['child_user_cpu_s'] + x['child_system_cpu_s'] for x in row['rounds']),
            'final_frozen_time_us': row['rounds'][-1]['stats']['entries'][0]['dump']['frozen_time'],
            'final_pages_written': row['rounds'][-1]['stats']['entries'][0]['dump']['pages_written'],
            'final_pages_skipped_parent': row['rounds'][-1]['stats']['entries'][0]['dump']['pages_skipped_parent'],
            'restore_ms': row['restore_ms'],
            'final_start_to_external_verification_ms': row['final_start_to_external_verification_ms']}


for dirty in sorted({row['dirty_percent'] for row in measured}):
    condition = {'dirty_percent': dirty, 'routes': {}, 'candidate_minus_baseline': {}}
    valid = [row for row in measured if row['dirty_percent'] == dirty and row['status'] == 'passed']
    for route in sorted({row['route'] for row in valid}):
        group = [metrics(row) for row in valid if row['route'] == route]
        condition['routes'][route] = {key: {'median': statistics.median(x[key] for x in group),
                                               'values': [x[key] for x in group]}
                                      for key in group[0]}
    baseline = {row['repetition']: metrics(row) for row in valid if row['route'] == 'baseline'}
    candidate = {row['repetition']: metrics(row) for row in valid if row['route'] == 'candidate'}
    pairs = sorted(baseline.keys() & candidate.keys())
    if pairs:
        for key in baseline[pairs[0]]:
            differences = [candidate[p][key] - baseline[p][key] for p in pairs]
            condition['candidate_minus_baseline'][key] = {
                'paired_differences': differences, 'median_paired_difference': statistics.median(differences),
                'observed_min': min(differences), 'observed_max': max(differences)}
    summary['conditions'].append(condition)
print(json.dumps(summary, indent=2))
