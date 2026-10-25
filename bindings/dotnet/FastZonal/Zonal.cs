namespace FastZonal;

/// <summary>Zonal reductions over a zone-index mask (fz_zonal_*).</summary>
public static unsafe class Zonal
{
    private static Native.FzShape Shape(int width, int height, long rasterLength, long maskLength)
    {
        if (width <= 0 || height <= 0) throw new ArgumentException("width and height must be positive");
        long n = (long)width * height;
        if (rasterLength != n || maskLength != n)
            throw new ArgumentException("raster and mask must have width * height entries");
        return new Native.FzShape { Width = width, Height = height };
    }

    private static void CheckCentroids(ReadOnlySpan<int> rows, ReadOnlySpan<int> cols, int nZones)
    {
        if (rows.IsEmpty != cols.IsEmpty)
            throw new ArgumentException("centroidRow and centroidCol must both be given or both be empty");
        if (!rows.IsEmpty && (rows.Length != nZones || cols.Length != nZones))
            throw new ArgumentException("centroid arrays must have nZones entries");
    }

    public static ZoneStats[] Stats(ReadOnlySpan<float> raster, ReadOnlySpan<int> mask, int width, int height,
        int nZones, float nodata, Options? options = null)
    {
        var s = Shape(width, height, raster.Length, mask.Length);
        if (nZones <= 0) throw new ArgumentException("nZones must be positive");
        var stats = new ZoneStats[nZones];
        var opt = (options ?? Options.Default).ToNative();
        fixed (float* pr = raster) fixed (int* pm = mask) fixed (ZoneStats* ps = stats)
            Native.Check(Native.fz_zonal_f32(pr, pm, s, nZones, nodata, ps, &opt));
        return stats;
    }

    public static ZoneStats[] Stats(ReadOnlySpan<double> raster, ReadOnlySpan<int> mask, int width, int height,
        int nZones, double nodata, Options? options = null)
    {
        var s = Shape(width, height, raster.Length, mask.Length);
        if (nZones <= 0) throw new ArgumentException("nZones must be positive");
        var stats = new ZoneStats[nZones];
        var opt = (options ?? Options.Default).ToNative();
        fixed (double* pr = raster) fixed (int* pm = mask) fixed (ZoneStats* ps = stats)
            Native.Check(Native.fz_zonal_f64(pr, pm, s, nZones, nodata, ps, &opt));
        return stats;
    }

    /// <summary>Mean per zone; an empty zone takes the raster value at its centroid cell (both
    /// centroid arrays given) or 0.</summary>
    public static double[] MeanWithFallback(ReadOnlySpan<float> raster, int width, int height, float nodata,
        ReadOnlySpan<ZoneStats> stats, ReadOnlySpan<int> centroidRow = default, ReadOnlySpan<int> centroidCol = default)
    {
        var s = Shape(width, height, raster.Length, raster.Length);
        CheckCentroids(centroidRow, centroidCol, stats.Length);
        var mean = new double[stats.Length];
        fixed (float* pr = raster) fixed (ZoneStats* ps = stats) fixed (int* prow = centroidRow)
        fixed (int* pcol = centroidCol) fixed (double* pm = mean)
            Native.Check(Native.fz_zonal_mean_fallback_f32(pr, s, nodata, ps, stats.Length,
                centroidRow.IsEmpty ? null : prow, centroidCol.IsEmpty ? null : pcol, pm));
        return mean;
    }

    public static double[] MeanWithFallback(ReadOnlySpan<double> raster, int width, int height, double nodata,
        ReadOnlySpan<ZoneStats> stats, ReadOnlySpan<int> centroidRow = default, ReadOnlySpan<int> centroidCol = default)
    {
        var s = Shape(width, height, raster.Length, raster.Length);
        CheckCentroids(centroidRow, centroidCol, stats.Length);
        var mean = new double[stats.Length];
        fixed (double* pr = raster) fixed (ZoneStats* ps = stats) fixed (int* prow = centroidRow)
        fixed (int* pcol = centroidCol) fixed (double* pm = mean)
            Native.Check(Native.fz_zonal_mean_fallback_f64(pr, s, nodata, ps, stats.Length,
                centroidRow.IsEmpty ? null : prow, centroidCol.IsEmpty ? null : pcol, pm));
        return mean;
    }

