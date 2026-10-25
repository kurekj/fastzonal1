# Expected outputs of the C# baseline

One CSV per (operation, zone set, raster) of a fixture, written by
`FzBench expected <fixtureDir> <outDir>` (benchmarks/csharp). Each file has a header `zone,value`
and one line per zone with the value printed in round-trip (`R`) format:

- `rasterize_<zones>_<raster>.csv`: number of pixels assigned to the zone by the reference rasteriser
- `mean_<zones>_<raster>.csv`: mean with the centroid fallback
- `majority_<zones>_<raster>.csv`: mode (ties to the smallest value) with the centroid fallback
- `ratio_<zones>_<num>_<den>.csv`: mean of the cell-wise ratio on the numerator grid

The committed fixtures are `synthetic_small` and `leze_micro`; the expected outputs of the larger
fixtures live next to the fixtures themselves (see benchmarks/fixtures/manifest.json).
