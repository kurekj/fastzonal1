using FzBench.Baseline;

namespace FzBench.Rungs;

/// <summary>The SpongeGIS code as deployed: single-threaded, double[,] rasters, int[,] masks.</summary>
public sealed class BaselineRung : IRung
{
    public string Name => "csharp-baseline";

    public int EffectiveThreads => 1;

    public object Rasterize(Fixture f, string zones, string raster)
    {
        var r = f.Rasters[raster];
        return Reference.RasterizePolygons(f.Zones[zones].Geometries, r.Transform, r.Height, r.Width);
    }

    public double[] Mean(Fixture f, string zones, string raster, object mask)
    {
        var r = f.Rasters[raster];
        var z = f.Zones[zones];
        return Reference.ComputeMeanRasterValueBatch(r.Pixels, (int[,])mask, z.ZoneCount, z.Geometries, r.Transform);
    }

    public double[] Majority(Fixture f, string zones, string raster, object mask)
    {
        var r = f.Rasters[raster];
        var z = f.Zones[zones];
        var transform = r.Transform;
        return Reference.PerSpuMajority(r.Pixels, (int[,])mask, z.ZoneCount, i =>
        {
            var centroid = z.Geometries[i].Centroid;
            return (transform.WorldToRow(centroid.Y), transform.WorldToCol(centroid.X));
        });
    }

    public double[] Ratio(Fixture f, string zones, string num, RasterData denOnGrid, object mask)
    {
        var n = f.Rasters[num];
        var z = f.Zones[zones];
        var ratio = RatioRaster(n, denOnGrid);
        return Reference.ComputeMeanRasterValueBatch(ratio, (int[,])mask, z.ZoneCount, z.Geometries, n.Transform);
    }

    /// <summary>Cell-wise num / den on one grid with the exclusion rules of the reference's
    /// RasterAlign (NoData in either raster or a denominator below 1e-12 in magnitude becomes NaN,
    /// which the mean skips). Forming this raster is part of the reference's work and is timed.</summary>
    public static double[,] RatioRaster(RasterData num, RasterData denOnGrid)
    {
        var d = denOnGrid.Pixels;
        var o = new double[num.Height, num.Width];
        for (int r = 0; r < num.Height; r++)
            for (int c = 0; c < num.Width; c++)
            {
                double a = num.Pixels[r, c], b = d[r, c];
                if (double.IsNaN(a) || a == -9999 || double.IsNaN(b) || b == -9999 || Math.Abs(b) < 1e-12)
                    o[r, c] = double.NaN;
                else
                    o[r, c] = a / b;
            }
        return o;
    }

    public int[] MaskToArray(object mask)
    {
        var m = (int[,])mask;
        int h = m.GetLength(0), w = m.GetLength(1);
        var flat = new int[h * w];
        Buffer.BlockCopy(m, 0, flat, 0, flat.Length * sizeof(int));
        return flat;
    }
}
