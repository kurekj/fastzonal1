namespace FzBench;

/// <summary>One implementation on the benchmark ladder. A rung keeps the mask in whatever form
/// suits it (the baseline uses int[,], the library int[]); <see cref="MaskToArray"/> converts it
/// for checksums outside any timed region. Every method computes exactly the semantics of the
/// reference application for the given fixture raster and zone set.</summary>
public interface IRung
{
    string Name { get; }

    /// <summary>Workers the rung can actually use on a raster of this height (the library and the
    /// optimised rung never use more workers than bands; the baseline is always 1).</summary>
    int EffectiveThreads(int height);

    /// <summary>Rasterises the zone set onto the raster's grid and returns the rung-native mask.</summary>
    object Rasterize(Fixture f, string zones, string raster);

    /// <summary>Mean per zone with the centroid fallback.</summary>
    double[] Mean(Fixture f, string zones, string raster, object mask);

    /// <summary>Mode per zone, ties to the smallest value, centroid fallback.</summary>
    double[] Majority(Fixture f, string zones, string raster, object mask);

    /// <summary>Mean per zone of the cell-wise ratio num / den. <paramref name="denOnGrid"/> is already on
    /// the numerator's grid (<see cref="Fixture.PrepareDenominator"/>, done once by the driver).</summary>
    double[] Ratio(Fixture f, string zones, string num, RasterData denOnGrid, object mask);

    int[] MaskToArray(object mask);
}
