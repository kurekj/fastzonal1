# Expected outputs of the C# baseline

One CSV per (operation, zone set, raster) of a fixture, written by
`FzBench expected <fixtureDir> <outDir>` (benchmarks/csharp). Each file has a header `zone,value`
and one line per zone with the value printed in round-trip (`R`) format:

- `rasterize_<zones>_<raster>.csv`: number of pixels assigned to the zone by the reference rasteriser
- `mean_<zones>_<raster>.csv`: mean with the centroid fallback
- `majority_<zones>_<raster>.csv`: mode (ties to the smallest value) with the centroid fallback
- `ratio_<zones>_<num>_<den>.csv`: mean of the cell-wise ratio on the numerator grid

The committed fixtures are `synthetic_small` and `leze_micro`. The expected outputs of the larger
fixtures are part of the published fixture archive (`expected/<fixture>/` inside it) and are
listed with hashes under `expected` in benchmarks/fixtures/manifest.json.
