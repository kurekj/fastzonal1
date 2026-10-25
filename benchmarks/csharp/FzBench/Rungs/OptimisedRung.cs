using FzBench.Baseline;
using NetTopologySuite.Geometries;

namespace FzBench.Rungs;

/// <summary>The reference algorithms rewritten the way a careful C# developer would: float[]
/// rasters, int[] masks, no per-row allocations, and Parallel.For over bands of 64 rows with
/// thread-local accumulators. Same semantics as the baseline (COMPAT rasterisation, NoData
/// -9999 or NaN, centroid fallback, smallest-value ties); the floating-point summation order
/// differs from the baseline, so means agree to rounding, not bit for bit.</summary>
public sealed class OptimisedRung : IRung
{
    private const int BandRows = 64;
    private readonly int _threads;

    public OptimisedRung(int threads)
    {
        _threads = threads <= 0 ? Environment.ProcessorCount : threads;
    }

    public string Name => "csharp-optimised";
    public int EffectiveThreads => _threads;

    private ParallelOptions Par => new() { MaxDegreeOfParallelism = _threads };

    // ---- rasterisation --------------------------------------------------------------------

    private sealed class PolyRing
    {
        public required int Zone;
        public required double[] X;
        public required double[] Y;
        public required int StartRow, EndRow, StartCol, EndCol;
    }

    public object Rasterize(Fixture f, string zones, string raster)
    {
        var r = f.Rasters[raster];
        var t = r.Transform;
        int width = r.Width, height = r.Height;
        var rings = PrepareRings(f.Zones[zones].Geometries, t, width, height);
        var mask = new int[(long)width * height];
        int nBands = (height + BandRows - 1) / BandRows;
        // rings sorted by zone index already (they are produced in geometry order); every band
        // applies the rings touching it in that order, so later polygons win as in the reference
        var byBand = new List<int>[nBands];
        for (int b = 0; b < nBands; b++) byBand[b] = new List<int>();
        for (int i = 0; i < rings.Count; i++)
        {
            var p = rings[i];
            if (p.StartRow > p.EndRow) continue;
            for (int b = p.StartRow / BandRows; b <= p.EndRow / BandRows && b < nBands; b++) byBand[b].Add(i);
        }
        Parallel.For(0, nBands, Par, () => new List<double>(64), (b, _, crossings) =>
        {
            int row0 = b * BandRows, row1 = Math.Min(height, row0 + BandRows);
            Array.Fill(mask, -1, row0 * width, (row1 - row0) * width);
            foreach (int i in byBand[b])
            {
                var p = rings[i];
                int rb = Math.Max(p.StartRow, row0), re = Math.Min(p.EndRow, row1 - 1);
                for (int row = rb; row <= re; row++)
                {
                    double wy = t.OriginY - (row + 0.5) * t.ScaleY;
                    crossings.Clear();
                    for (int k = 0; k < p.X.Length - 1; k++)
                    {
                        double y1 = p.Y[k], y2 = p.Y[k + 1];
                        if ((y1 <= wy && y2 > wy) || (y2 <= wy && y1 > wy))
                            crossings.Add(p.X[k] + (wy - y1) / (y2 - y1) * (p.X[k + 1] - p.X[k]));
                    }
                    crossings.Sort();
                    int rowBase = row * width;
                    for (int k = 0; k + 1 < crossings.Count; k += 2)
                    {
                        int cStart = Math.Max(p.StartCol, t.WorldToCol(crossings[k]));
                        int cEnd = Math.Min(p.EndCol, t.WorldToCol(crossings[k + 1]));
                        for (int col = cStart; col <= cEnd; col++) mask[rowBase + col] = p.Zone;
                    }
                }
            }
            return crossings;
        }, _ => { });
        return mask;
    }

    private static List<PolyRing> PrepareRings(List<Geometry> geoms, GeoTransform t, int width, int height)
    {
        var rings = new List<PolyRing>();
        for (int zone = 0; zone < geoms.Count; zone++)
        {
            foreach (var polygon in Parts(geoms[zone]))
            {
                var coords = polygon.ExteriorRing.Coordinates;
                var x = new double[coords.Length];
                var y = new double[coords.Length];
                double minX = double.MaxValue, maxX = double.MinValue, minY = double.MaxValue, maxY = double.MinValue;
                for (int i = 0; i < coords.Length; i++)
                {
                    x[i] = coords[i].X;
                    y[i] = coords[i].Y;
                    if (x[i] < minX) minX = x[i];
                    if (x[i] > maxX) maxX = x[i];
                    if (y[i] < minY) minY = y[i];
                    if (y[i] > maxY) maxY = y[i];
                }
                rings.Add(new PolyRing
                {
                    Zone = zone, X = x, Y = y,
                    StartCol = Math.Max(0, t.WorldToCol(minX)), EndCol = Math.Min(width - 1, t.WorldToCol(maxX)),
                    StartRow = Math.Max(0, t.WorldToRow(maxY)), EndRow = Math.Min(height - 1, t.WorldToRow(minY))
                });
            }
        }
        return rings;
    }

    private static IEnumerable<Polygon> Parts(Geometry g)
    {
        if (g is Polygon p) yield return p;
        else if (g is GeometryCollection gc)
            for (int i = 0; i < gc.NumGeometries; i++)
                foreach (var q in Parts(gc.GetGeometryN(i))) yield return q;
    }

