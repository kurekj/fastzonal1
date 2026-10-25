using System.Globalization;
using FzBench;
using FzBench.Rungs;
using Xunit;

namespace FzBench.Tests;

public class BaselineTests
{
    private static string RepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null && !File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt"))) dir = dir.Parent;
        return dir?.FullName ?? throw new InvalidOperationException("repository root not found");
    }

    private static string FixtureDir(string id) => Path.Combine(RepoRoot(), "tests", "fixtures", id);

    [Fact]
    public void SyntheticMeanOfZoneZeroMatchesIndependentCheck()
    {
        var dir = FixtureDir("synthetic_small");
        var fixture = Fixture.Load(dir);
        var rung = new BaselineRung();
        var mask = rung.Rasterize(fixture, "rect", "synth");
        var mean = rung.Mean(fixture, "rect", "synth", mask);
        var expected = double.Parse(File.ReadAllText(Path.Combine(dir, "check_mean_zone0.txt")).Trim(),
            CultureInfo.InvariantCulture);
        Assert.Equal(expected, mean[0], 1e-9);
        Assert.Equal(50, mean.Length);
    }

    [Fact]
    public void LezeRasterizeAssignsPixelsWithinZoneRange()
    {
        var fixture = Fixture.Load(FixtureDir("leze_micro"));
        var rung = new BaselineRung();
        var mask = rung.MaskToArray(rung.Rasterize(fixture, "grid250", "dem30"));
        int assigned = 0;
        foreach (var v in mask)
        {
            if (v >= 0) assigned++;
            Assert.True(v >= -1 && v < fixture.Zones["grid250"].ZoneCount);
        }
        Assert.True(assigned > 0);
        Assert.Equal(fixture.Rasters["dem30"].Width * fixture.Rasters["dem30"].Height, mask.Length);
    }

    [Fact]
    public void MajorityOnSyntheticReturnsObservedValues()
    {
        var fixture = Fixture.Load(FixtureDir("synthetic_small"));
        var rung = new BaselineRung();
        var mask = rung.Rasterize(fixture, "rect", "synth");
        var majority = rung.Majority(fixture, "rect", "synth", mask);
        Assert.Equal(50, majority.Length);
        var raster = fixture.Rasters["synth"].Pixels;
        var observed = new HashSet<double>();
        foreach (var v in raster) observed.Add(v);
        foreach (var m in majority) Assert.Contains(m, observed);
    }

    [Fact]
    public void RatioOfRasterWithItselfIsOneWherePixelsExist()
    {
        var fixture = Fixture.Load(FixtureDir("synthetic_small"));
        var rung = new BaselineRung();
        var mask = rung.Rasterize(fixture, "rect", "synth");
        var den = Fixture.PrepareDenominator(fixture.Rasters["synth"], fixture.Rasters["synth"]);
        var ratio = rung.Ratio(fixture, "rect", "synth", den, mask);
        foreach (var r in ratio) Assert.True(r == 0.0 || Math.Abs(r - 1.0) < 1e-12);
    }
}
