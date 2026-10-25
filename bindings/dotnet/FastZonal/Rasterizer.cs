namespace FastZonal;

/// <summary>Polygon rasterisation to a zone-index mask (fz_rasterize).</summary>
public static unsafe class Rasterizer
{
    /// <summary>Rasterises polygons given as flat ring arrays (see the C header for the layout) and
    /// returns a row-major mask of width * height zone indices, -1 outside every polygon.</summary>
    public static int[] Rasterize(ReadOnlySpan<double> xy, ReadOnlySpan<long> ringStart, ReadOnlySpan<byte> ringIsHole,
        ReadOnlySpan<int> ringZone, Transform t, int width, int height, Options? options = null)
    {
        var mask = new int[checked((long)width * height)];
        Rasterize(xy, ringStart, ringIsHole, ringZone, t, width, height, mask, options);
        return mask;
    }

    /// <summary>As above, writing into a caller-provided mask of exactly width * height entries.</summary>
    public static void Rasterize(ReadOnlySpan<double> xy, ReadOnlySpan<long> ringStart, ReadOnlySpan<byte> ringIsHole,
        ReadOnlySpan<int> ringZone, Transform t, int width, int height, Span<int> maskOut, Options? options = null)
    {
        long nRings = ringZone.Length;
        if (ringIsHole.Length != nRings || ringStart.Length != nRings + 1)
            throw new ArgumentException("ringStart must have nRings + 1 entries and ringIsHole nRings entries");
        if (nRings > 0)
        {
            long nPoints = xy.Length / 2;
            if (ringStart[0] < 0) throw new ArgumentException("ring offsets must be non-negative");
            for (int i = 0; i < nRings; i++)
                if (ringStart[i + 1] < ringStart[i] || ringStart[i + 1] > nPoints)
                    throw new ArgumentException("ring offsets must be non-decreasing and inside the coordinate array");
        }
        if (width <= 0 || height <= 0 || maskOut.Length != (long)width * height)
            throw new ArgumentException("maskOut must have width * height entries");
        var opt = (options ?? Options.Default).ToNative();
        fixed (double* pxy = xy)
        fixed (long* prs = ringStart)
        fixed (byte* ph = ringIsHole)
        fixed (int* pz = ringZone)
        fixed (int* pm = maskOut)
        {
            Native.Check(Native.fz_rasterize(pxy, prs, ph, pz, nRings, t.ToNative(),
                new Native.FzShape { Width = width, Height = height }, pm, &opt));
        }
    }
}
