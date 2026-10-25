#!/usr/bin/env bash
# Development aid: time the library rasteriser on one fixture, serial and a few thread counts.
#   bash tools/local_raster_bench.sh <fixtureDir> <zones> <raster> [threads...]
set -euo pipefail
fx="$1"; zones="$2"; raster="$3"; shift 3
repo="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp /tmp/raster_bench_XXXXXX.csv)"
b="$repo/build/release/fz_bench"
"$b" "$fx" rasterize "$zones" "$raster" --backend serial --reps 3 --out "$out" > /dev/null
for t in "${@:-2 4 8}"; do
    "$b" "$fx" rasterize "$zones" "$raster" --backend openmp --threads "$t" --reps 3 --out "$out" > /dev/null
done
python3 - "$out" <<'EOF'
import csv, statistics, collections, sys
d = collections.defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])):
    d[(r["rung"], int(r["threads"]))].append(float(r["seconds"]))
for k in sorted(d):
    print(f"{k[0]:17s} x{k[1]:<3d} median {statistics.median(d[k]) * 1000:8.1f} ms")
EOF
rm -f "$out"
