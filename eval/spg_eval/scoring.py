"""Metrics from docs/eval.md and the verdict against eval/thresholds.toml.

Inputs are ground truth frames and the pipeline's mask plans, paired by
timestamp. Every rule the docs leave to the scorer is documented in
docs/eval.md ("Scoring rules"). The report holds metric values and counts
only, never item text.
"""
from __future__ import annotations

import json
import math
import tomllib
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

from .contracts import CATEGORIES, Box, GroundTruthFrame, MaskPlan

# Keys in a [suite.*] table that are settings rather than metric limits.
SUITE_SETTINGS = ("required_in", "resolution", "fps")
MODES = ("ci", "local")


class ScoreError(ValueError):
    """Inputs cannot be scored (missing, duplicate or unpaired frames, bad thresholds)."""


@dataclass
class Sequence:
    name: str
    truth: list[GroundTruthFrame]
    plans: list[MaskPlan]


@dataclass
class SuiteRun:
    name: str
    sequences: list[Sequence]
    # Per detection cycle, from a realtime run (SPG-11/16). None when not measured.
    latencies_ms: list[float] | None = None


@dataclass
class Metric:
    value: float | None
    num: float = 0
    den: float = 0

    @classmethod
    def ratio(cls, num: float, den: float) -> Metric:
        return cls(num / den if den else None, num, den)


@dataclass
class Report:
    lines: list[str] = field(default_factory=list)
    suites: dict = field(default_factory=dict)
    passed: bool = True
    verdict: str = "THRESHOLDS: FAIL"


Rect = tuple[float, float, float, float]  # x0, y0, x1, y1


def _clip(box: Box, x0: float, y0: float, x1: float, y1: float) -> Rect | None:
    left, top = max(box.x, x0), max(box.y, y0)
    right, bottom = min(box.x + box.w, x1), min(box.y + box.h, y1)
    return (left, top, right, bottom) if right > left and bottom > top else None


def union_area(rects: list[Rect]) -> float:
    """Exact area of a union of axis-aligned rectangles (coordinate compression)."""
    xs = sorted({x for r in rects for x in (r[0], r[2])})
    total = 0.0
    for left, right in zip(xs, xs[1:]):
        spans = sorted((r[1], r[3]) for r in rects if r[0] <= left and r[2] >= right)
        covered, end = 0.0, -math.inf
        for top, bottom in spans:
            if bottom > end:
                covered += bottom - max(top, end)
                end = bottom
        total += covered * (right - left)
    return total


def covered_fraction(box: Rect, masks: list[Rect]) -> float:
    inside = [c for m in masks if (c := _clip(Box(m[0], m[1], m[2] - m[0], m[3] - m[1]), *box))]
    area = (box[2] - box[0]) * (box[3] - box[1])
    return union_area(inside) / area


def _group(kind: str) -> str:
    return "pii" if kind.startswith("pii:") else kind


def _pair(seq: Sequence) -> list[tuple[GroundTruthFrame, MaskPlan]]:
    plans: dict[int, MaskPlan] = {}
    for plan in seq.plans:
        if plan.timestamp_ns in plans:
            raise ScoreError(f"{seq.name}: two mask plans for timestamp {plan.timestamp_ns}")
        plans[plan.timestamp_ns] = plan
    truth = sorted(seq.truth, key=lambda f: f.frame)
    stamps = [f.timestamp_ns for f in truth]
    if len(set(stamps)) != len(stamps) or len({f.frame for f in truth}) != len(truth):
        raise ScoreError(f"{seq.name}: ground truth repeats a frame or timestamp")
    missing = [f.frame for f in truth if f.timestamp_ns not in plans]
    if missing:
        raise ScoreError(f"{seq.name}: no mask plan for {len(missing)} frame(s), first frame {missing[0]}")
    extra = set(plans) - set(stamps)
    if extra:
        raise ScoreError(f"{seq.name}: {len(extra)} mask plan(s) match no ground-truth frame")
    return [(f, plans[f.timestamp_ns]) for f in truth]


def suite_metrics(run: SuiteRun, tau: float) -> dict[str, Metric]:
    pairs: dict[str, list[int]] = defaultdict(lambda: [0, 0])  # group or kind -> [covered, visible]
    first: dict[tuple[str, str], bool] = {}
    leak = sensitive_frames = clean_frames = fallback_frames = total_frames = 0
    clean_area = 0.0
    for seq in run.sequences:
        for gt, plan in _pair(seq):
            frame_rect = (0.0, 0.0, float(gt.width), float(gt.height))
            masks = [] if plan.fallback else [c for r in plan.rects if (c := _clip(r.box, *frame_rect))]
            total_frames += 1
            fallback_frames += plan.fallback
            listed_sensitive = visible_sensitive = leaked = False
            for inst in gt.instances:
                listed_sensitive |= inst.sensitive
                box = _clip(inst.box, *frame_rect)
                if inst.visibility <= 0 or box is None:
                    continue
                covered = plan.fallback or covered_fraction(box, masks) >= tau
                for key in {_group(inst.kind), inst.kind}:
                    pairs[key][0] += covered
                    pairs[key][1] += 1
                if inst.kind == "watchlist":
                    first.setdefault((seq.name, inst.id), covered)
                if inst.sensitive:
                    visible_sensitive = True
                    leaked |= not covered
            sensitive_frames += visible_sensitive
            leak += leaked
            if not listed_sensitive:
                clean_frames += 1
                clean_area += 1.0 if plan.fallback else union_area(masks) / (gt.width * gt.height)

    metrics = {f"{group}_pair_recall": Metric.ratio(*pairs[group]) for group in ("watchlist", "pii", "face")}
    metrics["watchlist_first_frame_recall"] = Metric.ratio(sum(first.values()), len(first))
    metrics["leak_frame_rate"] = Metric.ratio(leak, sensitive_frames)
    metrics["clean_frame_blur_area"] = Metric.ratio(clean_area, clean_frames)
    metrics["decoy_blur_rate"] = Metric.ratio(*pairs["decoy"])
    metrics["fail_closed_frame_rate"] = Metric.ratio(fallback_frames, total_frames)
    lat = sorted(run.latencies_ms or [])
    # Nearest-rank 95th percentile.
    metrics["p95_detection_latency_ms"] = Metric(lat[math.ceil(0.95 * len(lat)) - 1] if lat else None, 0, len(lat))
    for kind in sorted(k for k in pairs if k.startswith("pii:")):
        metrics[f"{kind}_pair_recall"] = Metric.ratio(*pairs[kind])
    return metrics


