// Verbatim extract of the author's SpongeGIS code, SPONGEGIS2026.NET commit 8028b95f (2026-09-26),
// relicensed MIT by the author. Source: WebSPONGEGIS/Services/IndicatorCalculationService.cs,
// private static methods RasterizePolygons, CollectPolygons and RasterizeOnePolygon. The method
// bodies are unchanged; they were moved into this static class and made public.
using NetTopologySuite.Geometries;

namespace FzBench.Baseline;

public static partial class Reference
{
    /// <summary>
    /// Rasterizes SPU polygons into an integer mask matching the given raster grid.
    /// Each pixel gets the index (0..N-1) of its containing polygon, or -1 if outside all polygons.
    /// </summary>
    public static int[,] RasterizePolygons(
        List<Geometry> spuPolygons, GeoTransform transform, int height, int width)
    {
        var mask = new int[height, width];
        // Initialize all pixels to -1 (no polygon)
        for (int r = 0; r < height; r++)
            for (int c = 0; c < width; c++)
                mask[r, c] = -1;

        for (int polyIdx = 0; polyIdx < spuPolygons.Count; polyIdx++)
        {
            var spu = spuPolygons[polyIdx];

            // Extract all polygon rings (handles Polygon, MultiPolygon, GeometryCollection)
            var polygons = new List<Polygon>();
            CollectPolygons(spu, polygons);

            foreach (var polygon in polygons)
                RasterizeOnePolygon(polygon.ExteriorRing.Coordinates, polyIdx, mask, transform, height, width);
        }

        return mask;
    }

    /// <summary>
    /// Recursively collects all Polygon geometries from any geometry type.
    /// </summary>
    private static void CollectPolygons(Geometry geom, List<Polygon> result)
    {
        if (geom is Polygon p)
        {
            result.Add(p);
        }
        else if (geom is GeometryCollection gc)
        {
            for (int i = 0; i < gc.NumGeometries; i++)
                CollectPolygons(gc.GetGeometryN(i), result);
        }
    }

    /// <summary>
    /// Rasterizes a single polygon ring using scanline fill.
    /// </summary>
    private static void RasterizeOnePolygon(
        Coordinate[] ring, int polyIdx, int[,] mask,
        GeoTransform transform, int height, int width)
    {
        // Compute bbox in pixel coords from ring coordinates
        double minX = double.MaxValue, maxX = double.MinValue;
        double minY = double.MaxValue, maxY = double.MinValue;
        for (int i = 0; i < ring.Length; i++)
        {
            if (ring[i].X < minX) minX = ring[i].X;
            if (ring[i].X > maxX) maxX = ring[i].X;
            if (ring[i].Y < minY) minY = ring[i].Y;
            if (ring[i].Y > maxY) maxY = ring[i].Y;
        }

        int startCol = Math.Max(0, transform.WorldToCol(minX));
        int endCol = Math.Min(width - 1, transform.WorldToCol(maxX));
        int startRow = Math.Max(0, transform.WorldToRow(maxY));
        int endRow = Math.Min(height - 1, transform.WorldToRow(minY));

        // Scanline rasterization per row
        for (int row = startRow; row <= endRow; row++)
        {
            double wy = transform.OriginY - (row + 0.5) * transform.ScaleY;
            var xIntersections = new List<double>();

            for (int i = 0; i < ring.Length - 1; i++)
            {
                double y1 = ring[i].Y, y2 = ring[i + 1].Y;
                if ((y1 <= wy && y2 > wy) || (y2 <= wy && y1 > wy))
                {
                    double x1 = ring[i].X, x2 = ring[i + 1].X;
                    double xInt = x1 + (wy - y1) / (y2 - y1) * (x2 - x1);
                    xIntersections.Add(xInt);
                }
            }

            xIntersections.Sort();

            for (int k = 0; k + 1 < xIntersections.Count; k += 2)
            {
                int cStart = Math.Max(startCol, transform.WorldToCol(xIntersections[k]));
                int cEnd = Math.Min(endCol, transform.WorldToCol(xIntersections[k + 1]));
                for (int col = cStart; col <= cEnd; col++)
                    mask[row, col] = polyIdx;
            }
        }
    }
}
