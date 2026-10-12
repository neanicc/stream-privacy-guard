#!/usr/bin/env python3
"""Score mask plans against generated suites and eval/thresholds.toml (docs/eval.md).

    python3 eval/score.py --mode ci --suite text=eval-out/text                   # plans next to the suite
    python3 eval/score.py --mode ci --suite text=eval-out/text --plans text=DIR  # plans elsewhere
    python3 eval/score.py --mode ci --suite text=eval-out/text --reference noop  # a reference pipeline

Prints one line per metric limit, then exactly one of `THRESHOLDS: PASS` or
`THRESHOLDS: FAIL`. Exit status 0 on PASS, 1 on FAIL (including unscorable
input). Standard library only.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from spg_eval import scoring, suite  # noqa: E402
from spg_eval.contracts import ContractError  # noqa: E402


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--thresholds", type=Path, default=HERE / "thresholds.toml")
    parser.add_argument("--mode", choices=scoring.MODES, required=True)
    parser.add_argument("--suite", action="append", default=[], metavar="NAME=DIR",
                        help="a generated suite to score (repeatable)")
    parser.add_argument("--plans", action="append", default=[], metavar="NAME=DIR",
                        help="where that suite's <seq>/mask_plans.jsonl are (default: the suite directory)")
    parser.add_argument("--reference", choices=suite.REFERENCE_PIPELINES,
                        help="score a reference pipeline built from ground truth instead of plans")
    parser.add_argument("--out", type=Path, help="write report.txt and report.json here")
    args = parser.parse_args(argv)
    try:
        thresholds = scoring.load_thresholds(args.thresholds)
        plans = dict(p.split("=", 1) for p in args.plans)
        runs = {}
        for spec in args.suite:
            name, _, suite_dir = spec.partition("=")
            if args.reference:
                run = suite.reference_run(Path(suite_dir), args.reference)
            else:
                run = suite.load_run(Path(suite_dir), Path(plans[name]) if name in plans else None)
            if run.name != name:
                raise scoring.ScoreError(f"{suite_dir} holds suite {run.name!r}, not {name!r}")
            runs[name] = run
        report = scoring.evaluate(thresholds, runs, args.mode)
    except (scoring.ScoreError, ContractError, OSError, ValueError) as exc:
        print(f"score: error: {exc}", file=sys.stderr)
        print("THRESHOLDS: FAIL")
        return 1
    print("\n".join(report.lines))
    print(report.verdict)
    if args.out:
        scoring.write_report(args.out, report.lines, report.verdict, report.suites, mode=args.mode)
    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
