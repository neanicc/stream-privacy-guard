#!/usr/bin/env python3
"""Eval driver called by `scripts/ci/run.sh eval [--local]` (docs/eval.md).

1. Generate the suites for the mode: ci = text (ci profile); local = text and
   faces (full profiles). The realtime suite comes from `spg-cli bench
   --realtime` (SPG-11/SPG-16) and is reported as not run until then.
2. Sanity-check the scorer on the generated ground truth: the no-op reference
   pipeline must fail recall, blur-everything must fail the clean-frame
   budget, and the oracle must pass. Any other outcome is an error.
3. Run the real pipeline and score it. Until SPG-11 provides `spg-cli run`,
   run_pipeline() raises PipelineUnavailable and the driver prints
   `THRESHOLDS: NOT RUN`, which passes in ci mode and fails in local mode,
   as scripts/ci/run.sh did before this driver existed.

Writes <out>/report.txt and <out>/report.json (metrics only, no item text).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from generate import DEFAULT_SEED, default_cache, generate_suite  # noqa: E402
from spg_eval import scoring, suite  # noqa: E402

SUITES = {"ci": {"text": "ci"}, "local": {"text": "full", "faces": "full"}}
# The limits each reference pipeline must break (or, for the oracle, keep).
MUST_FAIL = {"noop": ("watchlist_pair_recall", "pii_pair_recall", "face_pair_recall"),
             "blur_all": ("clean_frame_blur_area",)}


class PipelineUnavailable(RuntimeError):
    pass


def run_pipeline(build_dir: Path, suite_dir: Path, plans_dir: Path) -> None:
    """Run the pipeline on every sequence in suite_dir, writing <plans_dir>/<seq>/mask_plans.jsonl.

    SPG-11 replaces this body with `spg-cli run` from build_dir, fed each
    sequence's frames directory, the suite's policy.json and the fps in suite.json.
    """
    raise PipelineUnavailable("spg-cli run is not implemented yet (SPG-11)")


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
    args = parser.parse_args(argv)

    lines: list[str] = []
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
        try:
            for name, suite_dir in suite_dirs.items():
                run_pipeline(args.build_dir, suite_dir, args.out / "plans" / name)
        except PipelineUnavailable as exc:
            verdict = f"THRESHOLDS: NOT RUN ({exc})"
            print(verdict)
            scoring.write_report(args.out, lines, verdict, {}, mode=args.mode, seed=args.seed)
            # CI stays green before the pipeline exists; local acceptance must not pass.
            return 0 if args.mode == "ci" else 1
        runs = {name: suite.load_run(d, args.out / "plans" / name) for name, d in suite_dirs.items()}
        report = scoring.evaluate(thresholds, runs, args.mode)
    except Exception as exc:  # noqa: BLE001 - any failure must end in a FAIL verdict
        print(f"run_eval: error: {exc}", file=sys.stderr)
        print("THRESHOLDS: FAIL")
        scoring.write_report(args.out, [*lines, f"error: {exc}"], "THRESHOLDS: FAIL", {}, mode=args.mode)
        return 1
    print("\n".join(report.lines))
    print(report.verdict)
    scoring.write_report(args.out, [*lines, *report.lines], report.verdict, report.suites,
                         mode=args.mode, seed=args.seed)
    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
