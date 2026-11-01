"""Focused regressions for stable-window selection and attribution claims."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ANALYZER = Path(__file__).with_name("analyze_rss.py")


def row(minute, rss=100, worker=100, mappings=100):
    return {"epochMs": 1000000 + minute * 60000, "clientCount": 1, "workerCount": 1,
            "mappings": mappings,
            "clients": [{"role": "publisher", "rssMiB": rss, "privateMiB": rss}],
            "workers": [{"role": "publisher-worker", "rssMiB": worker}]}


class RssTests(unittest.TestCase):
    def analyze(self, rows):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            (path / "resources.json").write_text(json.dumps(rows))
            result = subprocess.run([sys.executable, "-B", str(ANALYZER), "--run", d,
                                     "--expected-processes", "2", "--out", str(path / "out.json")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads((path / "out.json").read_text())

    def test_warmup_mapping_jump_is_not_residual(self):
        rows = [row(m, 100 if m == 0 else 120, mappings=100 if m == 0 else 120)
                for m in range(30)]
        result = self.analyze(rows)
        totals = result["clientTotalsRssMiB"]
        self.assertEqual(totals["largestWarmupMinuteJump"]["deltaMiB"], 20)
        self.assertIsNone(totals["largestStableMinuteJump"])
        self.assertFalse(result["residualStep"]["observedAtLeast10MiB"])
        self.assertEqual(result["verdict"], "bounded-but-unlocated")

    def test_exit_jump_is_excluded(self):
        result = self.analyze([row(m, 100 if m < 29 else 200) for m in range(30)])
        self.assertEqual(result["clientTotalsRssMiB"]["largestExitMinuteJump"]["deltaMiB"], 100)
        self.assertIsNone(result["correlation"]["jump"])

    def test_boundary_jumps_need_both_endpoints_inside(self):
        result = self.analyze([row(m, 100 if m < 3 else 150 if m <= 27 else 250)
                               for m in range(30)])
        self.assertIsNone(result["correlation"]["jump"])
        self.assertEqual([j["deltaMiB"] for j in result["clientTotalsRssMiB"]["excludedBoundaryJumps"]], [50, 100])

    def test_correlated_stable_jump_is_unlocated(self):
        result = self.analyze([row(m, 100 if m < 18 else 120,
                                   100 if m < 18 else 130, 100 if m < 18 else 120)
                               for m in range(30)])
        self.assertEqual(result["correlation"]["jump"]["fromMinute"], 17)
        self.assertEqual(result["correlation"]["workerDeltaAtJump"], 30)
        self.assertEqual(result["correlation"]["mappingDeltaAtJump"], 20)
        self.assertTrue(result["residualStep"]["observedAtLeast10MiB"])
        self.assertEqual(result["residualStep"]["allocationSite"], "unlocated")
        self.assertEqual(result["residualStep"]["independentReproduction"], "not-assessed-by-single-run-analyzer")
        self.assertEqual(result["verdict"], "bounded-but-unlocated")

    def test_incomplete_fleet_samples_do_not_create_stable_jump(self):
        rows = [row(m) for m in range(30)]
        partial = row(18, 1000)
        partial["workerCount"] = 0
        partial["workers"] = []
        rows.insert(19, partial)
        result = self.analyze(rows)
        self.assertIsNone(result["correlation"]["jump"])
        self.assertEqual(result["clientTotalsRssMiB"]["stableDeltaMiB"], 0)
        self.assertEqual(result["completeSampleCount"], 30)

    def test_missing_minute_does_not_bridge_or_claim_bounded(self):
        rows = [row(m, 100 if m < 18 else 120) for m in range(30) if m != 17]
        result = self.analyze(rows)
        self.assertIsNone(result["correlation"]["jump"])
        self.assertIsNone(result["windowObservation"]["boundedByInheritedCriterion"])
        self.assertEqual(result["verdict"], "insufficient-stable-coverage")

    def test_growing_window_does_not_claim_bounded(self):
        result = self.analyze([row(m, 100 + 2 * m) for m in range(30)])
        self.assertFalse(result["windowObservation"]["boundedByInheritedCriterion"])
        self.assertEqual(result["verdict"], "not-window-bounded-unlocated")

    def test_initial_incomplete_sample_does_not_rebase_process_minutes(self):
        rows = [row(m, 100 + m) for m in range(30)]
        rows[0]["workerCount"] = 0
        rows[0]["workers"] = []
        result = self.analyze(rows)
        self.assertEqual(result["perProcess"]["publisher"]["stableFirstRssMiB"], 103)
        self.assertEqual(result["clientTotalsRssMiB"]["stableFirstMiB"], 103)

    def test_decreasing_window_has_no_positive_jump(self):
        result = self.analyze([row(m, 200 - m) for m in range(30)])
        self.assertIsNone(result["correlation"]["jump"])


if __name__ == "__main__":
    unittest.main()
