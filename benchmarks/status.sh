#!/usr/bin/env bash
# One-line progress report of a running or finished benchmark: done/total, elapsed, ETA, current point.
set -euo pipefail
status="$(cd "$(dirname "$0")" && pwd)/results/status.json"
[ -f "$status" ] || { echo "no status.json yet"; exit 0; }
python3 - "$status" <<'EOF'
import json, sys
s = json.load(open(sys.argv[1]))
def hms(x): x = int(x); return f"{x // 3600:d}:{x % 3600 // 60:02d}:{x % 60:02d}"
print(f"{s['done']}/{s['total']} points ({s.get('measured', s['done'])} measured now), elapsed {hms(s['elapsed_s'])}, ETA {hms(s['eta_s'])}, current: {s['current']} (updated {s['updated']})")
EOF
