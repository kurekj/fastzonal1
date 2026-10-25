using System.Text.Json;
using FzBench.Baseline;
using NetTopologySuite.Geometries;

namespace FzBench;

/// <summary>A raster of the fixture layout (benchmarks/fixtures/FORMAT.md) in both the baseline's
/// double[,] form and a flat float[] form for the other rungs. NoData is remapped to -9999 in the
/// double copy, which is what the reference code checks for.</summary>
public sealed class RasterData
{
    public required string Name { get; init; }
    public required int Width { get; init; }
    public required int Height { get; init; }
    public required GeoTransform Transform { get; init; }
    public required double? NoData { get; init; }
    public required string DType { get; init; }
    public required double[,] Pixels { get; init; }
    public required float[] Flat { get; init; }
    public float FlatNoData => -9999f;
}

/// <summary>A zone set: NetTopologySuite geometries for the baseline and the flat ring arrays for
/// the library, plus the stored centroid cells per raster.</summary>
public sealed class ZoneData
{
    public required string Name { get; init; }
    public required int ZoneCount { get; init; }
    public required List<Geometry> Geometries { get; init; }
    public required double[] Xy { get; init; }
    public required long[] RingStart { get; init; }
    public required byte[] RingIsHole { get; init; }
    public required int[] RingZone { get; init; }
    public required Dictionary<string, (int[] Rows, int[] Cols)> Centroids { get; init; }
}

public sealed class Fixture
{
    public required string Id { get; init; }
    public required string Directory { get; init; }
    public required Dictionary<string, RasterData> Rasters { get; init; }
    public required Dictionary<string, ZoneData> Zones { get; init; }

