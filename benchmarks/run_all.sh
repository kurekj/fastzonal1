#!/usr/bin/env bash
# Runs the whole benchmark matrix and writes benchmarks/results/raw_<stamp>.csv, env_<stamp>.json
# and status.json. Resumable: a point already present in the raw CSV is skipped. Intended to run
# under tmux on the benchmark machine:
#
#   bash benchmarks/run_all.sh [--quick] [--fixtures DIR] [--stamp STAMP] [--threads "1 2 4 8"]
#
# --quick: one repetition, biebrza_upper only (a smoke test of the whole pipeline).
# The library is built Release with FASTZONAL_NATIVE_ARCH=ON into build/bench; the C# harness is
# published Release. Every measurement line carries the checksum, and the runner stops with an
# error when a rung's checksum differs from the baseline's for the same workload.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
results="$repo/benchmarks/results"
fixtures="$repo/benchmarks/fixtures"
quick=0
stamp="$(date +%Y%m%d_%H%M%S)"
threads_list="1 2 4 8 16 32 56 112"
reps=5
while [ $# -gt 0 ]; do
    case "$1" in
        --quick) quick=1; reps=1 ;;
        --fixtures) fixtures="$2"; shift ;;
        --stamp) stamp="$2"; shift ;;
        --threads) threads_list="$2"; shift ;;
        *) echo "unknown option $1" >&2; exit 1 ;;
    esac
    shift
done
mkdir -p "$results"
raw="$results/raw_$stamp.csv"
status="$results/status.json"
env_json="$results/env_$stamp.json"

echo "== building (Release, native arch)"
cmake -S "$repo" -B "$repo/build/bench" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTZONAL_NATIVE_ARCH=ON \
      -DFASTZONAL_BUILD_TESTS=OFF > "$results/build_$stamp.log" 2>&1
cmake --build "$repo/build/bench" >> "$results/build_$stamp.log" 2>&1
fz_bench="$repo/build/bench/fz_bench"
native_dir="$repo/bindings/dotnet/FastZonal/runtimes/linux-x64/native"
mkdir -p "$native_dir" && cp "$repo/build/bench/libfastzonal.so" "$native_dir/"
dotnet publish "$repo/benchmarks/csharp/FzBench/FzBench.csproj" -c Release -o "$repo/build/bench/FzBench" \
      >> "$results/build_$stamp.log" 2>&1
fzbench_dll="$repo/build/bench/FzBench/FzBench.dll"

# environment record
python3 - "$env_json" "$repo" "$fz_bench" <<'EOF'
import json, os, platform, subprocess, sys
out, repo, fz = sys.argv[1:4]
def sh(cmd):
    try: return subprocess.check_output(cmd, shell=True, text=True, stderr=subprocess.DEVNULL).strip()
    except Exception: return ""
cpu = ""
for line in open("/proc/cpuinfo"):
    if line.startswith("model name"): cpu = line.split(":", 1)[1].strip(); break
json.dump({
    "hostname": platform.node(), "os": platform.platform(), "cpu": cpu, "cores": os.cpu_count(),
    "mem_kb": next((int(l.split()[1]) for l in open("/proc/meminfo") if l.startswith("MemTotal")), None),
    "compiler": sh("g++ --version | head -1"), "cmake": sh("cmake --version | head -1"),
    "dotnet": sh("dotnet --version"), "git_commit": sh(f"git -C '{repo}' rev-parse HEAD"),
    "git_dirty": bool(sh(f"git -C '{repo}' status --porcelain")),
    "flags": "Release -O3 -march=native -ffp-contract=off OpenMP", "omp_num_threads": os.environ.get("OMP_NUM_THREADS"),
}, open(out, "w"), indent=2)
EOF

# ---- matrix ----------------------------------------------------------------------------------
# fixture|op|zones|raster|den
points=()
add_fixture() {  # fixture, grids...
    local fx="$1"; shift
    for g in "$@"; do
        points+=("$fx|rasterize|$g|dem30|")
        points+=("$fx|mean|$g|dem30|")
        points+=("$fx|majority|$g|clc100|")
        points+=("$fx|ratio|$g|dem30|dem60")
    done
}
if [ "$quick" = 1 ]; then
    add_fixture biebrza_upper grid500 grid100
