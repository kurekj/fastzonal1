using System.Runtime.InteropServices;

namespace FastZonal;

/// <summary>Raw P/Invoke surface of the fastzonal C ABI (include/fastzonal/fastzonal.h). The typed
/// wrappers in <see cref="Zonal"/> and <see cref="Rasterizer"/> are the intended entry points.</summary>
public static unsafe class Native
{
    public const string LibraryName = "fastzonal";

    static Native()
    {
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, Resolve);
    }

    /// <summary>Resolution order: the FASTZONAL_NATIVE_PATH environment variable (a file or a
    /// directory), then runtimes/&lt;rid&gt;/native next to this assembly, then the default search.</summary>
    private static IntPtr Resolve(string name, System.Reflection.Assembly assembly, DllImportSearchPath? path)
    {
        if (name != LibraryName) return IntPtr.Zero;
        string file = OperatingSystem.IsWindows() ? "fastzonal.dll"
            : OperatingSystem.IsMacOS() ? "libfastzonal.dylib" : "libfastzonal.so";
        var env = Environment.GetEnvironmentVariable("FASTZONAL_NATIVE_PATH");
        if (!string.IsNullOrEmpty(env))
        {
            var candidate = Directory.Exists(env) ? Path.Combine(env, file) : env;
            if (File.Exists(candidate)) return NativeLibrary.Load(candidate);
        }
        var baseDir = AppContext.BaseDirectory;
        var rid = RuntimeInformation.RuntimeIdentifier;
        var arch = RuntimeInformation.ProcessArchitecture.ToString().ToLowerInvariant();
        var osPrefix = OperatingSystem.IsWindows() ? "win" : OperatingSystem.IsMacOS() ? "osx" : "linux";
        foreach (var candidate in new[]
                 {
                     Path.Combine(baseDir, "runtimes", rid, "native", file),
                     Path.Combine(baseDir, "runtimes", $"{osPrefix}-{arch}", "native", file),
                     Path.Combine(baseDir, file),
                 })
        {
            if (File.Exists(candidate)) return NativeLibrary.Load(candidate);
        }
        return IntPtr.Zero;  // fall back to the default probing
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct FzTransform { public double OriginX, OriginY, PixelW, PixelH; }

    [StructLayout(LayoutKind.Sequential)]
    public struct FzShape { public int Width, Height; }

    [StructLayout(LayoutKind.Sequential)]
    public struct FzOptions { public int Backend, Threads, Device, RasterMode, BandRows; }

    [DllImport(LibraryName, EntryPoint = "fz_version")] private static extern IntPtr fz_version();
    [DllImport(LibraryName, EntryPoint = "fz_has_openmp")] private static extern int fz_has_openmp();
    [DllImport(LibraryName, EntryPoint = "fz_has_cuda")] private static extern int fz_has_cuda();
    [DllImport(LibraryName, EntryPoint = "fz_error_string")] private static extern IntPtr fz_error_string(int code);
    [DllImport(LibraryName, EntryPoint = "fz_options_default")] public static extern void fz_options_default(FzOptions* opt);

    [DllImport(LibraryName, EntryPoint = "fz_rasterize")]
    public static extern int fz_rasterize(double* xy, long* ringStart, byte* ringIsHole, int* ringZone, long nRings,
        FzTransform t, FzShape s, int* maskOut, FzOptions* opt);

    [DllImport(LibraryName, EntryPoint = "fz_zonal_f32")]
    public static extern int fz_zonal_f32(float* raster, int* mask, FzShape s, int nZones, float nodata,
        ZoneStats* outStats, FzOptions* opt);

    [DllImport(LibraryName, EntryPoint = "fz_zonal_f64")]
    public static extern int fz_zonal_f64(double* raster, int* mask, FzShape s, int nZones, double nodata,
        ZoneStats* outStats, FzOptions* opt);

    [DllImport(LibraryName, EntryPoint = "fz_zonal_mean_fallback_f32")]
    public static extern int fz_zonal_mean_fallback_f32(float* raster, FzShape s, float nodata, ZoneStats* stats,
        int nZones, int* centroidRow, int* centroidCol, double* meanOut);

    [DllImport(LibraryName, EntryPoint = "fz_zonal_mean_fallback_f64")]
    public static extern int fz_zonal_mean_fallback_f64(double* raster, FzShape s, double nodata, ZoneStats* stats,
        int nZones, int* centroidRow, int* centroidCol, double* meanOut);

    [DllImport(LibraryName, EntryPoint = "fz_zonal_majority_f32")]
    public static extern int fz_zonal_majority_f32(float* raster, int* mask, FzShape s, int nZones, float nodata,
        int* centroidRow, int* centroidCol, double* valueOut, byte* hasValueOut, FzOptions* opt);

    [DllImport(LibraryName, EntryPoint = "fz_zonal_majority_f64")]
    public static extern int fz_zonal_majority_f64(double* raster, int* mask, FzShape s, int nZones, double nodata,
        int* centroidRow, int* centroidCol, double* valueOut, byte* hasValueOut, FzOptions* opt);

    [DllImport(LibraryName, EntryPoint = "fz_zonal_majority_i32")]
    public static extern int fz_zonal_majority_i32(int* raster, int* mask, FzShape s, int nZones, int nodata,
        int* centroidRow, int* centroidCol, double* valueOut, byte* hasValueOut, FzOptions* opt);

    [DllImport(LibraryName, EntryPoint = "fz_zonal_ratio_f32")]
    public static extern int fz_zonal_ratio_f32(float* num, float* den, int* mask, FzShape s, int nZones,
        float nodataNum, float nodataDen, double eps, ZoneStats* outStats, FzOptions* opt);

    [DllImport(LibraryName, EntryPoint = "fz_zonal_ratio_f64")]
    public static extern int fz_zonal_ratio_f64(double* num, double* den, int* mask, FzShape s, int nZones,
        double nodataNum, double nodataDen, double eps, ZoneStats* outStats, FzOptions* opt);

    public static string Version() => Marshal.PtrToStringAnsi(fz_version()) ?? "";
    public static bool HasOpenMp() => fz_has_openmp() != 0;
    public static bool HasCuda() => fz_has_cuda() != 0;
    public static string ErrorString(int code) => Marshal.PtrToStringAnsi(fz_error_string(code)) ?? "unknown error";

    /// <summary>Throws <see cref="FastZonalException"/> for any code other than FZ_OK.</summary>
    public static void Check(int code)
    {
        if (code != 0) throw new FastZonalException(code, ErrorString(code));
    }
}
