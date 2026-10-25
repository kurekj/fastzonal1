using FzBench.Baseline;
using NetTopologySuite.Geometries;

namespace FzBench.Rungs;

/// <summary>The reference algorithms rewritten the way a careful C# developer would: float[]
/// rasters, int[] masks, flat ring arrays with pixel bounding boxes prepared per call (inside the
/// timed region, like the library's own per-call pre-pass and the baseline's coordinate access),
/// specialised loops without delegates, and Parallel.For over bands of rows with per-band partial
/// results merged in ascending band order, so the result does not depend on the thread count. Same semantics as the baseline (COMPAT rasterisation, NoData -9999
/// or NaN, centroid fallback, smallest-value ties); the summation order differs from the
/// baseline's single sequential pass, so means agree to rounding, not bit for bit.</summary>
public sealed class OptimisedRung : IRung
{
    private readonly int _bandRows;
    private readonly int _threads;

    public OptimisedRung(int threads, int bandRows = 16)
    {
        _threads = threads <= 0 ? Environment.ProcessorCount : threads;
        _bandRows = bandRows;
    }

    public string Name => "csharp-optimised";
    public int EffectiveThreads(int height) => Math.Min(_threads, Bands(height));

    private int Bands(int height) => (height + _bandRows - 1) / _bandRows;
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
        var rings = PrepareRings(f.Zones[zones].Geometries, t, width, height, Par);
        var mask = new int[(long)width * height];
        int nBands = Bands(height);
        // every band applies, in polygon order, the rings whose row range touches it, so later
        // polygons win exactly as in the reference
        var byBand = new List<int>[nBands];
        for (int b = 0; b < nBands; b++) byBand[b] = new List<int>();
        for (int i = 0; i < rings.Count; i++)
        {
            var p = rings[i];
            if (p.StartRow > p.EndRow) continue;
            for (int b = p.StartRow / _bandRows; b <= p.EndRow / _bandRows && b < nBands; b++) byBand[b].Add(i);
        }
        Parallel.For(0, nBands, Par, () => new List<double>(64), (b, _, crossings) =>
        {
            int row0 = b * _bandRows, row1 = Math.Min(height, row0 + _bandRows);
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

    /// <summary>Flat ring arrays with pixel bounding boxes; the per-polygon work is independent
    /// and runs in parallel, the list is assembled in zone order.</summary>
    private static List<PolyRing> PrepareRings(List<Geometry> geoms, GeoTransform t, int width, int height,
        ParallelOptions par)
    {
        var perZone = new PolyRing[]?[geoms.Count];
        Parallel.For(0, geoms.Count, par, zone =>
        {
            var list = new List<PolyRing>(1);
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
                list.Add(new PolyRing
                {
                    Zone = zone, X = x, Y = y,
                    StartCol = Math.Max(0, t.WorldToCol(minX)), EndCol = Math.Min(width - 1, t.WorldToCol(maxX)),
                    StartRow = Math.Max(0, t.WorldToRow(maxY)), EndRow = Math.Min(height - 1, t.WorldToRow(minY))
                });
            }
            perZone[zone] = list.ToArray();
        });
        var rings = new List<PolyRing>(geoms.Count);
        foreach (var arr in perZone) if (arr != null) rings.AddRange(arr);
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

    /// <summary>Thread-local dense accumulators with a seen flag per zone; after every band the
    /// touched zones are copied out as that band's partial and the accumulators are reset.</summary>
    private sealed class Acc
    {
        public readonly double[] Sum;
        public readonly long[] Count;
        public readonly bool[] Seen;
        public readonly List<int> Touched = new();
        public Acc(int n) { Sum = new double[n]; Count = new long[n]; Seen = new bool[n]; }

        public void Touch(int zone)
        {
            if (Seen[zone]) return;
            Seen[zone] = true;
            Touched.Add(zone);
        }

        public (int[] Zones, double[] Sum, long[] Count) Flush()
        {
            var zones = Touched.ToArray();
            var sum = new double[zones.Length];
            var count = new long[zones.Length];
            for (int i = 0; i < zones.Length; i++)
            {
                int z = zones[i];
                sum[i] = Sum[z];
                count[i] = Count[z];
                Sum[z] = 0.0;
                Count[z] = 0;
                Seen[z] = false;
            }
            Touched.Clear();
            return (zones, sum, count);
        }
    }

    public double[] Mean(Fixture f, string zones, string raster, object mask)
    {
        var r = f.Rasters[raster];
        var z = f.Zones[zones];
        var m = (int[])mask;
        int width = r.Width, height = r.Height, n = z.ZoneCount;
        var values = r.Flat;
        int nBands = Bands(height);
        var partials = new (int[] Zones, double[] Sum, long[] Count)[nBands];
        Parallel.For(0, nBands, Par, () => new Acc(n), (b, _, acc) =>
        {
            long begin = (long)b * _bandRows * width;
            long end = Math.Min((long)height, (long)(b + 1) * _bandRows) * width;
            int prev = -1;
            var sum = acc.Sum;
            var count = acc.Count;
            for (long i = begin; i < end; i++)
            {
                int zone = m[i];
                if (zone < 0) continue;
                if (zone != prev) { acc.Touch(zone); prev = zone; }
                float v = values[i];
                if (float.IsNaN(v) || v == -9999f) continue;
                sum[zone] += v;
                count[zone]++;
            }
            partials[b] = acc.Flush();
            return acc;
        }, _ => { });
        var total = new double[n];
        var counts = new long[n];
        foreach (var (zs, sums, cs) in partials)  // ascending band order: the fixed merge order
            for (int i = 0; i < zs.Length; i++) { total[zs[i]] += sums[i]; counts[zs[i]] += cs[i]; }
        var result = new double[n];
        var t = r.Transform;
        for (int zone = 0; zone < n; zone++)
        {
            if (counts[zone] > 0) { result[zone] = total[zone] / counts[zone]; continue; }
            var centroid = z.Geometries[zone].Centroid;
            int col = t.WorldToCol(centroid.X), row = t.WorldToRow(centroid.Y);
            if (col < 0 || col >= width || row < 0 || row >= height) continue;
            float cv = values[(long)row * width + col];
            if (!(float.IsNaN(cv) || cv == -9999f)) result[zone] = cv;
        }
        return result;
    }

    public double[] Majority(Fixture f, string zones, string raster, object mask)
    {
        var r = f.Rasters[raster];
        var z = f.Zones[zones];
        var m = (int[])mask;
        int n = z.ZoneCount, width = r.Width, height = r.Height;
        var values = r.Flat;
        int nBands = Bands(height);
        var partials = new List<(int Zone, Dictionary<float, int> Counts)>[nBands];
        Parallel.For(0, nBands, Par, () => new Dictionary<float, int>?[n], (b, _, local) =>
        {
            long begin = (long)b * _bandRows * width;
            long end = Math.Min((long)height, (long)(b + 1) * _bandRows) * width;
            var touched = new List<int>();
            for (long i = begin; i < end; i++)
            {
                int zone = m[i];
                if (zone < 0) continue;
                float v = values[i];
                if (float.IsNaN(v) || v == -9999f) continue;
                var d = local[zone];
                if (d == null) { d = local[zone] = new Dictionary<float, int>(); touched.Add(zone); }
                d[v] = d.TryGetValue(v, out var c) ? c + 1 : 1;
            }
            var part = new List<(int, Dictionary<float, int>)>(touched.Count);
            foreach (int zone in touched) { part.Add((zone, local[zone]!)); local[zone] = null; }
            partials[b] = part;
            return local;
        }, _ => { });
        var merged = new Dictionary<float, int>?[n];
        foreach (var part in partials)  // integer counts commute; band order kept for clarity
            foreach (var (zone, counts) in part)
            {
                var d = merged[zone];
                if (d == null) { merged[zone] = counts; continue; }
                foreach (var (v, c) in counts) d[v] = d.TryGetValue(v, out var old) ? old + c : c;
            }
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
            float cv = values[(long)row * width + col];
            if (!(float.IsNaN(cv) || cv == -9999f)) result[zone] = cv;
        }
        return result;
    }

    public double[] Ratio(Fixture f, string zones, string num, RasterData denOnGrid, object mask)
    {
        var a = f.Rasters[num];
        var z = f.Zones[zones];
        var m = (int[])mask;
        int width = a.Width, height = a.Height, n = z.ZoneCount;
        var ap = a.Pixels;
        var bp = denOnGrid.Pixels;
        // the ratio raster in double, as the reference forms it (NaN where excluded, and a computed
        // -9999 is then NoData for the mean, exactly like the reference), band-parallel so the
        // configured worker limit is the same for the whole operation
        var ratio = new double[(long)width * height];
        int nBands = Bands(height);
        Parallel.For(0, nBands, Par, b =>
        {
            int row0 = b * _bandRows, row1 = Math.Min(height, row0 + _bandRows);
            for (int row = row0; row < row1; row++)
            {
                long rowBase = (long)row * width;
                for (int col = 0; col < width; col++)
                {
                    double x = ap[row, col], y = bp[row, col];
                    ratio[rowBase + col] = (double.IsNaN(x) || x == -9999.0 || double.IsNaN(y) || y == -9999.0 ||
                                            Math.Abs(y) < 1e-12) ? double.NaN : x / y;
                }
            }
        });
        var partials = new (int[] Zones, double[] Sum, long[] Count)[nBands];
        Parallel.For(0, nBands, Par, () => new Acc(n), (b, _, acc) =>
        {
            long begin = (long)b * _bandRows * width;
            long end = Math.Min((long)height, (long)(b + 1) * _bandRows) * width;
            int prev = -1;
            var sum = acc.Sum;
            var count = acc.Count;
            for (long i = begin; i < end; i++)
            {
                int zone = m[i];
                if (zone < 0) continue;
                if (zone != prev) { acc.Touch(zone); prev = zone; }
                double v = ratio[i];
                if (double.IsNaN(v) || v == -9999.0) continue;
                sum[zone] += v;
                count[zone]++;
            }
            partials[b] = acc.Flush();
            return acc;
        }, _ => { });
        var total = new double[n];
        var counts = new long[n];
        foreach (var (zs, sums, cs) in partials)
            for (int i = 0; i < zs.Length; i++) { total[zs[i]] += sums[i]; counts[zs[i]] += cs[i]; }
        var result = new double[n];
        var t = a.Transform;
        for (int zone = 0; zone < n; zone++)
        {
            if (counts[zone] > 0) { result[zone] = total[zone] / counts[zone]; continue; }
            var centroid = z.Geometries[zone].Centroid;
            int col = t.WorldToCol(centroid.X), row = t.WorldToRow(centroid.Y);
            if (col < 0 || col >= width || row < 0 || row >= height) continue;
            double cv = ratio[(long)row * width + col];
            if (!(double.IsNaN(cv) || cv == -9999.0)) result[zone] = cv;
        }
        return result;
    }

    public int[] MaskToArray(object mask) => (int[])mask;
}
