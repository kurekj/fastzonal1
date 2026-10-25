import json
import subprocess
import sys
from pathlib import Path

BENCH = Path(__file__).resolve().parents[1]

RAW = """fixture,op,rung,threads,rep,seconds,checksum,zones,raster,den,n_zones,n_cells
biebrza_upper,mean,csharp-baseline,1,0,0.100,10.0,grid500,dem30,,4178,2250412
biebrza_upper,mean,csharp-baseline,1,1,0.120,10.0,grid500,dem30,,4178,2250412
biebrza_upper,mean,csharp-baseline,1,2,0.110,10.0,grid500,dem30,,4178,2250412
biebrza_upper,mean,csharp-optimised,1,0,0.050,10.0,grid500,dem30,,4178,2250412
biebrza_upper,mean,csharp-optimised,1,1,0.060,10.0,grid500,dem30,,4178,2250412
biebrza_upper,mean,csharp-optimised,8,0,0.010,10.0,grid500,dem30,,4178,2250412
biebrza_upper,mean,csharp-optimised,8,1,0.012,10.0,grid500,dem30,,4178,2250412
biebrza_upper,mean,fastzonal-openmp,1,0,0.020,10.0000001,grid500,dem30,,4178,2250412
biebrza_upper,mean,fastzonal-openmp,1,1,0.022,10.0000001,grid500,dem30,,4178,2250412
biebrza_upper,mean,fastzonal-openmp,8,0,0.004,10.0000001,grid500,dem30,,4178,2250412
biebrza_upper,mean,fastzonal-openmp,8,1,0.005,10.0000001,grid500,dem30,,4178,2250412
biebrza_upper,mean,fastzonal-serial,1,0,0.021,10.0000001,grid500,dem30,,4178,2250412
biebrza_upper,mean,csharp-baseline,1,0,0.400,20.0,grid100,dem30,,100043,2250412
biebrza_upper,mean,csharp-baseline,1,1,0.420,20.0,grid100,dem30,,100043,2250412
biebrza_upper,mean,fastzonal-openmp,8,0,0.010,20.0000001,grid100,dem30,,100043,2250412
biebrza_upper,mean,fastzonal-openmp,8,1,0.011,20.0000001,grid100,dem30,,100043,2250412
biebrza_upper,rasterize,csharp-baseline,1,0,1.000,5000.0,grid100,dem30,,100043,2250412
biebrza_upper,rasterize,fastzonal-openmp,8,0,0.100,5000.0,grid100,dem30,,100043,2250412
biebrza_upper,ratio,csharp-baseline,1,0,0.300,30.0,grid500,dem30,dem60,4178,2250412
biebrza_upper,ratio,fastzonal-openmp,8,0,0.030,30.0000002,grid500,dem30,dem60,4178,2250412
"""


def test_make_paper_assets_from_raw_csv(tmp_path):
    raw = tmp_path / "raw_test.csv"
    raw.write_text(RAW)
    out = tmp_path / "paper_assets"
    subprocess.run([sys.executable, str(BENCH / "make_paper_assets.py"), "--raw", str(raw), "--out", str(out)],
                   check=True)
    for name in ("tables/scaling_zones.tex", "tables/speedup_threads.tex", "tables/ladder.tex",
                 "figures/scaling_zones.png", "figures/speedup_threads.png", "summary.json"):
        assert (out / name).exists(), name
    summary = json.loads((out / "summary.json").read_text())
    pt = summary["points"]
    key = "biebrza_upper|mean|csharp-baseline|1|grid500|dem30|"
    assert key in pt
    assert pt[key]["median"] == 0.11
    assert pt[key]["min"] == 0.10 and pt[key]["max"] == 0.12 and pt[key]["n"] == 3
    # speedup of the best fastzonal-openmp point over the baseline for the same workload
    assert abs(summary["speedup"]["biebrza_upper|mean|grid500|dem30|"]["fastzonal-openmp"]["8"] - 0.11 / 0.0045) < 1e-9
    ladder = (out / "tables/ladder.tex").read_text()
    assert "csharp-baseline" in ladder and "fastzonal-openmp" in ladder and "\\toprule" in ladder
    assert "checksum" in summary and summary["checksum"]["mismatches"] == []


def test_checksum_mismatch_is_reported(tmp_path):
    raw = tmp_path / "raw_bad.csv"
    raw.write_text(RAW.replace("fastzonal-serial,1,0,0.021,10.0000001", "fastzonal-serial,1,0,0.021,11.0"))
    out = tmp_path / "paper_assets"
    subprocess.run([sys.executable, str(BENCH / "make_paper_assets.py"), "--raw", str(raw), "--out", str(out)],
                   check=True)
    summary = json.loads((out / "summary.json").read_text())
    assert summary["checksum"]["mismatches"] == [
        {"workload": "biebrza_upper|mean|grid500|dem30|", "rung": "fastzonal-serial", "threads": 1,
         "checksum": 11.0, "baseline": 10.0}]
