namespace FzBench;

/// <summary>One implementation on the benchmark ladder. A rung keeps the mask in whatever form
/// suits it (the baseline uses int[,], the library int[]); <see cref="MaskToArray"/> converts it
/// for checksums outside any timed region. Every method computes exactly the semantics of the
/// reference application for the given fixture raster and zone set.</summary>
public interface IRung
{
    string Name { get; }

    /// <summary>Rasterises the zone set onto the raster's grid and returns the rung-native mask.</summary>
    object Rasterize(Fixture f, string zones, string raster);

    /// <summary>Mean per zone with the centroid fallback.</summary>
    double[] Mean(Fixture f, string zones, string raster, object mask);

    /// <summary>Mode per zone, ties to the smallest value, centroid fallback.</summary>
    double[] Majority(Fixture f, string zones, string raster, object mask);

    /// <summary>Mean per zone of the cell-wise ratio num / den after the denominator has been put on
    /// the numerator's grid (nearest neighbour, done by <see cref="Fixture.ResampleNearest"/>).</summary>
    double[] Ratio(Fixture f, string zones, string num, string den, object mask);

    int[] MaskToArray(object mask);
}
