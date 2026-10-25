# Reproducing the benchmark tables

Everything in the manuscript's tables and figures comes out of these steps. Absolute times depend
on the machine; checksums and the shape of the speedup curves should not.

## 1. Get the code and the fixtures

```bash
git clone https://github.com/kurekj/fastzonal1.git && cd fastzonal1
bash benchmarks/fetch_fixtures.sh          # downloads the archive named in benchmarks/fixtures/manifest.json
```

`fetch_fixtures.sh` verifies every file against the SHA-256 values in the manifest. The archive
also contains `expected/<fixture>/` (the C# baseline's outputs) for the parity test. With a local
copy of the archive: `FZ_FIXTURE_ARCHIVE=/path/fixtures-v1.tar.zst bash benchmarks/fetch_fixtures.sh`.

## 2. Build the environment

The reproduction platform is Linux x86-64. Two routes:

- **conda**: `conda-lock install -n fastzonal conda-lock.yml && conda activate fastzonal` gives
  the pinned C++ toolchain (GCC 13), CMake, Ninja, GDAL and every Python package. The .NET 10 SDK
  is not a conda package: install it from Microsoft's package feed or with the `dotnet-install.sh`
  script (`--channel 10.0`), and make sure `dotnet` is on `PATH`.
- **Docker**: `docker build -t fastzonal:bench .` builds the image from the same lock file, adds
  the .NET SDK, builds the library with OpenMP and runs the C++, .NET and Python test suites.
  CUDA is not part of the image.

## 3. Check parity before measuring

```bash
cmake --preset release && cmake --build --preset release
FZ_PARITY_FIXTURE_DIR=benchmarks/fixtures FZ_PARITY_EXPECTED_DIR=benchmarks/fixtures/expected \
    build/release/tests/fz_parity
```

## 4. Run the matrix

```bash
bash benchmarks/run_all.sh --quick     # one repetition, biebrza_upper only, about one minute
bash benchmarks/run_all.sh             # the published matrix: five repetitions, both fixtures
bash benchmarks/status.sh              # progress, elapsed, ETA
```

The runner is resumable with `--stamp <stamp>` (points whose repetitions are all in the raw
CSV are skipped; a resumed stamp refuses a different commit, machine or settings), writes
`benchmarks/results/raw_<stamp>.csv` and `env_<stamp>.json`, and stops with an error when a
rung's checksum differs from the baseline's for the same workload. On a machine with fewer
cores, pass `--threads "1 2 4 8"` with the counts available. The band size is 16 rows
(`--band-rows`); it is recorded in every CSV line because it fixes the summation order.

With Docker, mount the fixtures and a results directory so the measurements survive the
container:

```bash
docker run --rm -v "$PWD/benchmarks/fixtures:/work/benchmarks/fixtures" \n    -v "$PWD/benchmarks/results:/work/benchmarks/results" fastzonal:bench --quick
docker run --rm -v "$PWD/benchmarks/fixtures:/work/benchmarks/fixtures" \n    -v "$PWD/benchmarks/results:/work/benchmarks/results" fastzonal:bench
# parity and the tables from inside the image:
docker run --rm -v "$PWD/benchmarks/fixtures:/work/benchmarks/fixtures" --entrypoint bash fastzonal:bench -c \n    'FZ_PARITY_FIXTURE_DIR=benchmarks/fixtures FZ_PARITY_EXPECTED_DIR=benchmarks/fixtures/expected build/release/tests/fz_parity'
docker run --rm -v "$PWD/benchmarks/results:/work/benchmarks/results" -v "$PWD/paper_assets:/work/paper_assets" \n    --entrypoint python fastzonal:bench benchmarks/make_paper_assets.py --raw 'benchmarks/results/raw_*.csv' --out paper_assets
```

## 5. Make the tables and figures

```bash
python benchmarks/make_paper_assets.py --raw benchmarks/results/raw_<stamp>.csv --out paper_assets
```

`paper_assets/summary.json` holds every number (median, min, max per point; speedups; checksum
comparison); `tables/*.tex` are the fragments the manuscript inputs; `figures/*.png` the plots.
Name the raw file of your own run explicitly: the script refuses to pool raw files from different
commits or machines (such as the committed reference run next to yours) unless `--allow-mixed` is
given, and then records that in the summary.

Expected wall time of the full matrix on the reference machine (2x Xeon Gold 5420+, 112
threads): about ten minutes. The reference raw and environment files are committed under
`benchmarks/results/` and the assets built from them under `paper_assets/`; the commit that
produced them is recorded in the environment file.