    // ---- reductions ------------------------------------------------------------------------

    private sealed class Acc
    {
        public readonly double[] Sum;
        public readonly long[] Count;
        public Acc(int n) { Sum = new double[n]; Count = new long[n]; }
    }

    private static bool IsNoData(float v) => float.IsNaN(v) || v == -9999f;

    public double[] Mean(Fixture f, string zones, string raster, object mask)
    {
        var r = f.Rasters[raster];
        var z = f.Zones[zones];
        return MeanOf(r.Flat, (int[])mask, r.Width, r.Height, z, r.Transform, i => r.Flat[i]);
    }

    private double[] MeanOf(float[] values, int[] mask, int width, int height, ZoneData z, GeoTransform t,
        Func<long, float> sampleAt)
    {
        int n = z.ZoneCount;
        var total = new Acc(n);
        var gate = new object();
        int nBands = (height + BandRows - 1) / BandRows;
        Parallel.For(0, nBands, Par, () => new Acc(n), (b, _, acc) =>
        {
            long begin = (long)b * BandRows * width;
            long end = Math.Min((long)height, (long)(b + 1) * BandRows) * width;
            for (long i = begin; i < end; i++)
            {
                int zone = mask[i];
                if (zone < 0) continue;
                float v = values[i];
                if (IsNoData(v)) continue;
                acc.Sum[zone] += v;
                acc.Count[zone]++;
            }
            return acc;
        }, acc =>
        {
            lock (gate)
                for (int zone = 0; zone < n; zone++) { total.Sum[zone] += acc.Sum[zone]; total.Count[zone] += acc.Count[zone]; }
        });
        var result = new double[n];
        for (int zone = 0; zone < n; zone++)
        {
            if (total.Count[zone] > 0) { result[zone] = total.Sum[zone] / total.Count[zone]; continue; }
            var centroid = z.Geometries[zone].Centroid;
            int col = t.WorldToCol(centroid.X), row = t.WorldToRow(centroid.Y);
            if (col < 0 || col >= width || row < 0 || row >= height) continue;
            float cv = sampleAt((long)row * width + col);
            if (!IsNoData(cv)) result[zone] = cv;
        }
        return result;
    }

    public double[] Majority(Fixture f, string zones, string raster, object mask)
    {
        var r = f.Rasters[raster];
        var z = f.Zones[zones];
        var m = (int[])mask;
        int n = z.ZoneCount, width = r.Width, height = r.Height;
        var merged = new Dictionary<float, int>?[n];
        var gate = new object();
        int nBands = (height + BandRows - 1) / BandRows;
        Parallel.For(0, nBands, Par, () => new Dictionary<float, int>?[n], (b, _, local) =>
        {
            long begin = (long)b * BandRows * width;
            long end = Math.Min((long)height, (long)(b + 1) * BandRows) * width;
            for (long i = begin; i < end; i++)
            {
                int zone = m[i];
                if (zone < 0) continue;
                float v = r.Flat[i];
                if (IsNoData(v)) continue;
                var d = local[zone] ??= new Dictionary<float, int>();
                d[v] = d.TryGetValue(v, out var c) ? c + 1 : 1;
            }
            return local;
        }, local =>
        {
            lock (gate)
                for (int zone = 0; zone < n; zone++)
                {
                    if (local[zone] == null) continue;
                    var d = merged[zone] ??= new Dictionary<float, int>();
                    foreach (var (v, c) in local[zone]!) d[v] = d.TryGetValue(v, out var old) ? old + c : c;
                }
        });
        var result = new double[n];
        for (int zone = 0; zone < n; zone++)
        {
            var d = merged[zone];
            if (d != null && d.Count > 0)
            {
                int best = -1; float bestVal = 0;
                foreach (var (v, c) in d)
                    if (c > best || (c == best && v < bestVal)) { best = c; bestVal = v; }
                result[zone] = bestVal;
                continue;
            }
            var centroid = z.Geometries[zone].Centroid;
            int col = r.Transform.WorldToCol(centroid.X), row = r.Transform.WorldToRow(centroid.Y);
            if (col < 0 || col >= width || row < 0 || row >= height) continue;
            float cv = r.Flat[(long)row * width + col];
            if (!IsNoData(cv)) result[zone] = cv;
        }
        return result;
    }

    public double[] Ratio(Fixture f, string zones, string num, RasterData denOnGrid, object mask)
    {
        var a = f.Rasters[num];
        var z = f.Zones[zones];
        int width = a.Width, height = a.Height;
        var ratio = new float[(long)width * height];
        var bf = denOnGrid.Flat;
        Parallel.For(0, height, Par, row =>
        {
            long rowBase = (long)row * width;
            for (int col = 0; col < width; col++)
            {
                long i = rowBase + col;
                float x = a.Flat[i], y = bf[i];
                ratio[i] = (IsNoData(x) || IsNoData(y) || Math.Abs(y) < 1e-12) ? float.NaN : x / y;
            }
        });
        return MeanOf(ratio, (int[])mask, width, height, z, a.Transform, i => ratio[i]);
    }

    public int[] MaskToArray(object mask) => (int[])mask;
}
