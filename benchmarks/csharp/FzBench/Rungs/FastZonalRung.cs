using FastZonal;

namespace FzBench.Rungs;

/// <summary>The fastzonal library through its .NET binding, in COMPAT rasterisation mode so the
/// results match the reference. Centroid cells for the empty-zone fallback are read from the
/// fixture (the same truncating rule as the reference, applied to shapely centroids); the
/// reference computes them lazily for empty zones only, so the cost difference is negligible.
/// A fixture without centroid files for the raster is refused rather than measured with a
/// different fallback.</summary>
public sealed class FastZonalRung : IRung
{
    private readonly Options _options;
    private readonly bool _serial;

    public FastZonalRung(bool serial, int threads, int bandRows = 16)
    {
        _serial = serial;
        _options = new Options
        {
            Backend = serial ? Backend.Serial : Backend.OpenMp,
            Threads = serial ? 1 : threads,
            RasterMode = RasterMode.Compat,
            BandRows = bandRows
        };
    }

    public string Name => _serial ? "fastzonal-serial" : "fastzonal-openmp";

    /// <summary>The library never uses more workers than bands.</summary>
    public int EffectiveThreads(int height)
    {
        if (_serial) return 1;
        int requested = _options.Threads <= 0 ? Environment.ProcessorCount : _options.Threads;
        int bands = (height + _options.BandRows - 1) / _options.BandRows;
        return Math.Min(requested, bands);
    }

    private static FastZonal.Transform Tr(RasterData r) =>
        new(r.Transform.OriginX, r.Transform.OriginY, r.Transform.ScaleX, r.Transform.ScaleY);

    private static (int[] Rows, int[] Cols) Centroids(ZoneData z, string raster) =>
        z.Centroids.TryGetValue(raster, out var c)
            ? c
            : throw new InvalidDataException($"zone set {z.Name} has no centroid file for raster {raster}");

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
        var (rows, cols) = Centroids(z, raster);
        var stats = Zonal.Stats(r.Flat, (int[])mask, r.Width, r.Height, z.ZoneCount, -9999f, _options);
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
        var (rows, cols) = Centroids(z, num);
        var stats = Zonal.Ratio(a.Flat, denOnGrid.Flat, (int[])mask, a.Width, a.Height, z.ZoneCount, -9999f, -9999f,
            1e-12, _options);
        // mean of the ratio with the reference's centroid fallback: the quotient at the centroid
        // cell in double precision, with the reference's exclusion rules
        var result = new double[z.ZoneCount];
        for (int zone = 0; zone < result.Length; zone++)
        {
            if (stats[zone].Count > 0) { result[zone] = stats[zone].Mean; continue; }
            int row = rows[zone], col = cols[zone];
            if (row < 0 || row >= a.Height || col < 0 || col >= a.Width) continue;
            long i = (long)row * a.Width + col;
            double x = a.Flat[i], y = denOnGrid.Flat[i];
            if (double.IsNaN(x) || x == -9999.0 || double.IsNaN(y) || y == -9999.0 || Math.Abs(y) < 1e-12) continue;
            double q = x / y;
            if (double.IsNaN(q) || q == -9999.0) continue;
            result[zone] = q;
        }
        return result;
    }

    public int[] MaskToArray(object mask) => (int[])mask;
}
