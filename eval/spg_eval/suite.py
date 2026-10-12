"""On-disk layout of a generated suite and the reference pipelines.

    <suite>/suite.json                  index: suite name, seed, profile, fps, sequences
    <suite>/policy.json                 schema-1 policy config with the synthetic watch-list
    <suite>/<seq>/frames/000000.png     RGB frames in Source pixels
    <suite>/<seq>/ground_truth.jsonl    one ground-truth document per frame
    <plans>/<seq>/mask_plans.jsonl      one mask plan per frame, written by the pipeline

The reference pipelines produce mask plans straight from ground truth. They
are the scorer's sanity checks: "noop" must fail recall, "blur_all" must fail
the clean-frame budget, and "oracle" (exact boxes of visible sensitive items)
must pass, which shows the thresholds are reachable.
"""
from __future__ import annotations

import json
from pathlib import Path

from .contracts import (Box, GroundTruthFrame, MaskPlan, MaskRect, parse_ground_truth_frame, parse_mask_plan,
                        read_jsonl, serialize_mask_plan)
from .scoring import ScoreError, Sequence, SuiteRun

INDEX = "suite.json"
TRUTH = "ground_truth.jsonl"
PLANS = "mask_plans.jsonl"
REFERENCE_PIPELINES = ("noop", "blur_all", "oracle")


def read_index(suite_dir: Path) -> dict:
    index = json.loads((suite_dir / INDEX).read_text(encoding="utf-8"))
    if index.get("schema") != 1 or not isinstance(index.get("sequences"), list):
        raise ValueError(f"{suite_dir / INDEX}: expected schema 1 with a sequence list")
    return index


def load_truth(suite_dir: Path) -> dict[str, list[GroundTruthFrame]]:
    """Each sequence's ground truth, which must hold exactly the frames 0..n-1 that suite.json declares.

    Plans are paired with ground truth frame by frame, so a shortened ground
    truth file would let a matching shortened plan file pass unnoticed.
    """
    truth = {}
    for seq in read_index(suite_dir)["sequences"]:
        name, declared = seq["name"], seq.get("frames")
        if not isinstance(declared, int) or isinstance(declared, bool) or declared < 1:
            raise ScoreError(f"{suite_dir / INDEX}: sequence {name} must declare a positive frame count")
        frames = read_jsonl(suite_dir / name / TRUTH, parse_ground_truth_frame)
        if sorted(f.frame for f in frames) != list(range(declared)):
            raise ScoreError(f"{name}: ground truth holds {len(frames)} frame(s), "
                             f"suite.json declares frames 0..{declared - 1}")
        truth[name] = frames
    return truth


def load_run(suite_dir: Path, plans_dir: Path | None = None) -> SuiteRun:
    """Pair a suite's ground truth with the mask plans under `plans_dir` (default: the suite)."""
    index = read_index(suite_dir)
    plans_dir = plans_dir or suite_dir
    sequences = []
    for name, truth in load_truth(suite_dir).items():
        path = plans_dir / name / PLANS
        if not path.is_file():
            raise FileNotFoundError(f"no mask plans for sequence {name}: {path}")
        sequences.append(Sequence(name, truth, read_jsonl(path, parse_mask_plan)))
    return SuiteRun(index["suite"], sequences)


def reference_plans(truth: list[GroundTruthFrame], pipeline: str) -> list[MaskPlan]:
    if pipeline not in REFERENCE_PIPELINES:
        raise ValueError(f"unknown reference pipeline {pipeline!r}")
    plans = []
    for f in truth:
        if pipeline == "noop":
            rects = ()
        elif pipeline == "blur_all":
            rects = (MaskRect(Box(0.0, 0.0, float(f.width), float(f.height)), "blur"),)
        else:
            rects = tuple(MaskRect(i.box, "blur") for i in f.instances if i.sensitive and i.visibility > 0)
        plans.append(MaskPlan(f.timestamp_ns, False, rects))
    return plans


def reference_run(suite_dir: Path, pipeline: str) -> SuiteRun:
    index = read_index(suite_dir)
    return SuiteRun(index["suite"], [Sequence(name, truth, reference_plans(truth, pipeline))
                                     for name, truth in load_truth(suite_dir).items()])


def write_plans(path: Path, plans: list[MaskPlan]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(serialize_mask_plan(p) + "\n" for p in plans), encoding="utf-8")
