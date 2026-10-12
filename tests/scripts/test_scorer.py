"""Tests for the eval scorer and its JSON contract reader (standard library only).
Run by ctest (scoring.scorer) or directly: python3 -I tests/scripts/test_scorer.py
"""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "eval"))

from spg_eval import contracts, scoring, suite  # noqa: E402
from spg_eval.contracts import Box, ContractError, GroundTruthFrame, Instance, MaskPlan, MaskRect  # noqa: E402

FIXTURES = ROOT / "tests" / "fixtures" / "contracts"
THRESHOLDS = ROOT / "eval" / "thresholds.toml"
SENTINEL = "SENTINEL-QX7-not-for-logs"


def gt(frame: int, *instances: Instance, w: int = 100, h: int = 100) -> GroundTruthFrame:
    return GroundTruthFrame(frame, frame * 1000, w, h, tuple(instances))


def inst(ident: str, kind: str, x: float, y: float, w: float = 10, h: float = 10, vis: float = 1.0) -> Instance:
    return Instance(ident, kind, Box(x, y, w, h), vis)


def plan(frame: int, *rects: tuple, fallback: bool = False) -> MaskPlan:
    return MaskPlan(frame * 1000, fallback, tuple(MaskRect(Box(*r), "blur") for r in rects))


class ContractTests(unittest.TestCase):
    def test_committed_fixtures_parse(self) -> None:
        truth = contracts.parse_ground_truth_frame((FIXTURES / "ground_truth_frame.json").read_bytes())
        self.assertEqual((truth.frame, truth.timestamp_ns, truth.width, truth.height), (12, 200000000, 1920, 1080))
        self.assertEqual([(i.id, i.kind) for i in truth.instances],
                         [("wl-0", "watchlist"), ("pii-3", "pii:card"), ("decoy-1", "decoy")])
        self.assertEqual(truth.instances[0].box, Box(100, 40.5, 220, 18))
        self.assertEqual(truth.instances[1].visibility, 0.5)
        mask = contracts.parse_mask_plan((FIXTURES / "mask_plan.json").read_bytes())
        self.assertEqual(mask, MaskPlan(200000000, False, (MaskRect(Box(92, 32.5, 236, 34), "blur"),)))

    def test_round_trip_is_stable(self) -> None:
        truth = contracts.parse_ground_truth_frame((FIXTURES / "ground_truth_frame.json").read_text())
        text = contracts.serialize_ground_truth_frame(truth)
        self.assertNotIn("\n", text)
        self.assertEqual(contracts.parse_ground_truth_frame(text), truth)
        self.assertEqual(contracts.serialize_ground_truth_frame(contracts.parse_ground_truth_frame(text)), text)
        p = plan(3, (1.5, 2, 3, 4), fallback=True)
        self.assertEqual(contracts.parse_mask_plan(contracts.serialize_mask_plan(p)), p)

    def test_floats_narrow_like_cpp(self) -> None:
        head = '{"schema":1,"space":"source","frame":0,"timestamp_ns":0,"width":64,"height":64,"instances":['
        tiny = contracts.parse_ground_truth_frame(
            head + '{"id":"a","kind":"face","box":{"x":0.1,"y":0,"w":1,"h":1},"visibility":1e-50}]}')
        self.assertEqual(tiny.instances[0].visibility, 0.0)  # float32 underflow, as in C++
        self.assertEqual(tiny.instances[0].box.x, 0.10000000149011612)

    def test_ground_truth_parsing_is_strict(self) -> None:
        # The same rejections as tests/unit/json_test.cpp, plus Python-specific traps.
        head = '{"schema":1,"space":"source","frame":0,"timestamp_ns":0,"width":64,"height":64,"instances":['
        box = '"box":{"x":0,"y":0,"w":1,"h":1}'
        bad = [
            "{}", '{"schema":2}', "[1]", '{"schema":1} trailing',
            '{"schema":1,"space":"readback","frame":0,"timestamp_ns":0,"width":64,"height":64,"instances":[]}',
            '{"schema":1,"space":"source","frame":-1,"timestamp_ns":0,"width":64,"height":64,"instances":[]}',
            '{"schema":1,"space":"source","frame":0,"timestamp_ns":0,"width":0,"height":64,"instances":[]}',
            '{"schema":1,"space":"source","frame":0,"width":64,"height":64,"instances":[]}',
            '{"schema":1,"space":"source","frame":1.0,"timestamp_ns":0,"width":64,"height":64,"instances":[]}',
            '{"schema":true,"space":"source","frame":0,"timestamp_ns":0,"width":64,"height":64,"instances":[]}',
            '{"schema":1,"space":"source","frame":0,"frame":0,"timestamp_ns":0,"width":64,"height":64,"instances":[]}',
            head + '{"id":"a","kind":"pii:fax",' + box + ',"visibility":1}]}',
            head + '{"id":"a","kind":"face","box":{"x":0,"y":0,"w":-1,"h":1},"visibility":1}]}',
            head + '{"id":"a","kind":"face","box":{"x":0,"y":0,"w":1},"visibility":1}]}',
            head + '{"id":"a","kind":"face",' + box + '}]}',
            head + '{"id":"a","kind":"face",' + box + ',"visibility":2}]}',
            head + '{"id":"a","kind":"face",' + box + ',"visibility":1.00000001}]}',
            head + '{"id":"a","kind":"face",' + box + ',"visibility":-1e-50}]}',
            head + '{"id":"a","kind":"face","box":{"x":0,"y":0,"w":-1e-50,"h":1},"visibility":1}]}',
            head + '{"id":"a","kind":"face","box":{"x":1e300,"y":0,"w":1,"h":1},"visibility":1}]}',
            head + '{"id":"a","kind":"face","box":{"x":0,"y":-1e39,"w":1,"h":1},"visibility":1}]}',
            head + '{"id":"a","kind":"face","box":{"x":NaN,"y":0,"w":1,"h":1},"visibility":1}]}',
            head + '{"id":"a","kind":"face","box":{"x":1e400,"y":0,"w":1,"h":1},"visibility":1}]}',
            # An integer too large for a float must be a contract error, not an OverflowError.
            head + '{"id":"a","kind":"face","box":{"x":' + "9" * 400 + ',"y":0,"w":1,"h":1},"visibility":1}]}',
            head + '{"id":"a","kind":"face",' + box + ',"visibility":-' + "9" * 400 + '}]}',
            head + '{"id":"","kind":"face",' + box + ',"visibility":1}]}',
            head + '{"id":"a","kind":"face",' + box + ',"visibility":1,"text":"x"}]}',
            head + '{"id":"a","kind":"face",' + box + ',"visibility":1},{"id":"a","kind":"face",' + box
            + ',"visibility":1}]}',
            b'{"schema":1,"space":"\xff"}',
        ]
        for text in bad:
            with self.subTest(text=text), self.assertRaises(ContractError):
                contracts.parse_ground_truth_frame(text)

    def test_mask_plan_parsing_is_strict(self) -> None:
        bad = [
            '{"schema":1,"space":"source","timestamp_ns":0,"fallback":0,"rects":[]}',
            '{"schema":1,"space":"source","timestamp_ns":0,"fallback":false,"rects":[{"box":{"x":0,"y":0,"w":1,"h":1},"style":"x"}]}',
            '{"schema":1,"space":"source","timestamp_ns":0,"fallback":false,"rects":[{"box":{"x":0,"y":0,"w":1e39,"h":1},"style":"blur"}]}',
            '{"schema":1,"space":"source","timestamp_ns":0.5,"fallback":false,"rects":[]}',
            '{"schema":1,"space":"source","timestamp_ns":0,"fallback":false}',
            '{"schema":1,"space":"source","timestamp_ns":0,"fallback":false,"rects":[{"box":{"x":0,"y":0,"w":'
            + "1" * 400 + ',"h":1},"style":"blur"}]}',
        ]
        for text in bad:
            with self.subTest(text=text), self.assertRaises(ContractError):
                contracts.parse_mask_plan(text)
        edge = contracts.parse_mask_plan('{"schema":1,"space":"source","timestamp_ns":0,"fallback":false,"rects":'
                                         '[{"box":{"x":-0.0,"y":0,"w":0,"h":3.4028234663852886e38},"style":"blur"}]}')
        self.assertEqual(edge.rects[0].box.h, contracts.FLOAT_MAX)

    def test_errors_never_quote_input(self) -> None:
        head = '{"schema":1,"space":"source","frame":0,"timestamp_ns":0,"width":64,"height":64,"instances":['
        for text in (head + '{"id":"a","' + SENTINEL + '":1}]}', '{"schema":1,"space":"' + SENTINEL + '"}',
                     head + '{"id":"a","kind":"' + SENTINEL + '"}]}', '{"' + SENTINEL, '{"schema":1,"' + SENTINEL
                     + '":1,"' + SENTINEL + '":2}'):
            with self.assertRaises(ContractError) as ctx:
                contracts.parse_ground_truth_frame(text)
            self.assertNotIn("SENTINEL", str(ctx.exception))


