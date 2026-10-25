#!/usr/bin/env bash
# Runs the whole benchmark matrix and writes benchmarks/results/raw_<stamp>.csv, env_<stamp>.json
# and status.json. Resumable: a point whose repetitions 0..reps-1 are all present in the raw CSV
# is skipped. Intended to run under tmux on the benchmark machine:
#
#   bash benchmarks/run_all.sh [--quick] [--fixtures DIR] [--stamp STAMP] [--threads "1 2 4 8"]
#                              [--band-rows B] [--reps R]
#
# --quick: one repetition, biebrza_upper only (a smoke test of the whole pipeline).
# The library is built Release with FASTZONAL_NATIVE_ARCH=ON into build/bench; the C# harness is
# published Release. Every measurement line carries the checksum, and the runner stops with an
# error when a rung's checksum differs from the baseline's for the same workload. The band size
# (default 16 rows) is passed to every rung that has one and recorded in each CSV line.
set -euo pipefail
# Thread placement: OpenMP workers bind to cores in order, and every run with at most one socket's
# worth of threads is pinned to NUMA node 0 (memory too) so that the serial rungs, the C# rungs and
# the small thread counts are not scattered across sockets. Larger thread counts use both sockets.
export OMP_PROC_BIND=close OMP_PLACES=cores
node0_cpus=$( (lscpu -p=CPU,NODE 2>/dev/null | grep -v '^#' | awk -F, '$2==0' | wc -l) || echo 0 )
pin() {  # pin <threads> <command...>
    local t="$1"; shift
    if command -v numactl >/dev/null 2>&1 && [ "$node0_cpus" -gt 0 ] && [ "$t" -le "$node0_cpus" ]; then
        numactl --cpunodebind=0 --membind=0 "$@"
    else
        "$@"
    fi
}
repo="$(cd "$(dirname "$0")/.." && pwd)"
results="$repo/benchmarks/results"
fixtures="$repo/benchmarks/fixtures"
quick=0
stamp="$(date +%Y%m%d_%H%M%S)"
threads_list="1 2 4 8 16 32 56 112"
reps=5
band_rows=16
while [ $# -gt 0 ]; do
    case "$1" in
        --quick) quick=1; reps=1 ;;
        --fixtures) fixtures="$2"; shift ;;
        --stamp) stamp="$2"; shift ;;
        --threads) threads_list="$2"; shift ;;
        --band-rows) band_rows="$2"; shift ;;
        --reps) reps="$2"; shift ;;
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

# The environment record is written once per stamp; resuming a stamp must not rewrite it (the
# measurements already in the raw file were taken with that build), so a resumed run refuses a
# different commit or settings instead.
python3 - "$env_json" "$repo" "$fixtures" "$band_rows" "$reps" "$threads_list" "$quick" <<'EOF'
import hashlib, json, os, platform, subprocess, sys
out, repo, fixtures, band_rows, reps, threads, quick = sys.argv[1:8]
def sh(cmd):
    try: return subprocess.check_output(cmd, shell=True, text=True, stderr=subprocess.DEVNULL).strip()
    except Exception: return ""
cpu = ""
for line in open("/proc/cpuinfo"):
    if line.startswith("model name"): cpu = line.split(":", 1)[1].strip(); break
cache = {}
for line in open(os.path.join(repo, "build/bench/CMakeCache.txt")):
    for key in ("CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS_RELEASE:", "FASTZONAL_NATIVE_ARCH:", "CMAKE_BUILD_TYPE:"):
        if line.startswith(key): cache[key.rstrip(":")] = line.split("=", 1)[1].strip()
manifest = os.path.join(fixtures, "manifest.json")
record = {
    "stamp": os.path.basename(out)[4:-5], "hostname": platform.node(), "os": platform.platform(), "cpu": cpu,
    "cores": os.cpu_count(),
    "mem_kb": next((int(l.split()[1]) for l in open("/proc/meminfo") if l.startswith("MemTotal")), None),
    "compiler": sh(f"{cache.get('CMAKE_CXX_COMPILER', 'g++')} --version | head -1"), "cmake": sh("cmake --version | head -1"),
    "dotnet": sh("dotnet --version"), "git_commit": sh(f"git -C '{repo}' rev-parse HEAD") or os.environ.get("FZ_GIT_COMMIT", ""),
    "git_dirty": bool(sh(f"git -C '{repo}' status --porcelain --untracked-files=no")),
    "cpu_governor": (open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").read().strip()
                     if os.path.exists("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") else None),
    "numa_node0_cpus": sh("lscpu -p=CPU,NODE | grep -v '^#' | awk -F, '$2==0' | wc -l"),
    "omp_proc_bind": os.environ.get("OMP_PROC_BIND"), "omp_places": os.environ.get("OMP_PLACES"),
    "omp_thread_limit": os.environ.get("OMP_THREAD_LIMIT"),
    "fz_version": sh(f"grep -m1 'project(fastzonal VERSION' '{repo}/CMakeLists.txt'").split("VERSION")[-1].split()[0] if True else "",
    "cmake_cache": cache, "extra_flags": "-ffp-contract=off (always), OpenMP",
    "band_rows": int(band_rows), "reps": int(reps), "threads": threads.split(), "quick": quick == "1",
    "fixtures_dir": fixtures,
    # hash of the fixture, expected and generated file tables only, so editing the archive entry of
    # the manifest later does not break the link between a run and its inputs
    "fixtures_manifest_sha256": hashlib.sha256(json.dumps({k: json.load(open(manifest)).get(k) for k in ("fixtures", "expected", "generated")}, sort_keys=True).encode()).hexdigest() if os.path.exists(manifest) else None,
    "omp_num_threads": os.environ.get("OMP_NUM_THREADS"),
}
if os.path.exists(out):
    old = json.load(open(out))
    for key in ("git_commit", "band_rows", "reps", "fixtures_manifest_sha256", "cpu", "hostname", "compiler", "dotnet",
                "cmake_cache", "omp_proc_bind", "omp_places", "omp_thread_limit"):
        if old.get(key) != record.get(key):
            sys.exit(f"cannot resume stamp {record['stamp']}: {key} changed ({old.get(key)!r} -> {record.get(key)!r})")
    sys.exit(0)
