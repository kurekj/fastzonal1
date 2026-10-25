// Verbatim extract of the author's SpongeGIS code, SPONGEGIS2026.NET commit 8028b95f (2026-09-26),
// relicensed MIT by the author. Source: WebSPONGEGIS/Services/IndicatorCalculationService.cs,
// private static method ComputeMeanRasterValueBatch. The body is unchanged.
using NetTopologySuite.Geometries;

namespace FzBench.Baseline;

public static partial class Reference
{
    /// <summary>
    /// Computes mean raster value per polygon using a pre-computed polygon mask.
    /// Single pass over the raster — O(height * width) instead of O(polygons * pixels).
    /// When <paramref name="polygons"/> is provided, any polygon that didn't receive
    /// a single pixel via the mask falls back to sampling the raster at the polygon's
    /// centroid. This handles the case where the raster is coarser than the SPU grid
    /// (e.g. CWB at 3617 m pixels vs SPU at 500 m): the mask assigns each big pixel
    /// to ONE polygon, leaving every other SPU under that pixel with count=0.
    /// </summary>
    public static double[] ComputeMeanRasterValueBatch(
        double[,] pixels, int[,] polyMask, int polyCount,
        List<Geometry>? polygons = null, GeoTransform? transform = null)
    {
        int height = pixels.GetLength(0);
        int width = pixels.GetLength(1);
        var sums = new double[polyCount];
        var counts = new int[polyCount];

        for (int r = 0; r < height; r++)
        {
            for (int c = 0; c < width; c++)
            {
                int polyId = polyMask[r, c];
                if (polyId < 0) continue;

                double val = pixels[r, c];
                if (double.IsNaN(val) || val == -9999) continue;

                sums[polyId] += val;
                counts[polyId]++;
            }
        }

        var result = new double[polyCount];
        for (int i = 0; i < polyCount; i++)
        {
            if (counts[i] > 0)
            {
                result[i] = sums[i] / counts[i];
                continue;
            }
            // Fallback: polygon smaller than one pixel — sample at centroid.
            if (polygons == null || transform == null) continue;
            var t = transform.Value;
            var centroid = polygons[i].Centroid;
            int col = t.WorldToCol(centroid.X);
            int row = t.WorldToRow(centroid.Y);
            if (col < 0 || col >= width || row < 0 || row >= height) continue;
            double cv = pixels[row, col];
            if (double.IsNaN(cv) || cv == -9999) continue;
            result[i] = cv;
        }

        return result;
    }
}
