import json
import subprocess
import sys
from pathlib import Path

BENCH = Path(__file__).resolve().parents[1]

HEADER = "fixture,op,rung,threads,rep,seconds,checksum,zones,raster,den,n_zones,n_cells,threads_eff,band_rows\n"
RAW = HEADER + """biebrza_upper,mean,csharp-baseline,1,0,0.100,10.0,grid500,dem30,,4178,2250412,1,16
biebrza_upper,mean,csharp-baseline,1,1,0.120,10.0,grid500,dem30,,4178,2250412,1,16
biebrza_upper,mean,csharp-baseline,1,2,0.110,10.0,grid500,dem30,,4178,2250412,1,16
biebrza_upper,mean,csharp-optimised,1,0,0.050,10.0,grid500,dem30,,4178,2250412,1,16
biebrza_upper,mean,csharp-optimised,1,1,0.060,10.0,grid500,dem30,,4178,2250412,1,16
biebrza_upper,mean,csharp-optimised,8,0,0.010,10.0,grid500,dem30,,4178,2250412,8,16
biebrza_upper,mean,csharp-optimised,8,1,0.012,10.0,grid500,dem30,,4178,2250412,8,16
biebrza_upper,mean,fastzonal-openmp,1,0,0.020,10.0000001,grid500,dem30,,4178,2250412,1,16
biebrza_upper,mean,fastzonal-openmp,1,1,0.022,10.0000001,grid500,dem30,,4178,2250412,1,16
biebrza_upper,mean,fastzonal-openmp,8,0,0.004,10.0000001,grid500,dem30,,4178,2250412,8,16
biebrza_upper,mean,fastzonal-openmp,8,1,0.005,10.0000001,grid500,dem30,,4178,2250412,8,16
biebrza_upper,mean,fastzonal-openmp,112,0,0.004,10.0000001,grid500,dem30,,4178,2250412,88,16
biebrza_upper,mean,fastzonal-serial,1,0,0.021,10.0000001,grid500,dem30,,4178,2250412,1,16
biebrza_upper,mean,csharp-baseline,1,0,0.400,20.0,grid100,dem30,,100043,2250412,1,16
biebrza_upper,mean,csharp-baseline,1,1,0.420,20.0,grid100,dem30,,100043,2250412,1,16
biebrza_upper,mean,fastzonal-openmp,8,0,0.010,20.0000001,grid100,dem30,,100043,2250412,8,16
biebrza_upper,mean,fastzonal-openmp,8,1,0.011,20.0000001,grid100,dem30,,100043,2250412,8,16
biebrza_upper,rasterize,csharp-baseline,1,0,1.000,5000.0,grid100,dem30,,100043,2250412,1,16
biebrza_upper,rasterize,fastzonal-openmp,8,0,0.100,5000.0,grid100,dem30,,100043,2250412,8,16
biebrza_upper,ratio,csharp-baseline,1,0,0.300,30.0,grid500,dem30,dem60,4178,2250412,1,16
biebrza_upper,ratio,fastzonal-openmp,8,0,0.030,30.0000002,grid500,dem30,dem60,4178,2250412,8,16
gradascica,mean,fastzonal-openmp,8,0,0.030,7.0,grid100,dem30,,18619,408682,8,16
"""


def run(raw_paths, out, extra=()):
    return subprocess.run([sys.executable, str(BENCH / "make_paper_assets.py"), "--raw", *map(str, raw_paths),
                           "--out", str(out), *extra], check=True, capture_output=True, text=True)


def test_make_paper_assets_from_raw_csv(tmp_path):
    raw = tmp_path / "raw_test.csv"
    raw.write_text(RAW)
    out = tmp_path / "paper_assets"
    run([raw], out)
    for name in ("tables/scaling_zones.tex", "tables/speedup_threads.tex", "tables/ladder.tex",
                 "figures/scaling_zones.png", "figures/speedup_threads.png", "summary.json"):
        assert (out / name).exists(), name
    summary = json.loads((out / "summary.json").read_text())
    pt = summary["points"]
    key = "biebrza_upper|mean|csharp-baseline|1|grid500|dem30|"
    assert key in pt
    assert pt[key]["median"] == 0.11
    assert pt[key]["min"] == 0.10 and pt[key]["max"] == 0.12 and pt[key]["n"] == 3
    assert pt["biebrza_upper|mean|fastzonal-openmp|112|grid500|dem30|"]["threads_eff"] == 88
    assert abs(summary["speedup"]["biebrza_upper|mean|grid500|dem30|"]["fastzonal-openmp"]["8"] - 0.11 / 0.0045) < 1e-9
    ladder = (out / "tables/ladder.tex").read_text()
    assert "csharp-baseline" in ladder and "fastzonal-openmp" in ladder and "\\toprule" in ladder
    assert summary["checksum"]["mismatches"] == []
    assert summary["checksum"]["workloads_without_baseline"] == ["gradascica|mean|grid100|dem30|"]


def test_checksum_mismatch_in_any_repetition_is_reported(tmp_path):
    raw = tmp_path / "raw_bad.csv"
    raw.write_text(RAW.replace("fastzonal-openmp,8,1,0.005,10.0000001", "fastzonal-openmp,8,1,0.005,11.0")
                   .replace("fastzonal-serial,1,0,0.021,10.0000001", "fastzonal-serial,1,0,0.021,nan"))
    out = tmp_path / "paper_assets"
    run([raw], out)
    summary = json.loads((out / "summary.json").read_text())
    mm = summary["checksum"]["mismatches"]
    assert {"workload": "biebrza_upper|mean|grid500|dem30|", "rung": "fastzonal-openmp", "threads": 8, "rep": 1,
            "checksum": 11.0, "baseline": 10.0} in mm
    nan_entries = [m for m in mm if m["rung"] == "fastzonal-serial"]
    assert len(nan_entries) == 1 and nan_entries[0]["checksum"] != nan_entries[0]["checksum"]  # NaN


def test_pooling_raw_files_requires_matching_provenance(tmp_path):
    (tmp_path / "raw_a.csv").write_text(RAW)
    (tmp_path / "raw_b.csv").write_text(RAW)
    env = {"git_commit": "abc", "cpu": "x", "band_rows": 16, "fixtures_manifest_sha256": "m"}
    (tmp_path / "env_a.json").write_text(json.dumps(env))
    (tmp_path / "env_b.json").write_text(json.dumps({**env, "git_commit": "def"}))
    proc = subprocess.run([sys.executable, str(BENCH / "make_paper_assets.py"), "--raw", str(tmp_path / "raw_a.csv"),
                           str(tmp_path / "raw_b.csv"), "--out", str(tmp_path / "o")], capture_output=True, text=True)
    assert proc.returncode != 0 and "git_commit differs" in proc.stderr
    run([tmp_path / "raw_a.csv", tmp_path / "raw_b.csv"], tmp_path / "o2", ["--allow-mixed"])
    summary = json.loads((tmp_path / "o2" / "summary.json").read_text())
    assert summary["mixed_runs_allowed"] is True
    assert summary["points"]["biebrza_upper|mean|csharp-baseline|1|grid500|dem30|"]["n"] == 6
