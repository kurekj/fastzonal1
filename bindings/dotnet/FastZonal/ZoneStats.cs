using System.Runtime.InteropServices;

namespace FastZonal;

/// <summary>Accumulated statistics of one zone; the layout is that of fz_stats.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct ZoneStats
{
    public long Count;
    public double Sum;
    public double SumSq;
    public double Min;
    public double Max;

    /// <summary>Sum / Count, or 0 when the zone has no cell.</summary>
    public readonly double Mean => Count > 0 ? Sum / Count : 0.0;

    /// <summary>Population standard deviation from the raw moments, or 0 when Count &lt; 2.</summary>
    public readonly double Std
    {
        get
        {
            if (Count < 2) return 0.0;
            double n = Count;
            double mean = Sum / n;
            double variance = SumSq / n - mean * mean;
            return variance > 0.0 ? Math.Sqrt(variance) : 0.0;
        }
    }
}
