using System.Globalization;
using FzBench;
using FzBench.Rungs;
using Xunit;

namespace FzBench.Tests;

/// <summary>Every rung must reproduce the baseline's expected outputs on the committed fixtures:
/// pixel counts exactly, mean and ratio within 1e-6 relative (the library rungs use float32
/// rasters), majority exactly.</summary>
public class RungParityTests
{
    private static string RepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null && !File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt"))) dir = dir.Parent;
        return dir!.FullName;
    }

    private static double[] Expected(string fixture, string file) =>
        File.ReadLines(Path.Combine(RepoRoot(), "tests", "fixtures", "expected", fixture, file)).Skip(1)
            .Select(l => double.Parse(l.Split(',')[1], CultureInfo.InvariantCulture)).ToArray();

    private static void AssertClose(double[] got, double[] want, double rel, string what)
    {
        Assert.Equal(want.Length, got.Length);
        for (int z = 0; z < want.Length; z++)
            Assert.True(Math.Abs(got[z] - want[z]) <= rel * Math.Max(1.0, Math.Abs(want[z])),
                $"{what}: zone {z} got {got[z]} want {want[z]}");
    }

    public static IEnumerable<object[]> Cases()
    {
        foreach (var rung in new[] { "csharp-optimised", "fastzonal-dotnet", "fastzonal-dotnet-serial" })
        {
            yield return new object[] { rung, "synthetic_small", "rect", "synth", "synth" };
            yield return new object[] { rung, "leze_micro", "grid250", "dem30", "dem30" };
        }
    }

    [Theory]
    [MemberData(nameof(Cases))]
    public void RungMatchesBaselineExpected(string rungName, string fixtureId, string zones, string raster, string den)
    {
        if (rungName.StartsWith("fastzonal") && !NativeAvailable()) return;  // Windows dev box without the DLL
        var f = Fixture.Load(Path.Combine(RepoRoot(), "tests", "fixtures", fixtureId));
        var rung = Program.MakeRung(rungName, threads: 3);
        var mask = rung.Rasterize(f, zones, raster);
        var flat = rung.MaskToArray(mask);
        var counts = new double[f.Zones[zones].ZoneCount];
        foreach (var v in flat) if (v >= 0) counts[v] += 1;
        AssertClose(counts, Expected(fixtureId, $"rasterize_{zones}_{raster}.csv"), 0.0, "rasterize");
        AssertClose(rung.Mean(f, zones, raster, mask), Expected(fixtureId, $"mean_{zones}_{raster}.csv"), 1e-6, "mean");
        AssertClose(rung.Majority(f, zones, raster, mask), Expected(fixtureId, $"majority_{zones}_{raster}.csv"), 0.0,
            "majority");
        var denOnGrid = Fixture.PrepareDenominator(f.Rasters[raster], f.Rasters[den]);
        AssertClose(rung.Ratio(f, zones, raster, denOnGrid, mask), Expected(fixtureId, $"ratio_{zones}_{raster}_{den}.csv"),
            1e-6, "ratio");
        Assert.True(rung.EffectiveThreads(f.Rasters[raster].Height) >= 1);
    }

    [Fact]
    public void EffectiveThreadsAreCappedByTheBandCount()
    {
        var rung = Program.MakeRung("csharp-optimised", threads: 2);
        Assert.Equal(2, rung.EffectiveThreads(200));
        var capped = Program.MakeRung("fastzonal-dotnet", threads: 112, bandRows: 16);
        Assert.Equal(13, capped.EffectiveThreads(200));  // 200 rows / 16 = 13 bands
        Assert.Equal(1, Program.MakeRung("fastzonal-dotnet-serial", threads: 112).EffectiveThreads(200));
    }

    private static bool NativeAvailable()
    {
        try
        {
            _ = global::FastZonal.Native.Version();
            return true;
        }
        catch (DllNotFoundException)
        {
            return false;
        }
    }
}
