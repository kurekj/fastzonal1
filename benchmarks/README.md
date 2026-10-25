# Benchmarks

Everything published about fastzonal's speed comes out of this directory.

- `fixtures/FORMAT.md`: the raw fixture layout shared by every harness.
- `fzfix/`: Python I/O for that layout.
- `prepare_fixture.py`: GeoTIFF + polygon layer -> fixture.
- `make_synthetic.py`: download-free synthetic fixture (used by the tests).
- `tests/`: pytest for the scripts (`python -m pytest benchmarks/tests -q`).

- `maintainer/build_fixtures.py`: rebuilds the public-data fixtures. It needs the SpongeGIS PostGIS
  database (open pan-European layers loaded there) and network access for HydroBASINS, so it is
  the maintainers' route; reviewers use `fetch_fixtures.sh`, which downloads the published
  archive and verifies every file against `fixtures/manifest.json`.
- `csharp/`: the C# harness (`FzBench expected` writes the baseline's outputs,
  `FzBench bench` times one rung and appends a CSV line).

Python requirements for the scripts: numpy, rasterio, pyogrio, shapely, pyproj, pytest
(the repository's `environment.yml` pins a complete conda environment).
