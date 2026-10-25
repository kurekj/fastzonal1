#!/usr/bin/env bash
# Quick local timing of the library driver on one fixture (development aid, not the published
# matrix): bash tools/local_bench.sh <fixtureDir> [threads]
set -euo pipefail
fx="$1"; threads="${2:-8}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp /tmp/local_bench_XXXXXX.csv)"
b="$repo/build/release/fz_bench"
"$b" "$fx" mean grid100 dem30 --backend serial --reps 5 --out "$out" > /dev/null
"$b" "$fx" mean grid100 dem30 --backend openmp --threads "$threads" --reps 5 --out "$out" > /dev/null
"$b" "$fx" majority grid100 clc100 --backend serial --reps 3 --out "$out" > /dev/null
"$b" "$fx" majority grid100 clc100 --backend openmp --threads "$threads" --reps 3 --out "$out" > /dev/null
"$b" "$fx" rasterize grid100 dem30 --backend serial --reps 3 --out "$out" > /dev/null
"$b" "$fx" rasterize grid100 dem30 --backend openmp --threads "$threads" --reps 3 --out "$out" > /dev/null
"$b" "$fx" ratio grid100 dem30 --den dem60 --backend serial --reps 3 --out "$out" > /dev/null
python3 - "$out" <<'EOF'
import csv, statistics, collections, sys
d = collections.defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])):
    d[(r["op"], r["rung"], int(r["threads"]))].append(float(r["seconds"]))
for k in sorted(d):
    print(f"{k[0]:9s} {k[1]:17s} x{k[2]:<3d} median {statistics.median(d[k]) * 1000:8.2f} ms")
EOF
rm -f "$out"
