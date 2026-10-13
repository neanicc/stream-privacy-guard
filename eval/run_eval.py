#!/usr/bin/env python3
"""Eval driver called by `scripts/ci/run.sh eval [--local]` (docs/eval.md).

1. Generate the suites for the mode: ci = text (ci profile); local = text and
   faces (full profiles).
2. Sanity-check the scorer on the generated ground truth: the no-op reference
   pipeline must fail recall, blur-everything must fail the clean-frame
   budget, and the oracle must pass. Any other outcome is an error.
3. Run the real pipeline with `spg-cli eval` from the build directory on
   every generated suite and score its mask plans. In local mode also replay
   the text suite's sequences at the `[suite.realtime]` resolution through
   `spg-cli bench --realtime` (the realtime suite: wall clock, worker
   threads) and score its fail-closed rate and detection latency. Its
   late-frame count is printed on an `unscored:` line: no limit scores it
   until #68 adds one to thresholds.toml.

The text and faces suites run in simulated time with a fixed cycle, so they
are evidence about plan logic, not timing margin (docs/cli.md).

`--workers N` passes the scan worker count to every `spg-cli eval` and
`bench` call (simulated workers, or worker threads in the realtime suite).
Without it spg-cli uses the scheduler's default, as the plugin runs. The
value is printed on a `workers:` line and recorded in report.json.

A missing or failing spg-cli is an error and prints `THRESHOLDS: FAIL`, in
every mode: no run passes without the pipeline.

Writes <out>/report.txt and <out>/report.json (metrics only, no item text).
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from generate import DEFAULT_SEED, default_cache, generate_suite  # noqa: E402
from spg_eval import scoring, suite  # noqa: E402
from spg_eval.contracts import parse_mask_plan, read_jsonl  # noqa: E402

SUITES = {"ci": {"text": "ci"}, "local": {"text": "full", "faces": "full"}}
# The limits each reference pipeline must break (or, for the oracle, keep).
MUST_FAIL = {"noop": ("watchlist_pair_recall", "pii_pair_recall", "face_pair_recall"),
             "blur_all": ("clean_frame_blur_area",)}
# The generated suite the realtime suite replays.
REALTIME_SOURCE = "text"


def find_cli(build_dir: Path) -> Path:
    """spg-cli in a single-config (Ninja, Make) or multi-config (Visual Studio) build tree."""
    for candidate in (build_dir / "cli" / "spg-cli", build_dir / "cli" / "spg-cli.exe",
                      build_dir / "cli" / "Release" / "spg-cli.exe"):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"spg-cli not found under {build_dir / 'cli'}; build it with scripts/ci/run.sh build")


def _run(command: list[str]) -> None:
    # spg-cli prints counts and timings only; its output goes to the log as is.
    sys.stdout.flush()
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"spg-cli {command[1]} exited with {result.returncode}")


def _timing_line(name: str, timing: dict) -> str:
    stages = timing.get("stages", {})
    parts = [f"{stage} p50 {stages[stage]['p50_ms']:.1f} ms" for stage in ("cycle", "text_detect", "face_detect")
             if stage in stages]
    late = f", {timing['late_frames']} late" if timing.get("late_frames") else ""
    return (f"pipeline: {name} {timing['frames']} frames, {timing['fail_closed_frames']} fail-closed, "
            f"{timing['padding_frames']} padding{late}; " + ", ".join(parts))


def _late_frames_line(bench: dict) -> str:
    """The realtime late-frame count, always printed and labeled as not scored.

    thresholds.toml has no late-frame limit yet; adding one is a human change
    tracked in #68 (SPG-31). The delay of a late frame still counts in its
    detection latency and fail-closed frames, which are scored.
    """
    return (f"unscored: realtime late_frames {int(bench.get('late_frames', 0))} "
            f"(no limit in thresholds.toml yet, #68)")


def _workers_args(workers: int | None) -> list[str]:
    return [] if workers is None else ["--workers", str(workers)]


def _workers_line(workers: int | None) -> str:
    return f"workers: {workers if workers is not None else 'spg-cli default (the scheduler default)'}"


def run_pipeline(build_dir: Path, suite_dir: Path, plans_dir: Path, cache: Path,
                 workers: int | None = None) -> dict:
    """Run spg-cli on every sequence in suite_dir, writing <plans_dir>/<seq>/mask_plans.jsonl.

    `spg-cli eval` reads the suite's frames, policy.json and fps from
    suite.json and drives the scheduler offline (simulated time, fixed cycle
    length; docs/cli.md), with the models from `cache`. Returns its timing
    report.
    """
    timing = plans_dir / "timing.json"
    _run([str(find_cli(build_dir)), "eval", "--suite", str(suite_dir), "--plans", str(plans_dir),
          "--timing", str(timing), "--cache", str(cache), *_workers_args(workers)])
    return json.loads(timing.read_text(encoding="utf-8"))


def run_realtime(build_dir: Path, source_dir: Path, plans_dir: Path, spec: dict,
                 cache: Path, workers: int | None = None) -> tuple[scoring.SuiteRun, dict]:
    """Replay source_dir's sequences at spec's resolution and fps on the wall clock, with the models from `cache`.

    A frame the replay could not feed on time keeps its due timestamp in
    spg-cli, so the delay counts in its latency and fail-closed frames
    (docs/cli.md); the late count is printed with the timing line and on an
    unscored line (no limit yet, #68).

    Returns the realtime suite run (those sequences' ground truth, the
    realtime plans and the per-cycle latencies) and the bench report.
    """
    index = suite.read_index(source_dir)
    if index.get("fps") != spec.get("fps", index.get("fps")):
        raise scoring.ScoreError(f"realtime fps {spec.get('fps')} differs from the {index['suite']} suite's "
                                 f"{index.get('fps')} fps")
    report_path = plans_dir / "bench.json"
    command = [str(find_cli(build_dir)), "bench", "--realtime", "--suite", str(source_dir), "--plans", str(plans_dir),
               "--report", str(report_path), "--cache", str(cache), *_workers_args(workers)]
    if "resolution" in spec:
        command += ["--resolution", str(spec["resolution"])]
    _run(command)
    report = json.loads(report_path.read_text(encoding="utf-8"))
    truth = suite.load_truth(source_dir)
    sequences = []
    for entry in report["sequences"]:
        name = entry["name"]
        plans = read_jsonl(plans_dir / name / suite.PLANS, parse_mask_plan)
        sequences.append(scoring.Sequence(name, truth[name], plans))
    if not sequences:
        raise scoring.ScoreError("the realtime replay ran no sequence")
    return scoring.SuiteRun("realtime", sequences, list(report["latencies_ms"])), report


def sanity_check(thresholds: dict, name: str, suite_dir: Path, mode: str) -> list[str]:
    lines = []
    for pipeline in suite.REFERENCE_PIPELINES:
        report = scoring.evaluate(thresholds, {name: suite.reference_run(suite_dir, pipeline)}, mode)
        failed = scoring.failed_limits(report, name)
        if pipeline == "oracle":
            ok = not failed
            expectation = "passes every limit"
        else:
            must = [m for m in MUST_FAIL[pipeline] if m in report.suites[name]["metrics"]
                    and report.suites[name]["metrics"][m]["pass"] is not None]
            ok = bool(must) and all(m in failed for m in must)
            expectation = f"fails {', '.join(must) or 'nothing it should'}"
        lines.append(f"sanity: {name} {pipeline:<8} {'ok' if ok else 'WRONG'}: {expectation} "
                     f"(failed limits: {', '.join(failed) or 'none'})")
        if not ok:
            raise scoring.ScoreError(f"scorer sanity check failed for {name}/{pipeline}")
    return lines


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mode", choices=scoring.MODES, default="ci")
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--thresholds", type=Path, default=HERE / "thresholds.toml")
    parser.add_argument("--out", type=Path, default=Path("eval-out"))
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--cache", type=Path, default=default_cache())
    parser.add_argument("--workers", type=int, default=None,
                        help="scan workers for every spg-cli call (default: spg-cli's, as the plugin runs)")
    args = parser.parse_args(argv)
    if args.workers is not None and args.workers < 1:
        parser.error("--workers must be at least 1")

    lines: list[str] = [_workers_line(args.workers)]
    try:
        thresholds = scoring.load_thresholds(args.thresholds)
        suite_dirs = {}
        for name, profile in SUITES[args.mode].items():
            suite_dirs[name] = args.out / name
            index = generate_suite(name, suite_dirs[name], args.seed, profile, args.cache)
            frames = sum(s["frames"] for s in index["sequences"])
            lines.append(f"generated: {name} ({profile}) {len(index['sequences'])} sequences, "
                         f"{frames} frames, seed {args.seed}")
            lines += sanity_check(thresholds, name, suite_dirs[name], args.mode)
        print("\n".join(lines))
        runs = {}
        for name, suite_dir in suite_dirs.items():
            # Plans left by an earlier run must never be scored as this run's output.
            plans_dir = args.out / "plans" / name
            shutil.rmtree(plans_dir, ignore_errors=True)
            timing = run_pipeline(args.build_dir, suite_dir, plans_dir, args.cache, args.workers)
            lines.append(_timing_line(name, timing))
            runs[name] = suite.load_run(suite_dir, plans_dir)
        realtime = thresholds["suite"].get("realtime")
        if realtime is not None and args.mode in realtime["required_in"]:
            plans_dir = args.out / "plans" / "realtime"
            shutil.rmtree(plans_dir, ignore_errors=True)
            runs["realtime"], bench = run_realtime(args.build_dir, suite_dirs[REALTIME_SOURCE], plans_dir, realtime,
                                                   args.cache, args.workers)
            lines.append(_timing_line("realtime", bench))
            lines.append(_late_frames_line(bench))
        print("\n".join(line for line in lines if line.startswith(("pipeline:", "unscored:"))))
        report = scoring.evaluate(thresholds, runs, args.mode)
    except Exception as exc:  # noqa: BLE001 - any failure must end in a FAIL verdict
        print(f"run_eval: error: {exc}", file=sys.stderr)
        print("THRESHOLDS: FAIL")
        scoring.write_report(args.out, [*lines, f"error: {exc}"], "THRESHOLDS: FAIL", {}, mode=args.mode,
                             workers=args.workers)
        return 1
    print("\n".join(report.lines))
    print(report.verdict)
    scoring.write_report(args.out, [*lines, *report.lines], report.verdict, report.suites,
                         mode=args.mode, seed=args.seed, workers=args.workers)
    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
