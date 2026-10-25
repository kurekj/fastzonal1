#!/usr/bin/env bash
# Runs the external-tool comparison (benchmarks/compare_tools.py) on the published fixture set and
# writes benchmarks/results/tools_<stamp>.csv and tools_env_<stamp>.json. Resumable: rerun with
# FZ_STAMP=<stamp> to continue an interrupted run.
#
#   FZ_FIXTURES=/data/fastzonal/fixtures bash benchmarks/run_tools.sh            # full set
#   FZ_FIXTURES=... FZ_QUICK=1 bash benchmarks/run_tools.sh                      # leze_micro only
#
# Environment: the conda environment of environment.yml / conda-lock.yml (rasterstats, exactextract,
# xarray-spatial, rasterio, shapely) and the release build of the library in build/release.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
fixtures="${FZ_FIXTURES:?set FZ_FIXTURES to the fixture root (the directory holding manifest.json)}"
python="${FZ_PYTHON:-python3}"
stamp="${FZ_STAMP:-$(date +%Y%m%d_%H%M%S)}"
threads="${FZ_THREADS:-1,8,56}"
reps="${FZ_REPS:-3}"
out="$repo/benchmarks/results/tools_$stamp.csv"

export FASTZONAL_LIBRARY="${FASTZONAL_LIBRARY:-$repo/build/release/libfastzonal.so}"
export OMP_PROC_BIND=close OMP_PLACES=cores
# PROJ looks for its database relative to the active environment; point it at pyproj's copy
export PROJ_DATA="${PROJ_DATA:-$("$python" -c 'import pyproj; print(pyproj.datadir.get_data_dir())' 2>/dev/null || true)}"

if [ "${FZ_QUICK:-0}" = "1" ]; then
    cases="leze_micro:grid250:dem30"
else
    cases="biebrza_upper:grid1000:dem30,biebrza_upper:grid500:dem30,biebrza_upper:grid250:dem30,biebrza_upper:grid100:dem30"
    cases="$cases,mazovia_5000:grid1000:dem30,mazovia_5000:grid500:dem30,mazovia_5000:grid250:dem30,mazovia_5000:grid100:dem30"
fi

# Pin to NUMA node 0 like run_all.sh when every requested thread count fits there.
node0_cpus=$( (lscpu -p=CPU,NODE 2>/dev/null | grep -v '^#' | awk -F, '$2==0' | wc -l) || echo 0 )
max_threads=$(echo "$threads" | tr ',' '\n' | sort -n | tail -1)
prefix=()
if command -v numactl >/dev/null 2>&1 && [ "$node0_cpus" -gt 0 ] && [ "$max_threads" -le "$node0_cpus" ]; then
    prefix=(numactl --cpunodebind=0 --membind=0)
    export FZ_NUMACTL="cpunodebind=0 membind=0"
fi

echo "stamp $stamp; cases: $cases; threads $threads; reps $reps"
"${prefix[@]}" "$python" "$repo/benchmarks/compare_tools.py" run --fixtures "$fixtures" --cases "$cases" \
    --threads "$threads" --reps "$reps" --out "$out" "$@"
