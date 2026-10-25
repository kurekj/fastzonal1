using FzBench;
using FzBench.Rungs;
using Xunit;

namespace FzBench.Tests;

public class FixtureTests
{
    private static string Write(string dir, string name, string dtype, byte[] bin, string nodata, int w, int h)
    {
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, $"raster_{name}.json"),
            $"{{\"dtype\": \"{dtype}\", \"width\": {w}, \"height\": {h}, \"transform\": [0.0, {h}.0, 1.0, 1.0], " +
            $"\"nodata\": {nodata}, \"crs\": \"none\"}}\n");
        File.WriteAllBytes(Path.Combine(dir, $"raster_{name}.bin"), bin);
        return dir;
    }

    [Fact]
    public void NoDataIsComparedInTheStorageType()
    {
        var dir = Path.Combine(Path.GetTempPath(), "fz_fixture_" + Guid.NewGuid().ToString("N"));
        // float32 0.1 is not double 0.1; a sample equal to float32(0.1) must still be NoData
        var bin = new byte[4 * 4];
        BitConverter.GetBytes(0.1f).CopyTo(bin, 0);
        BitConverter.GetBytes(2.0f).CopyTo(bin, 4);
        BitConverter.GetBytes(3.0f).CopyTo(bin, 8);
        BitConverter.GetBytes(4.0f).CopyTo(bin, 12);
        Write(dir, "a", "f32", bin, "0.1", 2, 2);
        var r = Fixture.LoadRaster(dir, "a");
        Assert.Equal(-9999.0, r.Pixels[0, 0]);
        Assert.Equal(2.0, r.Pixels[0, 1]);
        Directory.Delete(dir, true);
    }

    [Fact]
    public void WrongBinaryLengthIsRejected()
    {
        var dir = Path.Combine(Path.GetTempPath(), "fz_fixture_" + Guid.NewGuid().ToString("N"));
        Write(dir, "a", "f32", new byte[4 * 3], "null", 2, 2);  // 3 values for a 2x2 raster
        Assert.Throws<InvalidDataException>(() => Fixture.LoadRaster(dir, "a"));
        Directory.Delete(dir, true);
    }

    [Fact]
    public void LargeIntegerCodesAreRejectedForTheFloatCopy()
    {
        var dir = Path.Combine(Path.GetTempPath(), "fz_fixture_" + Guid.NewGuid().ToString("N"));
        var bin = new byte[4 * 4];
        BitConverter.GetBytes(16777217).CopyTo(bin, 0);  // 2^24 + 1 is not representable in float32
        Write(dir, "a", "i32", bin, "null", 2, 2);
        Assert.Throws<InvalidDataException>(() => Fixture.LoadRaster(dir, "a"));
        Directory.Delete(dir, true);
    }

    [Fact]
    public void RatioDenominatorIsPreparedOnceOutsideTheRung()
    {
        var root = RepoRoot();
        var f = Fixture.Load(Path.Combine(root, "tests", "fixtures", "synthetic_small"));
        var prepared = Fixture.PrepareDenominator(f.Rasters["synth"], f.Rasters["synth"]);
        Assert.Same(f.Rasters["synth"].Pixels, prepared.Pixels);  // same grid: no copy
        var rung = new BaselineRung();
        var mask = rung.Rasterize(f, "rect", "synth");
        var ratio = rung.Ratio(f, "rect", "synth", prepared, mask);
        Assert.Equal(50, ratio.Length);
        foreach (var v in ratio) Assert.True(v == 0.0 || Math.Abs(v - 1.0) < 1e-12);
        Assert.Equal(1, rung.EffectiveThreads(200));
    }

    private static string RepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null && !File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt"))) dir = dir.Parent;
        return dir!.FullName;
    }
}

public class FixtureNoDataOrderTests
{
    [Fact]
    public void IntegerNoDataIsRemappedBeforeTheExactnessCheck()
    {
        var dir = Path.Combine(Path.GetTempPath(), "fz_fixture_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "raster_a.json"),
            "{\"dtype\": \"i32\", \"width\": 2, \"height\": 2, \"transform\": [0.0, 2.0, 1.0, 1.0], \"nodata\": -2147483648}\n");
        var bin = new byte[16];
        BitConverter.GetBytes(1).CopyTo(bin, 0);
        BitConverter.GetBytes(2).CopyTo(bin, 4);
        BitConverter.GetBytes(int.MinValue).CopyTo(bin, 8);
        BitConverter.GetBytes(3).CopyTo(bin, 12);
        File.WriteAllBytes(Path.Combine(dir, "raster_a.bin"), bin);
        var r = Fixture.LoadRaster(dir, "a");
        Assert.Equal(-9999.0, r.Pixels[1, 0]);
        Assert.Equal(1.0, r.Pixels[0, 0]);
        Directory.Delete(dir, true);
    }
}