class MetricTests(unittest.TestCase):
    def test_union_area_counts_overlap_once(self) -> None:
        self.assertEqual(scoring.union_area([]), 0.0)
        self.assertEqual(scoring.union_area([(0, 0, 10, 10), (5, 5, 15, 15)]), 175.0)
        self.assertEqual(scoring.union_area([(0, 0, 10, 10), (2, 2, 4, 4)]), 100.0)
        self.assertEqual(scoring.union_area([(0, 0, 10, 10), (10, 0, 20, 10)]), 200.0)

    def test_coverage_uses_union_of_rects(self) -> None:
        # Two masks that each cover half the box cover all of it together.
        self.assertEqual(scoring.covered_fraction((0, 0, 10, 10), [(0, 0, 5, 10), (5, 0, 10, 10)]), 1.0)
        self.assertEqual(scoring.covered_fraction((0, 0, 10, 10), [(-5, -5, 5, 15)]), 0.5)

    def run_metrics(self, truth, plans, tau=0.9):
        return scoring.suite_metrics(scoring.SuiteRun("text", [scoring.Sequence("s", truth, plans)]), tau)

    def test_definitions(self) -> None:
        truth = [
            gt(0, inst("wl-1", "watchlist", 10, 10, vis=0.0), inst("decoy-2", "decoy", 50, 50)),
            gt(1, inst("wl-1", "watchlist", 10, 10, vis=0.5), inst("pii-3", "pii:card", 30, 30)),
            gt(2, inst("wl-1", "watchlist", 10, 10), inst("pii-3", "pii:card", 30, 30)),
            gt(3, inst("pii-3", "pii:card", 95, 95)),  # 5x5 of 10x10 on screen: clipped box
        ]
        plans = [plan(0), plan(1, (30, 30, 10, 10)), plan(2, (9, 9, 12, 12), (30, 30, 10, 8)),
                 plan(3, (95, 95, 5, 5))]
        m = self.run_metrics(truth, plans)
        self.assertEqual((m["watchlist_pair_recall"].num, m["watchlist_pair_recall"].den), (1, 2))
        # wl-1 first shows on frame 1 (visibility 0 on frame 0 is not visible) and is missed there.
        self.assertEqual(m["watchlist_first_frame_recall"].value, 0.0)
        # pii-3: covered on 1, 80% (< tau) on 2, clipped box fully covered on 3.
        self.assertEqual((m["pii_pair_recall"].num, m["pii_pair_recall"].den), (2, 3))
        self.assertEqual((m["pii:card_pair_recall"].num, m["pii:card_pair_recall"].den), (2, 3))
        self.assertEqual((m["leak_frame_rate"].num, m["leak_frame_rate"].den), (2, 3))
        # Frame 0 lists a sensitive item (invisible), so no frame here is clean.
        self.assertIsNone(m["clean_frame_blur_area"].value)
        self.assertEqual((m["decoy_blur_rate"].num, m["decoy_blur_rate"].den), (0, 1))
        self.assertIsNone(m["face_pair_recall"].value)

    def test_fallback_covers_everything_and_counts_as_full_blur(self) -> None:
        truth = [gt(0, inst("face-1", "face", 0, 0)), gt(1), gt(2)]
        m = self.run_metrics(truth, [plan(0, fallback=True), plan(1, fallback=True), plan(2, (0, 0, 50, 10))])
        self.assertEqual(m["face_pair_recall"].value, 1.0)
        self.assertEqual(m["fail_closed_frame_rate"].value, 2 / 3)
        self.assertAlmostEqual(m["clean_frame_blur_area"].value, (1.0 + 0.05) / 2)

    def test_latency_percentile_is_nearest_rank(self) -> None:
        run = scoring.SuiteRun("realtime", [], latencies_ms=[float(v) for v in range(1, 101)])
        self.assertEqual(scoring.suite_metrics(run, 0.9)["p95_detection_latency_ms"].value, 95.0)

    def test_unpaired_frames_are_errors(self) -> None:
        truth = [gt(0), gt(1)]
        for plans in ([plan(0)], [plan(0), plan(1), plan(2)], [plan(0), plan(0), plan(1)]):
            with self.subTest(n=len(plans)), self.assertRaises(scoring.ScoreError):
                self.run_metrics(truth, plans)


