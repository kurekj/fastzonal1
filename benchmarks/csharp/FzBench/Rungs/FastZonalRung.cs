using FastZonal;

namespace FzBench.Rungs;

/// <summary>The fastzonal library through its .NET binding, in COMPAT rasterisation mode so the
/// results match the reference. Centroid cells come from the fixture (the same truncating rule
/// as the reference, computed on shapely centroids).</summary>
public sealed class FastZonalRung : IRung
{
    private readonly Options _options;
    private readonly bool _serial;

    public FastZonalRung(bool serial, int threads)
    {
        _serial = serial;
        _options = new Options
        {
            Backend = serial ? Backend.Serial : Backend.OpenMp,
            Threads = serial ? 1 : threads,
            RasterMode = RasterMode.Compat
        };
    }

    public string Name => _serial ? "fastzonal-serial" : "fastzonal-openmp";

    public int EffectiveThreads => _serial ? 1 : (_options.Threads <= 0 ? Environment.ProcessorCount : _options.Threads);

    private static FastZonal.Transform Tr(RasterData r) =>
        new(r.Transform.OriginX, r.Transform.OriginY, r.Transform.ScaleX, r.Transform.ScaleY);

    private static (int[] Rows, int[] Cols) Centroids(ZoneData z, string raster) =>
        z.Centroids.TryGetValue(raster, out var c) ? c : (Array.Empty<int>(), Array.Empty<int>());

    public object Rasterize(Fixture f, string zones, string raster)
    {
        var r = f.Rasters[raster];
        var z = f.Zones[zones];
        return Rasterizer.Rasterize(z.Xy, z.RingStart, z.RingIsHole, z.RingZone, Tr(r), r.Width, r.Height, _options);
    }

    public double[] Mean(Fixture f, string zones, string raster, object mask)
    {
        var r = f.Rasters[raster];
        var z = f.Zones[zones];
        var stats = Zonal.Stats(r.Flat, (int[])mask, r.Width, r.Height, z.ZoneCount, -9999f, _options);
        var (rows, cols) = Centroids(z, raster);
        return Zonal.MeanWithFallback(r.Flat, r.Width, r.Height, -9999f, stats, rows, cols);
    }

    public double[] Majority(Fixture f, string zones, string raster, object mask)
    {
        var r = f.Rasters[raster];
        var z = f.Zones[zones];
        var (rows, cols) = Centroids(z, raster);
        return Zonal.Majority(r.Flat, (int[])mask, r.Width, r.Height, z.ZoneCount, -9999f, rows, cols, out _, _options);
    }

    public double[] Ratio(Fixture f, string zones, string num, RasterData denOnGrid, object mask)
    {
        var a = f.Rasters[num];
        var z = f.Zones[zones];
        var stats = Zonal.Ratio(a.Flat, denOnGrid.Flat, (int[])mask, a.Width, a.Height, z.ZoneCount, -9999f, -9999f,
            1e-12, _options);
        // mean of the ratio with the reference's centroid fallback (sampled on the ratio of the two
        // rasters at the centroid cell, with the same exclusion rules)
        var (rows, cols) = Centroids(z, num);
        var result = new double[z.ZoneCount];
        for (int zone = 0; zone < result.Length; zone++)
        {
            if (stats[zone].Count > 0) { result[zone] = stats[zone].Mean; continue; }
            if (rows.Length == 0) continue;
            int row = rows[zone], col = cols[zone];
            if (row < 0 || row >= a.Height || col < 0 || col >= a.Width) continue;
            long i = (long)row * a.Width + col;
            float x = a.Flat[i], y = denOnGrid.Flat[i];
            if (float.IsNaN(x) || x == -9999f || float.IsNaN(y) || y == -9999f || Math.Abs(y) < 1e-12) continue;
            result[zone] = x / y;
        }
        return result;
    }

    public int[] MaskToArray(object mask) => (int[])mask;
}