    /// <summary>Mode per zone (ties to the smallest value); <paramref name="hasValue"/> is false for a
    /// zone with no cell and no usable centroid cell.</summary>
    public static double[] Majority(ReadOnlySpan<float> raster, ReadOnlySpan<int> mask, int width, int height,
        int nZones, float nodata, ReadOnlySpan<int> centroidRow, ReadOnlySpan<int> centroidCol, out bool[] hasValue,
        Options? options = null)
    {
        var s = Shape(width, height, raster.Length, mask.Length);
        if (nZones <= 0) throw new ArgumentException("nZones must be positive");
        CheckCentroids(centroidRow, centroidCol, nZones);
        var value = new double[nZones];
        var has = new byte[nZones];
        var opt = (options ?? Options.Default).ToNative();
        fixed (float* pr = raster) fixed (int* pm = mask) fixed (int* prow = centroidRow) fixed (int* pcol = centroidCol)
        fixed (double* pv = value) fixed (byte* ph = has)
            Native.Check(Native.fz_zonal_majority_f32(pr, pm, s, nZones, nodata, centroidRow.IsEmpty ? null : prow,
                centroidCol.IsEmpty ? null : pcol, pv, ph, &opt));
        hasValue = Array.ConvertAll(has, b => b != 0);
        return value;
    }

    public static double[] Majority(ReadOnlySpan<double> raster, ReadOnlySpan<int> mask, int width, int height,
        int nZones, double nodata, ReadOnlySpan<int> centroidRow, ReadOnlySpan<int> centroidCol, out bool[] hasValue,
        Options? options = null)
    {
        var s = Shape(width, height, raster.Length, mask.Length);
        if (nZones <= 0) throw new ArgumentException("nZones must be positive");
        CheckCentroids(centroidRow, centroidCol, nZones);
        var value = new double[nZones];
        var has = new byte[nZones];
        var opt = (options ?? Options.Default).ToNative();
        fixed (double* pr = raster) fixed (int* pm = mask) fixed (int* prow = centroidRow) fixed (int* pcol = centroidCol)
        fixed (double* pv = value) fixed (byte* ph = has)
            Native.Check(Native.fz_zonal_majority_f64(pr, pm, s, nZones, nodata, centroidRow.IsEmpty ? null : prow,
                centroidCol.IsEmpty ? null : pcol, pv, ph, &opt));
        hasValue = Array.ConvertAll(has, b => b != 0);
        return value;
    }

    public static double[] Majority(ReadOnlySpan<int> raster, ReadOnlySpan<int> mask, int width, int height,
        int nZones, int nodata, ReadOnlySpan<int> centroidRow, ReadOnlySpan<int> centroidCol, out bool[] hasValue,
        Options? options = null)
    {
        var s = Shape(width, height, raster.Length, mask.Length);
        if (nZones <= 0) throw new ArgumentException("nZones must be positive");
        CheckCentroids(centroidRow, centroidCol, nZones);
        var value = new double[nZones];
        var has = new byte[nZones];
        var opt = (options ?? Options.Default).ToNative();
        fixed (int* pr = raster) fixed (int* pm = mask) fixed (int* prow = centroidRow) fixed (int* pcol = centroidCol)
        fixed (double* pv = value) fixed (byte* ph = has)
            Native.Check(Native.fz_zonal_majority_i32(pr, pm, s, nZones, nodata, centroidRow.IsEmpty ? null : prow,
                centroidCol.IsEmpty ? null : pcol, pv, ph, &opt));
        hasValue = Array.ConvertAll(has, b => b != 0);
        return value;
    }

    /// <summary>Statistics of the cell-wise ratio num / den on one grid; cells with NoData in either
    /// raster or |den| &lt; eps are excluded.</summary>
    public static ZoneStats[] Ratio(ReadOnlySpan<float> num, ReadOnlySpan<float> den, ReadOnlySpan<int> mask,
        int width, int height, int nZones, float nodataNum, float nodataDen, double eps = 1e-12,
        Options? options = null)
    {
        var s = Shape(width, height, num.Length, mask.Length);
        if (den.Length != num.Length) throw new ArgumentException("num and den must have the same length");
        if (nZones <= 0) throw new ArgumentException("nZones must be positive");
        var stats = new ZoneStats[nZones];
        var opt = (options ?? Options.Default).ToNative();
        fixed (float* pn = num) fixed (float* pd = den) fixed (int* pm = mask) fixed (ZoneStats* ps = stats)
            Native.Check(Native.fz_zonal_ratio_f32(pn, pd, pm, s, nZones, nodataNum, nodataDen, eps, ps, &opt));
        return stats;
    }

    public static ZoneStats[] Ratio(ReadOnlySpan<double> num, ReadOnlySpan<double> den, ReadOnlySpan<int> mask,
        int width, int height, int nZones, double nodataNum, double nodataDen, double eps = 1e-12,
        Options? options = null)
    {
        var s = Shape(width, height, num.Length, mask.Length);
        if (den.Length != num.Length) throw new ArgumentException("num and den must have the same length");
        if (nZones <= 0) throw new ArgumentException("nZones must be positive");
        var stats = new ZoneStats[nZones];
        var opt = (options ?? Options.Default).ToNative();
        fixed (double* pn = num) fixed (double* pd = den) fixed (int* pm = mask) fixed (ZoneStats* ps = stats)
            Native.Check(Native.fz_zonal_ratio_f64(pn, pd, pm, s, nZones, nodataNum, nodataDen, eps, ps, &opt));
        return stats;
    }
}
