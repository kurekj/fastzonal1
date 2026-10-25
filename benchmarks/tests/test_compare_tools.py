import csv
import sys
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "benchmarks"))

import compare_tools as ct  # noqa: E402


def test_parse_cases_and_mean_comparison():
    cases = ct.parse_cases("a:grid1000:dem30, b:grid100:dem30")
    assert [c.key for c in cases] == ["a:grid1000:dem30", "b:grid100:dem30"]
    with pytest.raises(ValueError):
        ct.parse_cases("a:b")
    tool = np.array([1.0, np.nan, 3.0, 4.5])
    ref = np.array([1.0, 2.0, np.nan, 4.0])
    assert ct.compare_means(tool, ref) == {"max_abs_diff": 0.5, "n_compared": 2, "n_only_tool": 1, "n_only_fastzonal": 1}
    assert ct.compare_masks(np.array([[0, 1], [-1, 2]]), np.array([[0, 1], [-1, 3]])) == {"n_diff_cells": 1}
    stats = np.zeros(3, dtype=[("count", "<i8"), ("sum", "<f8")])
    stats["count"] = [2, 0, 4]
    stats["sum"] = [3.0, 0.0, 10.0]
    assert np.array_equal(np.isnan(ct.means_from_stats(stats)), [False, True, False])
    assert ct.means_from_stats(stats)[2] == 2.5


def _fake_case():
    raster = np.arange(12, dtype=np.float32).reshape(3, 4)
    return ct.CaseData(ct.Case("fx", "z", "r"), raster, float("nan"), (0.0, 3.0, 1.0, 1.0), np.empty(2, dtype=object), 2)


def test_run_case_warms_up_compares_every_repetition_and_resumes(tmp_path):
    calls = []

    def fz_tool(cd, threads):
        calls.append(("fastzonal", threads))
        return ct.ToolResult(np.array([1.0, 2.0]), 0.01 / threads, 0.005 / threads)

    def other(cd, threads):
        calls.append(("other", threads))
        return ct.ToolResult(np.array([1.0, 2.25]), 0.5, 0.4, "other note")

    drift = iter([np.array([1.0, 2.0]), np.array([1.0, 2.0]), np.array([1.0, 2.0]), np.array([1.0, 9.0])])

    def drifting(cd, threads):  # warm-up and first two repetitions agree, the third does not
        calls.append(("drifting", threads))
        return ct.ToolResult(next(drift), 0.1, 0.1)

    def slow(cd, threads):
        calls.append(("slow", threads))
        return ct.ToolResult(np.array([np.nan, 2.0]), 10.0, 10.0)

    stages = {"pipeline": {"fastzonal": fz_tool, "other": other, "drifting": drifting, "slow": slow}}
    out = tmp_path / "tools_test.csv"
    ct.run_case(_fake_case(), stages, threads=[1, 4], reps=3, max_seconds=5.0, out=out, done=set(), log=lambda s: None)
    with open(out, newline="") as f:
        rows = list(csv.DictReader(f))
    assert list(rows[0].keys()) == ct.COLUMNS
    by = {}
    for r in rows:
        by.setdefault((r["tool"], int(r["threads"])), []).append(r)
    assert len(by[("fastzonal", 1)]) == 3 and len(by[("fastzonal", 4)]) == 3 and len(by[("other", 1)]) == 3
    assert len(by[("slow", 1)]) == 1  # exceeded max_seconds after the first repetition
    # one untimed warm-up call per tool and thread count precedes the timed repetitions
    assert calls.count(("fastzonal", 1)) == 4 and calls.count(("other", 1)) == 4 and calls.count(("slow", 1)) == 2
    assert by[("other", 1)][0]["max_abs_diff"] == "0.25" and by[("other", 1)][0]["note"] == "other note"
    assert by[("slow", 1)][0]["n_only_fastzonal"] == "1" and by[("slow", 1)][0]["n_compared"] == "1"
    assert by[("fastzonal", 4)][0]["max_abs_diff"] == "0.0"
    assert [r["max_abs_diff"] for r in by[("drifting", 1)]] == ["0.0", "0.0", "7.0"]
    n_calls = len(calls)
    # a second run with the same output skips the whole stage: nothing is called, nothing appended
    ct.run_case(_fake_case(), stages, threads=[1, 4], reps=3, max_seconds=5.0, out=out, done=ct.done_keys(out),
                log=lambda s: None)
    assert len(calls) == n_calls
    with open(out, newline="") as f:
        assert len(list(csv.DictReader(f))) == len(rows)
    # resuming with one comparator missing recomputes only the reference (untimed) and that comparator
    done = {k for k in ct.done_keys(out) if k[4] != "other"}
    ct.run_case(_fake_case(), {"pipeline": {"fastzonal": fz_tool, "other": other}}, threads=[1, 4], reps=3,
                max_seconds=5.0, out=out, done=done, log=lambda s: None)
    assert calls[n_calls:] == [("fastzonal", 1)] + [("other", 1)] * 4


def test_summary_tables(tmp_path):
    rows = []
    for tool, secs, thr, diffs in (("fastzonal", [0.02, 0.03, 0.01], 1, ["0.0"] * 3),
                                   ("fastzonal", [0.004, 0.005, 0.006], 8, ["0.0"] * 3),
                                   ("rasterstats", [2.0, 3.0, 2.5], 1, ["1e-6", "3e-6", "2e-6"]),
                                   ("exactextract", [1.0], 1, ["1e-6"])):
        for i, (s, d) in enumerate(zip(secs, diffs)):
            rows.append(dict(fixture="fx_a", zones="grid", raster="dem", n_zones="10", n_cells="100", stage="pipeline",
                             tool=tool, threads=str(thr), rep=str(i), seconds=str(s), compute_seconds=str(s / 2),
                             max_abs_diff=d, n_compared="10", n_only_tool="0", n_only_fastzonal="0",
                             n_diff_cells="", note=""))
    summary = ct.summarise(rows)
    g = {(r["tool"], r["threads"]): r for r in summary["groups"]}
    assert g[("fastzonal", 1)]["median_seconds"] == 0.02 and g[("rasterstats", 1)]["median_seconds"] == 2.5
    assert g[("exactextract", 1)]["reps"] == 1
    assert g[("rasterstats", 1)]["max_abs_diff"] == 3e-6  # the worst repetition, not the first
    ct.write_tables(summary, tmp_path, fast_threads=8)
    tex = (tmp_path / "tools_pipeline.tex").read_text()
    assert "fx a & grid & dem & 10 & 2.50 & 1.00 & 0.020 & 0.005 & 500.0 & 200.0" in tex
    assert (tmp_path / "tools_stages.tex").exists()
    assert ct._tex("a_b & 5%") == r"a\_b \& 5\%"
