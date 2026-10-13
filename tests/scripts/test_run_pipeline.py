"""Tests for the eval driver's pipeline wiring (eval/run_eval.py, SPG-11), standard library only.

The suites and spg-cli are replaced by fakes: suites are written from ground
truth alone and the fake spg-cli writes oracle (or empty) plans, so these
tests check the driver's control flow, not the pipeline. spg-cli itself is
tested by spg-cli-tests (ctest cli.*).
Run by ctest (scoring.driver) or directly: python3 -I tests/scripts/test_run_pipeline.py
"""
from __future__ import annotations

import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "eval"))

from spg_eval import contracts, suite  # noqa: E402
from spg_eval.contracts import Box, GroundTruthFrame, Instance  # noqa: E402

THRESHOLDS = ROOT / "eval" / "thresholds.toml"


def load_driver():
    spec = importlib.util.spec_from_file_location("run_eval_under_test", ROOT / "eval" / "run_eval.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def gt(frame: int, *instances: Instance) -> GroundTruthFrame:
    return GroundTruthFrame(frame, frame * 1_000_000_000 // 60, 1920, 1080, tuple(instances))


def inst(ident: str, kind: str, x: float) -> Instance:
    return Instance(ident, kind, Box(x, 10, 40, 20), 1.0)


def fake_generate_suite(name: str, out: Path, seed: int, profile: str, cache: Path) -> dict:
    """A busy and a clean sequence of 10 frames each, ground truth only."""
    if name == "text":
        busy = [gt(f, inst("wl-1", "watchlist", 10), inst("pii-2", "pii:card", 100), inst("d-3", "decoy", 300))
                for f in range(10)]
        clean = [gt(f, inst("d-1", "decoy", 300)) for f in range(10)]
    else:
        busy = [gt(f, inst("face-1", "face", 10)) for f in range(10)]
        clean = [gt(f) for f in range(10)]
    sequences = []
    for seq, truth in (("busy", busy), ("clean", clean)):
        (out / seq).mkdir(parents=True, exist_ok=True)
        (out / seq / suite.TRUTH).write_text("".join(contracts.serialize_ground_truth_frame(f) + "\n" for f in truth))
        sequences.append({"name": seq, "frames": len(truth), "width": 1920, "height": 1080})
    index = {"schema": 1, "suite": name, "seed": seed, "profile": profile, "fps": 60, "policy": "policy.json",
             "sequences": sequences}
    (out / suite.INDEX).write_text(json.dumps(index))
    return index


class FakeCli:
    """Stands in for spg-cli eval / bench --realtime: writes plans and a timing report."""

    def __init__(self, pipeline: str = "oracle", latency_ms: float = 120.0, exit_code: int = 0,
                 late_frames: int = 0) -> None:
        self.pipeline = pipeline
        self.latency_ms = latency_ms
        self.exit_code = exit_code
        self.late_frames = late_frames
        self.commands: list[list[str]] = []

    def __call__(self, command: list[str]) -> None:
        self.commands.append(command)
        if self.exit_code != 0:
            raise RuntimeError(f"spg-cli {command[1]} exited with {self.exit_code}")
        arg = dict(zip(command[2::1], command[3::1]))
        suite_dir, plans = Path(arg["--suite"]), Path(arg["--plans"])
        truth = suite.load_truth(suite_dir)
        frames = 0
        for name, frames_truth in truth.items():
            suite.write_plans(plans / name / suite.PLANS, suite.reference_plans(frames_truth, self.pipeline))
            frames += len(frames_truth)
        report = {"frames": frames, "fail_closed_frames": 0, "padding_frames": 30,
                  "stages": {"cycle": {"p50_ms": 150.0}, "text_detect": {"p50_ms": 120.0}},
                  "sequences": [{"name": name} for name in truth],
                  "latencies_ms": [self.latency_ms] * 20}
        if command[1] == "bench":
            report["late_frames"] = self.late_frames
        key = "--timing" if command[1] == "eval" else "--report"
        Path(arg[key]).write_text(json.dumps(report))


class DriverTests(unittest.TestCase):
    def run_driver(self, mode: str, cli: FakeCli | None, find_cli=None,
                   extra: tuple[str, ...] = ()) -> tuple[int, str, dict]:
        driver = load_driver()
        driver.generate_suite = fake_generate_suite
        if cli is not None:
            driver._run = cli
            driver.find_cli = lambda build_dir: Path("spg-cli")
        elif find_cli is not None:
            driver.find_cli = find_cli
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            stdout, stderr = io.StringIO(), io.StringIO()
            with redirect_stdout(stdout), redirect_stderr(stderr):
                code = driver.main(["--mode", mode, "--build-dir", str(Path(tmp) / "build"), "--out", str(out),
                                    "--thresholds", str(THRESHOLDS), "--cache", str(Path(tmp) / "cache"),
                                    *extra])
            report = json.loads((out / "report.json").read_text())
            return code, stdout.getvalue(), report

    def verdicts(self, stdout: str) -> list[str]:
        return [line for line in stdout.splitlines() if line.startswith("THRESHOLDS:")]

    def test_missing_spg_cli_fails_in_ci_mode(self) -> None:
        # No build: there is no "not run" outcome any more.
        code, stdout, report = self.run_driver("ci", None)
        self.assertEqual(code, 1)
        self.assertEqual(self.verdicts(stdout), ["THRESHOLDS: FAIL"])
        self.assertNotIn("NOT RUN", stdout)
        self.assertEqual(report["verdict"], "THRESHOLDS: FAIL")

    def test_failing_spg_cli_fails(self) -> None:
        code, stdout, _ = self.run_driver("ci", FakeCli(exit_code=1))
        self.assertEqual(code, 1)
        self.assertEqual(self.verdicts(stdout), ["THRESHOLDS: FAIL"])

    def test_ci_mode_scores_the_pipeline_plans(self) -> None:
        cli = FakeCli("oracle")
        code, stdout, report = self.run_driver("ci", cli)
        self.assertEqual(self.verdicts(stdout), ["THRESHOLDS: PASS"], stdout)
        self.assertEqual(code, 0)
        self.assertEqual([c[1] for c in cli.commands], ["eval"])
        self.assertIn("pipeline: text 20 frames", stdout)
        self.assertTrue(report["suites"]["text"]["run"])
        self.assertFalse(report["suites"]["realtime"]["run"])

    def test_ci_mode_fails_a_pipeline_that_blurs_nothing(self) -> None:
        code, stdout, report = self.run_driver("ci", FakeCli("noop"))
        self.assertEqual(code, 1)
        self.assertEqual(self.verdicts(stdout), ["THRESHOLDS: FAIL"])
        self.assertFalse(report["suites"]["text"]["metrics"]["watchlist_pair_recall"]["pass"])

    def test_local_mode_runs_the_realtime_suite(self) -> None:
        cli = FakeCli("oracle", latency_ms=120.0)
        code, stdout, report = self.run_driver("local", cli)
        self.assertEqual(self.verdicts(stdout), ["THRESHOLDS: PASS"], stdout)
        self.assertEqual(code, 0)
        self.assertEqual([c[1] for c in cli.commands], ["eval", "eval", "bench"])
        bench = cli.commands[-1]
        self.assertIn("--realtime", bench)
        self.assertEqual(bench[bench.index("--resolution") + 1], "1920x1080")
        self.assertIn("/text", bench[bench.index("--suite") + 1].replace("\\", "/"))
        realtime = report["suites"]["realtime"]
        self.assertTrue(realtime["run"])
        self.assertEqual(realtime["metrics"]["p95_detection_latency_ms"]["value"], 120.0)
        self.assertEqual(realtime["metrics"]["fail_closed_frame_rate"]["value"], 0.0)

    def test_every_cli_call_uses_the_chosen_cache(self) -> None:
        # Models fetched into a custom cache must be the ones spg-cli opens.
        cli = FakeCli("oracle")
        self.run_driver("local", cli)
        self.assertEqual([c[1] for c in cli.commands], ["eval", "eval", "bench"])
        for command in cli.commands:
            with self.subTest(command[1]):
                self.assertIn("--cache", command)
                self.assertEqual(Path(command[command.index("--cache") + 1]).name, "cache")

    def test_workers_reach_every_cli_call_and_the_report(self) -> None:
        cli = FakeCli("oracle")
        code, stdout, report = self.run_driver("local", cli, extra=("--workers", "3"))
        self.assertEqual(code, 0, stdout)
        self.assertEqual([c[1] for c in cli.commands], ["eval", "eval", "bench"])
        for command in cli.commands:
            with self.subTest(command[1]):
                self.assertEqual(command[command.index("--workers") + 1], "3")
        self.assertIn("workers: 3", stdout.splitlines())
        self.assertEqual(report["workers"], 3)

    def test_without_workers_spg_cli_keeps_its_default(self) -> None:
        cli = FakeCli("oracle")
        _, stdout, report = self.run_driver("local", cli)
        for command in cli.commands:
            with self.subTest(command[1]):
                self.assertNotIn("--workers", command)
        self.assertIn("workers: spg-cli default (the scheduler default)", stdout.splitlines())
        self.assertIsNone(report["workers"])

    def test_workers_below_one_is_a_usage_error(self) -> None:
        driver = load_driver()
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            driver.main(["--workers", "0"])
        self.assertEqual(raised.exception.code, 2)

    def test_realtime_line_reports_late_frames(self) -> None:
        cli = FakeCli("oracle", late_frames=3)
        _, stdout, _ = self.run_driver("local", cli)
        self.assertIn("pipeline: realtime 20 frames, 0 fail-closed, 30 padding, 3 late;", stdout)

    def test_realtime_late_frames_are_labeled_unscored(self) -> None:
        # No late-frame limit exists yet (#68); the count must still be visible,
        # including zero, and say that nothing scores it.
        for late in (0, 3):
            with self.subTest(late=late):
                _, stdout, _ = self.run_driver("local", FakeCli("oracle", late_frames=late))
                self.assertIn(f"unscored: realtime late_frames {late} (no limit in thresholds.toml yet, #68)",
                              stdout.splitlines())

    def test_ci_mode_prints_no_late_frame_line(self) -> None:
        _, stdout, _ = self.run_driver("ci", FakeCli("oracle"))
        self.assertNotIn("unscored:", stdout)

    def test_local_mode_fails_slow_detection(self) -> None:
        code, stdout, report = self.run_driver("local", FakeCli("oracle", latency_ms=900.0))
        self.assertEqual(code, 1)
        self.assertEqual(self.verdicts(stdout), ["THRESHOLDS: FAIL"])
        self.assertFalse(report["suites"]["realtime"]["metrics"]["p95_detection_latency_ms"]["pass"])

    def test_find_cli_layouts(self) -> None:
        driver = load_driver()
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            with self.assertRaises(FileNotFoundError):
                driver.find_cli(build)
            for rel in ("cli/spg-cli", "cli/Release/spg-cli.exe"):
                with self.subTest(rel):
                    path = build / rel
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text("")
                    self.assertEqual(driver.find_cli(build), path)
                    path.unlink()


if __name__ == "__main__":
    unittest.main()
