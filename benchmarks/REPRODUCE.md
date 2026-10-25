# Reproducing the benchmark tables

Everything in the manuscript's tables and figures comes out of these steps. Absolute times depend
on the machine; checksums and the shape of the speedup curves should not. The reproduction
platform is Linux x86-64.

Host prerequisites for every route: `git`, `python3`, `curl`, and `tar` with zstd support (or the
`zstd` binary). Docker users also need Docker; conda users need conda or mamba.

## 1. Get the code and the fixtures

```bash
git clone https://github.com/kurekj/fastzonal1.git && cd fastzonal1
git checkout v0.1.0                        # the revision the published numbers come from
bash benchmarks/fetch_fixtures.sh          # downloads the archive named in benchmarks/fixtures/manifest.json
```

`fetch_fixtures.sh` verifies every file against the SHA-256 values in the manifest: the fixtures,
the C# baseline's expected outputs (`benchmarks/fixtures/expected/<fixture>/`) and the generated
fixture. With a local copy of the archive (for example before the deposit is public):
`FZ_FIXTURE_ARCHIVE=/path/fixtures-v1.tar.zst bash benchmarks/fetch_fixtures.sh`.

## 2a. Docker route (one sequence, nothing else on the host)

```bash
docker build --build-arg GIT_COMMIT=$(git rev-parse HEAD) -t fastzonal:bench .
STAMP=mine
docker run --rm -v "$PWD/benchmarks/fixtures:/work/benchmarks/fixtures" \
    -v "$PWD/benchmarks/results:/work/benchmarks/results" fastzonal:bench --quick --stamp "$STAMP-quick"
docker run --rm -v "$PWD/benchmarks/fixtures:/work/benchmarks/fixtures" \
    -v "$PWD/benchmarks/results:/work/benchmarks/results" fastzonal:bench --stamp "$STAMP"
docker run --rm -v "$PWD/benchmarks/results:/work/benchmarks/results" -v "$PWD/my_assets:/work/my_assets" \
    --entrypoint python fastzonal:bench benchmarks/make_paper_assets.py \
    --raw "benchmarks/results/raw_$STAMP.csv" --out my_assets
```

The image build runs every test suite (C++, .NET, Python). The parity check against the C#
baseline's outputs on the full fixture set:

```bash
docker run --rm -v "$PWD/benchmarks/fixtures:/work/benchmarks/fixtures" --entrypoint bash fastzonal:bench -c \
    'FZ_PARITY_FIXTURE_DIR=benchmarks/fixtures FZ_PARITY_EXPECTED_DIR=benchmarks/fixtures/expected build/release/tests/fz_parity'
```

Pinning (`numactl`) is not available inside the image unless the host passes `--privileged`; the
runner then runs unpinned and records that in the environment file.

## 2b. conda route

```bash
conda-lock install -n fastzonal conda-lock.yml && conda activate fastzonal
# the .NET 10 SDK is not a conda package: install it from Microsoft's package feed or with
# dotnet-install.sh --channel 10.0, and make sure `dotnet` is on PATH
cmake --preset release && cmake --build --preset release && ctest --preset release
FZ_PARITY_FIXTURE_DIR=benchmarks/fixtures FZ_PARITY_EXPECTED_DIR=benchmarks/fixtures/expected build/release/tests/fz_parity
STAMP=mine
bash benchmarks/run_all.sh --quick --stamp "$STAMP-quick"   # one repetition, biebrza_upper only, about one minute
bash benchmarks/run_all.sh --stamp "$STAMP"                 # the published matrix: five repetitions, both fixtures
bash benchmarks/status.sh                                   # progress, elapsed, ETA (from another shell)
python benchmarks/make_paper_assets.py --raw "benchmarks/results/raw_$STAMP.csv" --out my_assets
```

## What the runner does

It builds the library Release with `FASTZONAL_NATIVE_ARCH=ON`, publishes the C# harness, pins
threads (`OMP_PROC_BIND=close OMP_PLACES=cores`, and `numactl --cpunodebind=0 --membind=0` for
every invocation with at most one socket's worth of threads), runs the matrix with one untimed
warm-up and five measured repetitions per point, writes `benchmarks/results/raw_<stamp>.csv`,
`env_<stamp>.json` (machine, compiler, commit, build flags, governor, OpenMP environment, fixture
manifest hash, settings) and `status.json`, and stops with an error when a rung's checksum differs
from the baseline's for the same workload (exactly for rasterize and majority, relative 1e-6 for
the float32 mean and ratio paths). It is resumable with the same `--stamp`; a resumed stamp
refuses a different commit, machine, toolchain or settings. On a machine with fewer cores pass
`--threads "1 2 4 8"` with the counts available. The band size is 16 rows (`--band-rows`) and is
recorded in every CSV line because it fixes the summation order.

## Reading the assets

`my_assets/summary.json` holds every number (median, min, max per point; speedups; checksum
comparison of every repetition; workloads without a baseline; variability warnings where the
serial backend and the OpenMP backend at one thread, which share one code path, differ by more
than 15%); `tables/*.tex` are the fragments the manuscript inputs; `figures/*.png` the plots. Name
your own raw file explicitly: the script refuses to pool raw files from different commits or
machines (such as the committed reference run next to yours) unless `--allow-mixed` is given, and
then records that in the summary.

Expected wall time of the full matrix on the reference machine (2x Xeon Gold 5420+, 112
threads): about ten minutes. The reference raw and environment files are committed under
`benchmarks/results/` and the assets built from them under `paper_assets/`; the commit that
produced them is recorded in the environment file.
