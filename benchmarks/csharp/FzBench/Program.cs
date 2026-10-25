using System.Diagnostics;
using System.Globalization;
using System.Text;
using FzBench.Rungs;

namespace FzBench;

public static class Program
{
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    public static int Main(string[] args)
    {
        if (args.Length == 0) return Usage();
        try
        {
            return args[0] switch
            {
                "expected" => Expected(args),
                "bench" => Bench(args),
                _ => Usage()
            };
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 2;
        }
    }

    private static int Usage()
    {
        Console.Error.WriteLine("usage:\n" +
            "  FzBench expected <fixtureDir> <outDir>\n" +
            "      writes <outDir>/<fixture>/<op>_<zones>_<raster>.csv for every zone set and raster\n" +
            "  FzBench bench <fixtureDir> <rung> <op> <zones> <raster> [--den <raster>] [--threads T] [--reps R] [--out results.csv]\n" +
            "      rungs: csharp-baseline; ops: rasterize, mean, majority, ratio");
        return 1;
    }

    public static IRung MakeRung(string name, int threads) => name switch
    {
        "csharp-baseline" => new BaselineRung(),
        _ => throw new ArgumentException($"unknown rung {name}")
    };

    private static int Expected(string[] args)
    {
        if (args.Length < 3) return Usage();
        var f = Fixture.Load(args[1]);
        var outDir = Path.Combine(args[2], f.Id);
        System.IO.Directory.CreateDirectory(outDir);
        var rung = new BaselineRung();
        foreach (var (zname, z) in f.Zones)
        {
            foreach (var (rname, r) in f.Rasters)
            {
                var mask = rung.Rasterize(f, zname, rname);
                var flat = rung.MaskToArray(mask);
                var counts = new long[z.ZoneCount];
                foreach (var v in flat) if (v >= 0) counts[v]++;
                WriteCsv(Path.Combine(outDir, $"rasterize_{zname}_{rname}.csv"), counts.Select(c => (double)c));
                WriteCsv(Path.Combine(outDir, $"mean_{zname}_{rname}.csv"), rung.Mean(f, zname, rname, mask));
                WriteCsv(Path.Combine(outDir, $"majority_{zname}_{rname}.csv"), rung.Majority(f, zname, rname, mask));
                Console.WriteLine($"{f.Id}: {zname} x {rname}: rasterize, mean, majority");
            }
            // ratio: the first raster over the second when the fixture has at least two rasters,
            // otherwise the raster over itself
            var names = f.Rasters.Keys.ToList();
            var num = names[0];
            var den = names.Count > 1 ? names[1] : names[0];
            var m = rung.Rasterize(f, zname, num);
            WriteCsv(Path.Combine(outDir, $"ratio_{zname}_{num}_{den}.csv"), rung.Ratio(f, zname, num, den, m));
            Console.WriteLine($"{f.Id}: {zname}: ratio {num}/{den}");
        }
        return 0;
    }

    private static void WriteCsv(string path, IEnumerable<double> values)
    {
        var sb = new StringBuilder("zone,value\n");
        int i = 0;
        foreach (var v in values) sb.Append(i++).Append(',').Append(v.ToString("R", Inv)).Append('\n');
        File.WriteAllText(path, sb.ToString(), new UTF8Encoding(false));
    }

    private static int Bench(string[] args)
    {
        if (args.Length < 6) return Usage();
        var fixtureDir = args[1];
        var rungName = args[2];
        var op = args[3];
        var zones = args[4];
        var raster = args[5];
        string den = raster;
        int threads = 1, reps = 5;
        string outPath = "results.csv";
        for (int i = 6; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--den": den = args[++i]; break;
                case "--threads": threads = int.Parse(args[++i], Inv); break;
                case "--reps": reps = int.Parse(args[++i], Inv); break;
                case "--out": outPath = args[++i]; break;
                default: throw new ArgumentException($"unknown option {args[i]}");
            }
        }
        var f = Fixture.Load(fixtureDir);
        var rung = MakeRung(rungName, threads);
        var mask = op == "rasterize" ? null : rung.Rasterize(f, zones, raster);
        var lines = new StringBuilder();
        for (int rep = 0; rep < reps; rep++)
        {
            double checksum;
            var sw = Stopwatch.StartNew();
            switch (op)
            {
                case "rasterize":
                {
                    var m = rung.Rasterize(f, zones, raster);
                    sw.Stop();
                    checksum = MaskChecksum(rung.MaskToArray(m));
                    break;
                }
                case "mean":
                {
                    var v = rung.Mean(f, zones, raster, mask!);
                    sw.Stop();
                    checksum = v.Sum();
                    break;
                }
                case "majority":
                {
                    var v = rung.Majority(f, zones, raster, mask!);
                    sw.Stop();
                    checksum = v.Sum();
                    break;
                }
                case "ratio":
                {
                    var v = rung.Ratio(f, zones, raster, den, mask!);
                    sw.Stop();
                    checksum = v.Sum();
                    break;
                }
                default:
                    throw new ArgumentException($"unknown op {op}");
            }
            var zcount = f.Zones[zones].ZoneCount;
            var r = f.Rasters[raster];
            lines.Append(string.Join(",", f.Id, op, rung.Name, threads.ToString(Inv), rep.ToString(Inv),
                sw.Elapsed.TotalSeconds.ToString("G17", Inv), checksum.ToString("G17", Inv),
                zones, raster, zcount.ToString(Inv), ((long)r.Width * r.Height).ToString(Inv))).Append('\n');
        }
        var header = "fixture,op,rung,threads,rep,seconds,checksum,zones,raster,n_zones,n_cells\n";
        if (!File.Exists(outPath)) File.WriteAllText(outPath, header, new UTF8Encoding(false));
        File.AppendAllText(outPath, lines.ToString(), new UTF8Encoding(false));
        Console.Write(lines.ToString());
        return 0;
    }

    public static double MaskChecksum(int[] mask)
    {
        long assigned = 0, sum = 0;
        foreach (var v in mask) if (v >= 0) { assigned++; sum += v; }
        return assigned + (double)sum;
    }
}