json.dump(record, open(out, "w"), indent=2)
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
    # resolution axis: the same zones over the generalised terrain
    points+=("$fx|mean|grid250|dem60|")
    points+=("$fx|mean|grid250|dem120|")
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
measured_n=0
start=$(date +%s)

# 0 when repetitions 0..reps-1 of this exact point are all present (fields compared exactly)
have() {
    python3 - "$raw" "$reps" "$1" "$2" "$3" "$4" "$5" "$6" "$7" <<'EOF'
import csv, os, sys
raw, reps = sys.argv[1], int(sys.argv[2])
fx, op, rung, threads, zones, raster, den = sys.argv[3:10]
if not os.path.exists(raw): sys.exit(1)
seen = set()
for r in csv.DictReader(open(raw, newline="")):
    if (r.get("fixture"), r.get("op"), r.get("rung"), r.get("threads"), r.get("zones"), r.get("raster"), r.get("den")) \
            == (fx, op, rung, threads, zones, raster, den) and r.get("n_cells"):
        try: seen.add(int(r["rep"]))
        except ValueError: pass
sys.exit(0 if all(k in seen for k in range(reps)) else 1)
EOF
}

write_status() {
    local now; now=$(date +%s)
    local elapsed=$(( now - start ))
    local eta=0
    if [ "$measured_n" -gt 0 ]; then eta=$(( elapsed * (total - done_n) / measured_n )); fi
    printf '{"done": %d, "total": %d, "measured": %d, "elapsed_s": %d, "eta_s": %d, "current": "%s", "raw": "%s", "updated": "%s"}\n' \
        "$done_n" "$total" "$measured_n" "$elapsed" "$eta" "$1" "$raw" "$(date -Is)" > "$status.tmp"
    mv -f "$status.tmp" "$status"
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
                pin 1 "$fz_bench" "$fxdir" "$op" "$zones" "$raster" ${den:+--den "$den"} --backend serial --band-rows "$band_rows" --reps "$reps" --out "$raw" > /dev/null ;;
            fastzonal-openmp)
                pin "$threads" "$fz_bench" "$fxdir" "$op" "$zones" "$raster" ${den:+--den "$den"} --backend openmp --threads "$threads" --band-rows "$band_rows" --reps "$reps" --out "$raw" > /dev/null ;;
            *)
                pin "$threads" dotnet "$fzbench_dll" bench "$fxdir" "$rung" "$op" "$zones" "$raster" ${den:+--den "$den"} --threads "$threads" --band-rows "$band_rows" --reps "$reps" --out "$raw" > /dev/null ;;
        esac
        done_n=$((done_n + 1)); measured_n=$((measured_n + 1))
        write_status "$label"
    done
    # checksum guard: every repetition of every rung of this workload must agree with the
    # baseline (exactly for the integer results of rasterize and majority, relative 1e-6 for the
    # float32 paths of mean and ratio; non-finite values never pass)
    python3 - "$raw" "$fx" "$op" "$zones" "$raster" "$den" <<'EOF'
import csv, math, sys
raw, fx, op, zones, raster, den = sys.argv[1:7]
rows = [r for r in csv.DictReader(open(raw, newline="")) if (r["fixture"], r["op"], r["zones"], r["raster"], r["den"]) == (fx, op, zones, raster, den)]
base = [float(r["checksum"]) for r in rows if r["rung"] == "csharp-baseline"]
if not base:
    print(f"no baseline for {fx} {op} {zones} {raster} {den}", file=sys.stderr); sys.exit(3)
b = base[0]
if not math.isfinite(b):
    print(f"non-finite baseline checksum for {fx} {op} {zones} {raster} {den}", file=sys.stderr); sys.exit(3)
tol = 0.0 if op in ("rasterize", "majority") else 1e-6 * max(1.0, abs(b))
bad = [(r["rung"], r["threads"], r["rep"], r["checksum"]) for r in rows
       if not math.isfinite(float(r["checksum"])) or abs(float(r["checksum"]) - b) > tol]
if bad:
    print(f"checksum mismatch for {fx} {op} {zones} {raster} {den}: baseline {b!r}, offenders {bad}", file=sys.stderr)
    sys.exit(3)
EOF
done
write_status "finished"
echo "== done: $raw"
