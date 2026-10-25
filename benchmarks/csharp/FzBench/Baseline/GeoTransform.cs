// Verbatim extract of the author's SpongeGIS code, SPONGEGIS2026.NET commit 8028b95f (2026-09-26),
// relicensed MIT by the author. Source: WebSPONGEGIS/Services/IndicatorCalculationService.cs,
// the GeoTransform record at the end of the file. Only the namespace was added.
namespace FzBench.Baseline;

public record struct GeoTransform(double OriginX, double OriginY, double ScaleX, double ScaleY)
{
    /// Returns the world coordinate of the pixel center (matching R's raster::extract behavior).
    public readonly (double X, double Y) PixelToWorld(int row, int col)
        => (OriginX + (col + 0.5) * ScaleX, OriginY - (row + 0.5) * ScaleY);

    public readonly int WorldToCol(double x)
        => (int)((x - OriginX) / ScaleX);

    public readonly int WorldToRow(double y)
        => (int)((OriginY - y) / ScaleY);
}
