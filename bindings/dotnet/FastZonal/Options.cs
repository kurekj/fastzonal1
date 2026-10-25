namespace FastZonal;

public enum Backend { Auto = 0, Serial = 1, OpenMp = 2, Cuda = 3 }

public enum RasterMode
{
    /// <summary>Pixel-centre rule on rows and columns, holes honoured.</summary>
    Center = 0,
    /// <summary>Reproduces the SpongeGIS reference rasteriser (exterior rings only).</summary>
    Compat = 1
}

/// <summary>North-up affine grid: pixel (row, col) has its centre at
/// x = OriginX + (col + 0.5) * PixelW, y = OriginY - (row + 0.5) * PixelH.</summary>
public readonly record struct Transform(double OriginX, double OriginY, double PixelW, double PixelH)
{
    internal Native.FzTransform ToNative() => new() { OriginX = OriginX, OriginY = OriginY, PixelW = PixelW, PixelH = PixelH };
}

/// <summary>Execution options; the defaults mirror fz_options_default.</summary>
public sealed class Options
{
    public Backend Backend { get; init; } = Backend.Auto;
    /// <summary>0 = all hardware threads (capped by the number of bands).</summary>
    public int Threads { get; init; } = 0;
    public int Device { get; init; } = 0;
    public RasterMode RasterMode { get; init; } = RasterMode.Center;
    /// <summary>Rows per band; fixes the floating-point summation order.</summary>
    public int BandRows { get; init; } = 64;

    public static readonly Options Default = new();

    internal Native.FzOptions ToNative() => new()
    {
        Backend = (int)Backend, Threads = Threads, Device = Device, RasterMode = (int)RasterMode, BandRows = BandRows
    };
}

public sealed class FastZonalException : Exception
{
    public int Code { get; }

    public FastZonalException(int code, string message) : base(message)
    {
        Code = code;
    }
}
