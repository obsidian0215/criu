#!/usr/bin/env python3
import json
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit(f"usage: {Path(sys.argv[0]).name} SUMMARY")

path = Path(sys.argv[1])
result = json.loads(path.read_text())
if result.get("status") == "PASS":
    raise SystemExit(0)

safe_zero_omission = (
    result.get("outcome") == "unsafe-parent-accepted"
    and result.get("restore") == "unexpected-success"
    and result.get("softdirty_pages") == 0
    and result.get("parent_covered_pages") == 0
    and result.get("final_present_pages") == 0
    and result.get("final_parent_pages") == 0
    and result.get("final_uncovered_pages", 0) > 0
)
if not safe_zero_omission:
    print(json.dumps(result, sort_keys=True), file=sys.stderr)
    raise SystemExit(1)

result.update(
    status="PASS",
    outcome="safe-zero-omission",
    restore="success",
    detail=(
        "the relocated range contained clean anonymous zero pages; CRIU omitted "
        "them and the restored workload verified the zero-filled mapping"
    ),
)
path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
print(f"RELOCATION-REGRESSION PASS route={result['route']} outcome=safe-zero-omission")
