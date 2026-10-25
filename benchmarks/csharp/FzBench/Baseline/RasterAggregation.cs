// Verbatim extract of the author's SpongeGIS code, SPONGEGIS2026.NET commit 8028b95f (2026-09-26),
// relicensed MIT by the author. Source: WebSPONGEGIS/Services/RasterAggregation.cs, the method
// PerSpuMajority (PerSpuRatio is not used by the live application and is omitted). The body is unchanged.
namespace FzBench.Baseline;

public static partial class Reference
{
    /// <summary>
    /// Majority (mode) of the raster values inside each SPU — Ignacy's "oneRasterToSPUmjrt"
    /// (docx 2026-09-12): for class rasters such as BFI or GRR the SPU should carry the dominant
    /// class, not the arithmetic mean of class codes. NoData (NaN, -9999) pixels are ignored; ties
    /// resolve to the smallest value so the result is deterministic. An SPU with no pixel centre
    /// inside it (smaller than one cell) falls back to the value at its centroid, like the mean.
    /// polyMask[r,c] = SPU index or -1, same layout as the mean aggregation.
    /// </summary>
    public static double[] PerSpuMajority(
        double[,] pixels, int[,] polyMask, int polyCount,
        Func<int, (int Row, int Col)?>? centroidCell = null)
    {
        int height = pixels.GetLength(0);
        int width = pixels.GetLength(1);
        var counts = new Dictionary<double, int>[polyCount];

        for (int r = 0; r < height; r++)
        {
            for (int c = 0; c < width; c++)
            {
                int polyId = polyMask[r, c];
                if (polyId < 0) continue;
                double val = pixels[r, c];
                if (double.IsNaN(val) || val == -9999) continue;
                var d = counts[polyId] ??= new Dictionary<double, int>();
                d[val] = d.TryGetValue(val, out var n) ? n + 1 : 1;
            }
        }

        var result = new double[polyCount];
        for (int i = 0; i < polyCount; i++)
        {
            var d = counts[i];
            if (d != null && d.Count > 0)
            {
                int best = -1; double bestVal = 0;
                foreach (var (val, n) in d)
                {
                    if (n > best || (n == best && val < bestVal)) { best = n; bestVal = val; }
                }
                result[i] = bestVal;
                continue;
            }
            var cell = centroidCell?.Invoke(i);
            if (cell == null) continue;
            var (row, col) = cell.Value;
            if (col < 0 || col >= width || row < 0 || row >= height) continue;
            double cv = pixels[row, col];
            if (double.IsNaN(cv) || cv == -9999) continue;
            result[i] = cv;
        }
        return result;
    }
}
