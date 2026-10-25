using System.Text.Json;
using FastZonal;

namespace FastZonal.Tests;

/// <summary>Minimal reader of the fixture layout (benchmarks/fixtures/FORMAT.md) for the tests.</summary>
internal static class FixtureReader
{
    public sealed record ZoneArrays(int ZoneCount, double[] Xy, long[] RingStart, byte[] RingIsHole, int[] RingZone);
    public sealed record CentroidArrays(int[] Rows, int[] Cols);

    public static float[] ReadRasterF32(string dir, string name, out int width, out int height, out Transform t)
    {
        var meta = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, $"raster_{name}.json"))).RootElement;
        width = meta.GetProperty("width").GetInt32();
        height = meta.GetProperty("height").GetInt32();
        var tr = meta.GetProperty("transform").EnumerateArray().Select(e => e.GetDouble()).ToArray();
        t = new Transform(tr[0], tr[1], tr[2], tr[3]);
        if (meta.GetProperty("dtype").GetString() != "f32") throw new NotSupportedException("f32 only");
        double? nodata = meta.GetProperty("nodata").ValueKind == JsonValueKind.Null
            ? null : meta.GetProperty("nodata").GetDouble();
        var bytes = File.ReadAllBytes(Path.Combine(dir, $"raster_{name}.bin"));
        var data = new float[width * height];
        Buffer.BlockCopy(bytes, 0, data, 0, bytes.Length);
        if (nodata.HasValue)
        {
            float nd = (float)nodata.Value;
            for (int i = 0; i < data.Length; i++) if (data[i] == nd) data[i] = -9999f;
        }
        return data;
    }

    public static ZoneArrays ReadZones(string dir, string name)
    {
        var meta = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, $"zones_{name}.json"))).RootElement;
        string b = Path.Combine(dir, $"zones_{name}");
        return new ZoneArrays(meta.GetProperty("n_zones").GetInt32(), Read<double>($"{b}.xy.f64", 8),
            Read<long>($"{b}.ring_start.i64", 8), File.ReadAllBytes($"{b}.ring_is_hole.u8"),
            Read<int>($"{b}.ring_zone.i32", 4));
    }

    public static CentroidArrays ReadCentroids(string dir, string zones, string raster)
    {
        var rc = Read<int>(Path.Combine(dir, $"zones_{zones}.centroid_rc_{raster}.i32"), 4);
        var rows = new int[rc.Length / 2];
        var cols = new int[rc.Length / 2];
        for (int z = 0; z < rows.Length; z++) { rows[z] = rc[2 * z]; cols[z] = rc[2 * z + 1]; }
        return new CentroidArrays(rows, cols);
    }

    private static T[] Read<T>(string path, int size) where T : struct
    {
        var bytes = File.ReadAllBytes(path);
        var arr = new T[bytes.Length / size];
        Buffer.BlockCopy(bytes, 0, arr, 0, arr.Length * size);
        return arr;
    }
}