def synthetic_suite() -> list[GroundTruthFrame]:
    """Two sequences' worth of frames: sensitive items of every scored kind, and clean frames."""
    frames = []
    for f in range(10):
        frames.append(gt(f, inst("wl-1", "watchlist", 10, 10, vis=0.5 if f == 0 else 1.0),
                         inst("pii-2", "pii:email", 40, 40), inst("decoy-3", "decoy", 70, 70)))
    return frames


class VerdictTests(unittest.TestCase):
    def setUp(self) -> None:
        self.thresholds = scoring.load_thresholds(THRESHOLDS)
        truth = synthetic_suite()
        clean = [gt(f, inst("decoy-1", "decoy", 5, 5)) for f in range(10)]
        self.sequences = [("busy", truth), ("clean", clean)]

    def run_for(self, pipeline: str, name: str = "text") -> scoring.SuiteRun:
        return scoring.SuiteRun(name, [scoring.Sequence(n, t, suite.reference_plans(t, pipeline))
                                       for n, t in self.sequences])

    def test_real_thresholds_name_only_known_metrics(self) -> None:
        metrics = scoring.suite_metrics(self.run_for("noop"), 0.9)
        for name, spec in self.thresholds["suite"].items():
            for key in spec:
                if key not in scoring.SUITE_SETTINGS:
                    self.assertIn(key[:-4], metrics, f"suite.{name}.{key}")

    def test_noop_pipeline_fails_recall(self) -> None:
        report = scoring.evaluate(self.thresholds, {"text": self.run_for("noop")}, "ci")
        self.assertEqual(report.verdict, "THRESHOLDS: FAIL")
        failed = scoring.failed_limits(report, "text")
        for metric in ("watchlist_pair_recall", "watchlist_first_frame_recall", "pii_pair_recall", "leak_frame_rate"):
            self.assertIn(metric, failed)

    def test_blur_everything_fails_clean_frame_budget(self) -> None:
        report = scoring.evaluate(self.thresholds, {"text": self.run_for("blur_all")}, "ci")
        self.assertEqual(report.verdict, "THRESHOLDS: FAIL")
        self.assertIn("clean_frame_blur_area", scoring.failed_limits(report, "text"))
        self.assertNotIn("watchlist_pair_recall", scoring.failed_limits(report, "text"))

    def test_oracle_passes_in_ci_mode(self) -> None:
        report = scoring.evaluate(self.thresholds, {"text": self.run_for("oracle")}, "ci")
        self.assertEqual(report.verdict, "THRESHOLDS: PASS", report.lines)
        self.assertTrue(any(line.startswith("faces") and "skipped" in line for line in report.lines))

    def test_missing_required_suite_fails_local_mode(self) -> None:
        report = scoring.evaluate(self.thresholds, {"text": self.run_for("oracle")}, "local")
        self.assertEqual(report.verdict, "THRESHOLDS: FAIL")
        self.assertTrue(any(line.startswith("faces") and "FAIL" in line for line in report.lines))

    def test_metric_without_data_fails(self) -> None:
        # A faces run with no faces cannot pass face_pair_recall_min.
        report = scoring.evaluate(self.thresholds, {"faces": self.run_for("oracle", "faces")}, "ci")
        self.assertIn("face_pair_recall", scoring.failed_limits(report, "faces"))

    def test_bad_thresholds_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.toml"
            for body in ('schema = 1\ncoverage_tau = 0.9\n[suite.text]\nrequired_in = ["ci"]\nwatchlist_recal_min = 1\n',
                         'schema = 1\ncoverage_tau = 0.9\n[suite.text]\nrequired_in = ["ci"]\nwatchlist_pair_recall = 1\n',
                         'schema = 1\ncoverage_tau = 1.5\n[suite.text]\nrequired_in = ["ci"]\n',
                         'schema = 2\ncoverage_tau = 0.9\n[suite.text]\nrequired_in = ["ci"]\n',
                         # A typo in a suite that ci mode skips must fail too, not wait for a local run.
                         'schema = 1\ncoverage_tau = 0.9\n[suite.text]\nrequired_in = ["ci"]\n'
                         '[suite.faces]\nrequired_in = ["local"]\nface_pair_recal_min = 0.9\n'):
                path.write_text(body, encoding="utf-8")
                with self.subTest(body=body), self.assertRaises(scoring.ScoreError):
                    scoring.evaluate(scoring.load_thresholds(path), {"text": self.run_for("noop")}, "ci")


