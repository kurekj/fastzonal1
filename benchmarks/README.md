# Benchmarks

Everything published about fastzonal's speed comes out of this directory.

- `fixtures/FORMAT.md`: the raw fixture layout shared by every harness.
- `fzfix/`: Python I/O for that layout.
- `prepare_fixture.py`: GeoTIFF + polygon layer -> fixture.
- `make_synthetic.py`: download-free synthetic fixture (used by the tests).
- `tests/`: pytest for the scripts (`python -m pytest benchmarks/tests -q`).

Python requirements for the scripts: numpy, rasterio, pyogrio, shapely, pyproj, pytest
(the repository's `environment.yml` pins a complete conda environment).