    public static Fixture Load(string dir)
    {
        var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "fixture.json"))).RootElement;
        var id = doc.GetProperty("id").GetString() ?? Path.GetFileName(dir);
        var rasters = new Dictionary<string, RasterData>();
        foreach (var r in doc.GetProperty("rasters").EnumerateArray())
            rasters[r.GetString()!] = LoadRaster(dir, r.GetString()!);
        var zones = new Dictionary<string, ZoneData>();
        foreach (var z in doc.GetProperty("zones").EnumerateArray())
            zones[z.GetString()!] = LoadZones(dir, z.GetString()!, rasters.Keys);
        return new Fixture { Id = id, Directory = dir, Rasters = rasters, Zones = zones };
    }

    public static RasterData LoadRaster(string dir, string name)
    {
        var meta = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, $"raster_{name}.json"))).RootElement;
        int width = meta.GetProperty("width").GetInt32();
        int height = meta.GetProperty("height").GetInt32();
        var t = meta.GetProperty("transform").EnumerateArray().Select(e => e.GetDouble()).ToArray();
        var transform = new GeoTransform(t[0], t[1], t[2], t[3]);
        double? nodata = meta.GetProperty("nodata").ValueKind == JsonValueKind.Null
            ? null : meta.GetProperty("nodata").GetDouble();
        var dtype = meta.GetProperty("dtype").GetString()!;
        if (!BitConverter.IsLittleEndian)
            throw new PlatformNotSupportedException("fixture binaries are little-endian; this host is not");
        var bytes = File.ReadAllBytes(Path.Combine(dir, $"raster_{name}.bin"));
        long n = (long)width * height;
        int size = dtype switch { "f32" => 4, "f64" => 8, "i32" => 4,
            _ => throw new InvalidDataException($"unsupported dtype {dtype}") };
        if (bytes.LongLength != n * size)
            throw new InvalidDataException($"raster_{name}.bin has {bytes.LongLength} bytes, expected {n * size}");
        var pixels = new double[height, width];
        var flat = new float[n];
        // NoData is compared in the storage type, so a float32 sentinel matches float32 samples exactly.
        float nodata32 = nodata.HasValue ? (float)nodata.Value : 0f;
        int nodataI32 = nodata.HasValue ? (int)nodata.Value : 0;
        for (long i = 0; i < n; i++)
        {
            double v;
            switch (dtype)
            {
                case "f32":
                {
                    float s = BitConverter.ToSingle(bytes, (int)(i * 4));
                    v = (nodata.HasValue && s == nodata32) ? -9999.0 : s;
                    break;
                }
                case "f64":
                {
                    double s = BitConverter.ToDouble(bytes, (int)(i * 8));
                    v = (nodata.HasValue && s == nodata.Value) ? -9999.0 : s;
                    break;
                }
                default:
                {
                    int s = BitConverter.ToInt32(bytes, (int)(i * 4));
                    if (s >= (1 << 24) || s <= -(1 << 24))
                        throw new InvalidDataException($"raster_{name}.bin: integer {s} is not exact in float32");
                    v = (nodata.HasValue && s == nodataI32) ? -9999.0 : s;
                    break;
                }
            }
            pixels[i / width, i % width] = v;
            flat[i] = (float)v;
        }
        return new RasterData
        {
            Name = name, Width = width, Height = height, Transform = transform, NoData = nodata,
            DType = dtype, Pixels = pixels, Flat = flat
        };
    }

    public static ZoneData LoadZones(string dir, string name, IEnumerable<string> rasterNames)
    {
        var meta = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, $"zones_{name}.json"))).RootElement;
        int zoneCount = meta.GetProperty("n_zones").GetInt32();
        string b = Path.Combine(dir, $"zones_{name}");
        var xy = ReadDoubles($"{b}.xy.f64");
        var ringStart = ReadLongs($"{b}.ring_start.i64");
        var ringIsHole = File.ReadAllBytes($"{b}.ring_is_hole.u8");
        var ringZone = ReadInts($"{b}.ring_zone.i32");

        // Rebuild NetTopologySuite geometries: one polygon per exterior ring with its holes; zones
        // with several exterior rings become MultiPolygons.
        var factory = new GeometryFactory();
        var perZone = new List<Polygon>[zoneCount];
        Polygon? current = null;
        var shells = new List<(int zone, LinearRing shell, List<LinearRing> holes)>();
        for (int i = 0; i < ringZone.Length; i++)
        {
            var ring = ReadRing(xy, ringStart[i], ringStart[i + 1], factory);
            if (ringIsHole[i] == 0)
                shells.Add((ringZone[i], ring, new List<LinearRing>()));
            else
                shells[^1].holes.Add(ring);
        }
        foreach (var (zone, shell, holes) in shells)
        {
            (perZone[zone] ??= new List<Polygon>()).Add(factory.CreatePolygon(shell, holes.ToArray()));
        }
        var geoms = new List<Geometry>(zoneCount);
        for (int z = 0; z < zoneCount; z++)
        {
            var polys = perZone[z] ?? new List<Polygon>();
            geoms.Add(polys.Count == 1 ? polys[0] : factory.CreateMultiPolygon(polys.ToArray()));
        }
        _ = current;

        if (ringZone.Length != ringIsHole.Length || ringStart.Length != ringZone.Length + 1)
            throw new InvalidDataException($"zones_{name}: ring arrays disagree");
        var centroids = new Dictionary<string, (int[] Rows, int[] Cols)>();
        foreach (var rn in rasterNames)
        {
            var path = $"{b}.centroid_rc_{rn}.i32";
            if (!File.Exists(path)) continue;
            var rc = ReadInts(path);
            if (rc.Length != 2 * zoneCount)
                throw new InvalidDataException($"zones_{name}: centroid file for {rn} has {rc.Length / 2} entries, expected {zoneCount}");
            var rows = new int[zoneCount];
            var cols = new int[zoneCount];
            for (int z = 0; z < zoneCount; z++) { rows[z] = rc[2 * z]; cols[z] = rc[2 * z + 1]; }
            centroids[rn] = (rows, cols);
        }
        return new ZoneData
        {
            Name = name, ZoneCount = zoneCount, Geometries = geoms, Xy = xy, RingStart = ringStart,
            RingIsHole = ringIsHole, RingZone = ringZone, Centroids = centroids
        };
    }

    private static LinearRing ReadRing(double[] xy, long begin, long end, GeometryFactory factory)
    {
        if (begin < 0 || end < begin || end * 2 > xy.LongLength)
            throw new InvalidDataException("ring offsets point outside the coordinate array");
        int n = (int)(end - begin);
        var coords = new Coordinate[n + 1];
        for (int k = 0; k < n; k++) coords[k] = new Coordinate(xy[2 * (begin + k)], xy[2 * (begin + k) + 1]);
        coords[n] = new Coordinate(coords[0]);
        return factory.CreateLinearRing(coords);
    }

    private static byte[] ReadExact(string path, int elementSize)
    {
        var bytes = File.ReadAllBytes(path);
        if (bytes.Length % elementSize != 0)
            throw new InvalidDataException($"{Path.GetFileName(path)}: {bytes.Length} bytes is not a multiple of {elementSize}");
        return bytes;
    }

    private static double[] ReadDoubles(string path)
    {
        var bytes = ReadExact(path, 8);
        var out_ = new double[bytes.Length / 8];
        Buffer.BlockCopy(bytes, 0, out_, 0, out_.Length * 8);
        return out_;
    }

    private static long[] ReadLongs(string path)
    {
        var bytes = ReadExact(path, 8);
        var out_ = new long[bytes.Length / 8];
        Buffer.BlockCopy(bytes, 0, out_, 0, out_.Length * 8);
        return out_;
    }

    private static int[] ReadInts(string path)
    {
        var bytes = ReadExact(path, 4);
        var out_ = new int[bytes.Length / 4];
        Buffer.BlockCopy(bytes, 0, out_, 0, out_.Length * 4);
        return out_;
    }

    /// <summary>The denominator of the ratio workload on the numerator's grid: the same object when
    /// the grids already match, otherwise a nearest-neighbour resampled copy. Called once by the
    /// driver before any timing; every rung receives the same prepared raster.</summary>
    public static RasterData PrepareDenominator(RasterData num, RasterData den)
    {
        if (ReferenceEquals(num, den) ||
            (den.Width == num.Width && den.Height == num.Height && den.Transform == num.Transform))
            return den;
        var pixels = ResampleNearest(den, num);
        var flat = new float[(long)num.Width * num.Height];
        for (int r = 0; r < num.Height; r++)
            for (int c = 0; c < num.Width; c++)
                flat[(long)r * num.Width + c] = (float)pixels[r, c];
        return new RasterData
        {
            Name = den.Name + "@" + num.Name, Width = num.Width, Height = num.Height, Transform = num.Transform,
            NoData = -9999.0, DType = "f64", Pixels = pixels, Flat = flat
        };
    }

    /// <summary>Nearest-neighbour resampling of <paramref name="src"/> onto the grid of
    /// <paramref name="target"/>; cells outside the source extent become -9999.</summary>
    public static double[,] ResampleNearest(RasterData src, RasterData target)
    {
        var o = new double[target.Height, target.Width];
        for (int r = 0; r < target.Height; r++)
        {
            double y = target.Transform.OriginY - (r + 0.5) * target.Transform.ScaleY;
            int sr = (int)Math.Floor((src.Transform.OriginY - y) / src.Transform.ScaleY);
            for (int c = 0; c < target.Width; c++)
            {
                double x = target.Transform.OriginX + (c + 0.5) * target.Transform.ScaleX;
                int sc = (int)Math.Floor((x - src.Transform.OriginX) / src.Transform.ScaleX);
                o[r, c] = (sr < 0 || sr >= src.Height || sc < 0 || sc >= src.Width) ? -9999.0 : src.Pixels[sr, sc];
            }
        }
        return o;
    }
}
