using System.Globalization;
using FastZonal;
using Xunit;

namespace FastZonal.Tests;

public class ZonalTests
{
    private static string RepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null && !File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt"))) dir = dir.Parent;
        return dir?.FullName ?? throw new InvalidOperationException("repository root not found");
    }

    [Fact]
    public void VersionIsReported()
    {
        Assert.Equal("0.1.0", Native.Version());
        Assert.True(Native.HasOpenMp() || !Native.HasOpenMp());  // callable either way
    }

    [Fact]
    public void StatsOnSmallRasterMatchHandValues()
    {
        float[] raster = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 20, 30, 40, 99, 99, 99, 99 };
        int[] mask = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, -1, -1, -1, -1 };
        var stats = Zonal.Stats(raster, mask, width: 4, height: 4, nZones: 2, nodata: -9999f,
            new Options { Backend = Backend.Serial });
        Assert.Equal(8, stats[0].Count);
        Assert.Equal(36.0, stats[0].Sum);
        Assert.Equal(4.5, stats[0].Mean);
        Assert.Equal(1.0, stats[0].Min);
        Assert.Equal(8.0, stats[0].Max);
        Assert.Equal(25.0, stats[1].Mean);
        Assert.Equal(0.0, new ZoneStats().Mean);
    }

    [Fact]
    public void RasterizeUnitSquareCenterMode()
    {
        // square (2,2)-(5,5) on a 10x10 unit grid with origin (0,10): columns 2..4 of rows 5..7
        double[] xy = { 2, 2, 5, 2, 5, 5, 2, 5 };
        long[] ringStart = { 0, 4 };
        byte[] isHole = { 0 };
        int[] zone = { 0 };
        var mask = Rasterizer.Rasterize(xy, ringStart, isHole, zone, new Transform(0, 10, 1, 1), width: 10, height: 10);
        Assert.Equal(100, mask.Length);
        Assert.Equal(9, mask.Count(v => v == 0));
        Assert.Equal(0, mask[5 * 10 + 2]);
        Assert.Equal(-1, mask[4 * 10 + 2]);
        var compat = Rasterizer.Rasterize(xy, ringStart, isHole, zone, new Transform(0, 10, 1, 1), 10, 10,
            new Options { RasterMode = RasterMode.Compat });
        Assert.Equal(12, compat.Count(v => v == 0));
    }

    [Fact]
    public void MajorityAndRatioOnSmallRaster()
    {
        float[] raster = { 3, 3, 1, 1, 5, 5, 5, 2, 7, 7, 8, 8, 0, 0, 0, 0 };
        int[] mask = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, -1, -1, -1, -1 };
        var majority = Zonal.Majority(raster, mask, 4, 4, 2, -9999f, centroidRow: null, centroidCol: null,
            out var hasValue);
        Assert.Equal(5.0, majority[0]);
        Assert.Equal(7.0, majority[1]);
        Assert.True(hasValue[0] && hasValue[1]);
        float[] den = { 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1 };
        var ratio = Zonal.Ratio(raster, den, mask, 4, 4, 2, -9999f, -9999f);
        Assert.Equal(8, ratio[0].Count);
        Assert.Equal((3 + 3 + 1 + 1 + 5 + 5 + 5 + 1) / 8.0, ratio[0].Mean);
    }

    [Fact]
    public void MeanWithFallbackSamplesCentroidForEmptyZone()
    {
        float[] raster = Enumerable.Repeat(2f, 16).ToArray();
        raster[3 * 4 + 1] = 7f;
        int[] mask = Enumerable.Repeat(0, 16).ToArray();
        var stats = Zonal.Stats(raster, mask, 4, 4, 2, -9999f);
        var mean = Zonal.MeanWithFallback(raster, 4, 4, -9999f, stats, new[] { 0, 3 }, new[] { 0, 1 });
        Assert.Equal(37.0 / 16.0, mean[0]);
        Assert.Equal(7.0, mean[1]);
    }

    [Fact]
    public void InvalidArgumentsThrowWithTheLibraryMessage()
    {
        float[] raster = new float[16];
        int[] mask = new int[16];
        mask[0] = 5;  // zone id outside [-1, nZones)
        var ex = Assert.Throws<FastZonalException>(() => Zonal.Stats(raster, mask, 4, 4, 2, -9999f));
        Assert.Equal(1, ex.Code);
        Assert.Equal("invalid argument", ex.Message);
        Assert.Throws<ArgumentException>(() => Zonal.Stats(raster, mask, 4, 5, 2, -9999f));  // length mismatch
    }

    [Fact]
    public void ParityWithTheCsharpBaselineOnLezeMicro()
    {
        var dir = Path.Combine(RepoRoot(), "tests", "fixtures", "leze_micro");
        var raster = FixtureReader.ReadRasterF32(dir, "dem30", out int width, out int height, out var transform);
        var zones = FixtureReader.ReadZones(dir, "grid250");
        var centroids = FixtureReader.ReadCentroids(dir, "grid250", "dem30");
        var opt = new Options { RasterMode = RasterMode.Compat };
        var mask = Rasterizer.Rasterize(zones.Xy, zones.RingStart, zones.RingIsHole, zones.RingZone, transform,
            width, height, opt);
        var stats = Zonal.Stats(raster, mask, width, height, zones.ZoneCount, -9999f, opt);
        var mean = Zonal.MeanWithFallback(raster, width, height, -9999f, stats, centroids.Rows, centroids.Cols);
        var expected = File.ReadLines(Path.Combine(RepoRoot(), "tests", "fixtures", "expected", "leze_micro",
                "mean_grid250_dem30.csv")).Skip(1)
            .Select(l => double.Parse(l.Split(',')[1], CultureInfo.InvariantCulture)).ToArray();
        Assert.Equal(expected.Length, mean.Length);
        for (int z = 0; z < expected.Length; z++)
            Assert.True(Math.Abs(mean[z] - expected[z]) <= 1e-9 * Math.Max(1.0, Math.Abs(expected[z])),
                $"zone {z}: {mean[z]} vs {expected[z]}");
    }
}

public class RasterizerValidationTests
{
    [Fact]
    public void EveryRingOffsetIsValidatedBeforeNativeCode()
    {
        double[] xy = { 0, 0, 2, 0, 2, 2, 0, 2, 5, 5, 6, 6 };  // six points
        byte[] holes = { 0, 0 };
        int[] zones = { 0, 1 };
        var t = new Transform(0, 10, 1, 1);
        // decreasing second offset: the native code would read point 4 before noticing
        Assert.Throws<ArgumentException>(() =>
            Rasterizer.Rasterize(xy, new long[] { 0, 4, 3 }, holes, zones, t, 10, 10));
        // offset beyond the coordinate array
        Assert.Throws<ArgumentException>(() =>
            Rasterizer.Rasterize(xy, new long[] { 0, 4, 7 }, holes, zones, t, 10, 10));
        // negative first offset
        Assert.Throws<ArgumentException>(() =>
            Rasterizer.Rasterize(xy, new long[] { -1, 4, 6 }, holes, zones, t, 10, 10));
        // the consistent layout still works
        var mask = Rasterizer.Rasterize(xy, new long[] { 0, 4, 6 }, holes, zones, t, 10, 10);
        Assert.Equal(100, mask.Length);
    }
}
