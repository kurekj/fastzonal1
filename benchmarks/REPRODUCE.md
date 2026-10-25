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

Either the pinned conda environment (`conda-lock install -n fastzonal conda-lock.yml`, then
`conda activate fastzonal`) or the Docker image:

```bash
docker build -t fastzonal:bench .
```

The image builds the library with OpenMP, runs the C++, .NET and Python test suites, and has
`benchmarks/run_all.sh` as its entry point. CUDA is not part of the image.

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

The runner is resumable (points already in the raw CSV are skipped), writes
`benchmarks/results/raw_<stamp>.csv` and `env_<stamp>.json`, and stops with an error when a
rung's checksum differs from the baseline's for the same workload. On a machine with fewer
cores, pass `--threads "1 2 4 8"` with the counts available.

## 5. Make the tables and figures

```bash
python benchmarks/make_paper_assets.py --raw benchmarks/results/raw_<stamp>.csv --out paper_assets
```

`paper_assets/summary.json` holds every number (median, min, max per point; speedups; checksum
comparison); `tables/*.tex` are the fragments the manuscript inputs; `figures/*.png` the plots.

Expected wall time of the full matrix on the reference machine (2x Xeon Gold 5420+, 112
threads): about ten minutes. The reference raw files are committed under `benchmarks/results/`.