class SuiteLayoutTests(unittest.TestCase):
    def write_suite(self, root: Path, declared, truth: list[GroundTruthFrame]) -> Path:
        out = root / "text"
        (out / "seq-000").mkdir(parents=True)
        seq = {"name": "seq-000"} if declared is None else {"name": "seq-000", "frames": declared}
        (out / "suite.json").write_text(json.dumps({"schema": 1, "suite": "text", "sequences": [seq]}))
        (out / "seq-000" / suite.TRUTH).write_text(
            "".join(contracts.serialize_ground_truth_frame(f) + "\n" for f in truth))
        suite.write_plans(out / "seq-000" / suite.PLANS, suite.reference_plans(truth, "oracle"))
        return out

    def test_complete_suite_loads(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = self.write_suite(Path(tmp), 10, synthetic_suite())
            self.assertEqual(len(suite.load_run(out).sequences[0].truth), 10)

    def test_ground_truth_must_hold_every_declared_frame(self) -> None:
        # Shortened ground truth with equally shortened plans pairs cleanly, so
        # only the declared count can catch it.
        full = synthetic_suite()
        cases = {"short": (10, full[:4]), "empty": (10, []), "gap": (10, full[:5] + full[6:]),
                 "extra": (9, full), "no count": (None, full), "zero": (0, []), "bool": (True, full[:1])}
        for label, (declared, truth) in cases.items():
            with self.subTest(label), tempfile.TemporaryDirectory() as tmp:
                out = self.write_suite(Path(tmp), declared, truth)
                with self.assertRaises(scoring.ScoreError):
                    suite.load_run(out)
                with self.assertRaises(scoring.ScoreError):
                    suite.reference_run(out, "oracle")


class CommandLineTests(unittest.TestCase):
    def test_score_cli_prints_one_verdict(self) -> None:
        import importlib.util
        spec = importlib.util.spec_from_file_location("score_cli", ROOT / "eval" / "score.py")
        cli = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cli)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "text"
            (out / "seq-000").mkdir(parents=True)
            truth = synthetic_suite()
            (out / "suite.json").write_text(json.dumps(
                {"schema": 1, "suite": "text", "sequences": [{"name": "seq-000", "frames": len(truth)}]}))
            (out / "seq-000" / suite.TRUTH).write_text(
                "".join(contracts.serialize_ground_truth_frame(f) + "\n" for f in truth))
            for pipeline, code, verdict in (("noop", 1, "THRESHOLDS: FAIL"), ("oracle", 1, "THRESHOLDS: FAIL")):
                stdout = io.StringIO()
                with redirect_stdout(stdout):
                    self.assertEqual(cli.main(["--mode", "ci", "--suite", f"text={out}", "--reference", pipeline]),
                                     code)
                verdicts = [line for line in stdout.getvalue().splitlines() if line.startswith("THRESHOLDS:")]
                self.assertEqual(verdicts, [verdict])  # the oracle fails here: this suite has no clean frames
            suite.write_plans(out / "seq-000" / suite.PLANS, suite.reference_plans(truth, "noop"))
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                self.assertEqual(cli.main(["--mode", "ci", "--suite", f"text={out}", "--out", str(Path(tmp) / "r")]), 1)
            self.assertTrue((Path(tmp) / "r" / "report.json").is_file())
            (out / "seq-000" / suite.PLANS).write_text("not json\n")
            stdout, stderr = io.StringIO(), io.StringIO()
            with redirect_stdout(stdout), redirect_stderr(stderr):
                self.assertEqual(cli.main(["--mode", "ci", "--suite", f"text={out}"]), 1)
            self.assertEqual(stdout.getvalue().strip().splitlines()[-1], "THRESHOLDS: FAIL")


if __name__ == "__main__":
    unittest.main()