def known_metrics() -> set[str]:
    """Every metric suite_metrics() can report, including the per-kind PII recalls."""
    return set(suite_metrics(SuiteRun("", []), 1.0)) | {f"{k}_pair_recall" for k in CATEGORIES
                                                         if k.startswith("pii:")}


def load_thresholds(path: Path) -> dict:
    with open(path, "rb") as handle:
        data = tomllib.load(handle)
    if data.get("schema") != 1:
        raise ScoreError(f"{path}: unsupported schema")
    tau = data.get("coverage_tau")
    if not isinstance(tau, (int, float)) or not 0 < tau <= 1:
        raise ScoreError(f"{path}: coverage_tau must be in (0, 1]")
    suites = data.get("suite")
    if not isinstance(suites, dict) or not suites:
        raise ScoreError(f"{path}: no [suite.*] tables")
    metrics = known_metrics()
    for name, spec in suites.items():
        required = spec.get("required_in")
        if not isinstance(required, list) or any(m not in MODES for m in required):
            raise ScoreError(f"{path}: suite.{name}.required_in must list modes from {MODES}")
        for key, limit in spec.items():
            if key in SUITE_SETTINGS:
                continue
            if not key.endswith(("_min", "_max")) or not isinstance(limit, (int, float)) or isinstance(limit, bool):
                raise ScoreError(f"{path}: suite.{name}.{key} is not a numeric <metric>_min/_max limit")
            # Checked here, not when the suite is scored, so a typo in a table
            # that this mode skips still fails at once.
            if key[:-4] not in metrics:
                raise ScoreError(f"{path}: suite.{name}.{key}: unknown metric {key[:-4]!r}")
    return data


def _fmt(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.4f}"


def evaluate(thresholds: dict, runs: dict[str, SuiteRun], mode: str) -> Report:
    """Score every run suite and decide PASS/FAIL for `mode`.

    PASS needs every suite required in `mode` to have run and every limit of
    every scored suite to hold. A metric with no data fails its limit.
    """
    if mode not in MODES:
        raise ScoreError(f"unknown mode {mode!r}")
    unknown = sorted(set(runs) - set(thresholds["suite"]))
    if unknown:
        raise ScoreError(f"no thresholds for suite(s) {unknown}")
    tau = float(thresholds["coverage_tau"])
    report = Report()
    for name, spec in thresholds["suite"].items():
        required = mode in spec["required_in"]
        run = runs.get(name)
        if run is None:
            state = f"FAIL (required in {mode} mode)" if required else f"skipped (not required in {mode} mode)"
            report.lines.append(f"{name:<9} not run  {state}")
            report.passed &= not required
            report.suites[name] = {"run": False, "required": required}
            continue
        metrics = suite_metrics(run, tau)
        checked = {}
        for key, limit in spec.items():
            if key in SUITE_SETTINGS:
                continue
            metric = key[:-4]
            if metric not in metrics:
                raise ScoreError(f"suite.{name}.{key}: unknown metric {metric!r}")
            m = metrics[metric]
            is_min = key.endswith("_min")
            ok = m.value is not None and (m.value >= limit if is_min else m.value <= limit)
            report.passed &= ok
            checked[metric] = ok
            counts = f"({m.num:g}/{m.den:g})" if m.value is not None else "(no data)"
            report.lines.append(f"{name:<9} {metric:<30} {_fmt(m.value):>7}  {'>=' if is_min else '<='} "
                                f"{limit:<6} {'PASS' if ok else 'FAIL'}  {counts}")
        report.suites[name] = {
            "run": True, "required": required, "sequences": len(run.sequences),
            "metrics": {k: {"value": m.value, "num": m.num, "den": m.den, "pass": checked.get(k)}
                        for k, m in metrics.items()}}
    report.verdict = "THRESHOLDS: PASS" if report.passed else "THRESHOLDS: FAIL"
    return report


def failed_limits(report: Report, suite: str) -> list[str]:
    """Names of the metrics whose limit failed in a scored suite."""
    metrics = report.suites.get(suite, {}).get("metrics", {})
    return sorted(k for k, m in metrics.items() if m["pass"] is False)


def write_report(out: Path, lines: list[str], verdict: str, suites: dict, **extra) -> None:
    """report.txt (the printed lines and verdict) and report.json (values and counts). No item text."""
    out.mkdir(parents=True, exist_ok=True)
    (out / "report.txt").write_text("\n".join([*lines, verdict]) + "\n", encoding="utf-8")
    payload = {"schema": 1, "verdict": verdict, "suites": suites, **extra}
    (out / "report.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