else
    add_fixture biebrza_upper grid1000 grid500 grid250 grid100
    add_fixture mazovia_5000 grid1000 grid500 grid250 grid100
fi

# rung|threads
rungs=("csharp-baseline|1" "fastzonal-serial|1")
for t in $threads_list; do rungs+=("csharp-optimised|$t" "fastzonal-openmp|$t"); done

total=$(( ${#points[@]} * ${#rungs[@]} ))
done_n=0
start=$(date +%s)
touch "$raw"

have() {  # fixture op rung threads zones raster den -> 0 when every rep is present
    local want="$1,$2,$3,$4,"; local pat; pat=$(printf '%s' "$want" | sed 's/[.[\*^$]/\\&/g')
    local n; n=$(grep -c "^$pat[0-9]*,[^,]*,[^,]*,$5,$6,$7," "$raw" 2>/dev/null || true)
    [ "$n" -ge "$reps" ]
}

write_status() {
    local now; now=$(date +%s)
    local elapsed=$(( now - start ))
    local eta=0
    if [ "$done_n" -gt 0 ]; then eta=$(( elapsed * (total - done_n) / done_n )); fi
    printf '{"done": %d, "total": %d, "elapsed_s": %d, "eta_s": %d, "current": "%s", "raw": "%s", "updated": "%s"}\n' \
        "$done_n" "$total" "$elapsed" "$eta" "$1" "$raw" "$(date -Is)" > "$status"
}

for p in "${points[@]}"; do
    IFS='|' read -r fx op zones raster den <<< "$p"
    fxdir="$fixtures/$fx"
    for r in "${rungs[@]}"; do
        IFS='|' read -r rung threads <<< "$r"
        label="$fx $op $zones $raster${den:+/$den} $rung x$threads"
        write_status "$label"
        if have "$fx" "$op" "$rung" "$threads" "$zones" "$raster" "$den"; then
            done_n=$((done_n + 1)); continue
        fi
        case "$rung" in
            fastzonal-serial)
                "$fz_bench" "$fxdir" "$op" "$zones" "$raster" ${den:+--den "$den"} --backend serial --reps "$reps" --out "$raw" > /dev/null ;;
            fastzonal-openmp)
                "$fz_bench" "$fxdir" "$op" "$zones" "$raster" ${den:+--den "$den"} --backend openmp --threads "$threads" --reps "$reps" --out "$raw" > /dev/null ;;
            *)
                dotnet "$fzbench_dll" bench "$fxdir" "$rung" "$op" "$zones" "$raster" ${den:+--den "$den"} --threads "$threads" --reps "$reps" --out "$raw" > /dev/null ;;
        esac
        done_n=$((done_n + 1))
        write_status "$label"
    done
    # checksum guard: every rung of this workload must agree with the baseline (relative 1e-6)
    python3 - "$raw" "$fx" "$op" "$zones" "$raster" "$den" <<'EOF'
import csv, sys
raw, fx, op, zones, raster, den = sys.argv[1:7]
rows = [r for r in csv.DictReader(open(raw, newline="")) if (r["fixture"], r["op"], r["zones"], r["raster"], r["den"]) == (fx, op, zones, raster, den)]
base = [float(r["checksum"]) for r in rows if r["rung"] == "csharp-baseline"]
if not base: sys.exit(0)
b = base[0]
bad = [(r["rung"], r["threads"], r["checksum"]) for r in rows if abs(float(r["checksum"]) - b) > 1e-6 * max(1.0, abs(b))]
if bad:
    print(f"checksum mismatch for {fx} {op} {zones} {raster} {den}: baseline {b!r}, offenders {bad}", file=sys.stderr)
    sys.exit(3)
EOF
done
write_status "finished"
echo "== done: $raw"
