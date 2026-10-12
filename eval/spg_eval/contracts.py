"""Strict reader and writer for the schema-1 ground truth and mask plan JSON.

Mirrors the C++ parser in core/src/contracts/json.cpp (rules in
docs/contracts.md): schema 1, "space": "source", every field required, unknown
and duplicate fields rejected, integers must be JSON integers, numbers finite,
float fields range-checked before they are narrowed to float32 exactly as C++
narrows them. Error messages name fields and indices only and never quote
input. tests/scripts/test_scorer.py parses tests/fixtures/contracts/*.json
with this module so the two parsers stay in step.
"""
from __future__ import annotations

import json
import math
import struct
from dataclasses import dataclass

SCHEMA = 1
CATEGORIES = ("face", "watchlist", "pii:email", "pii:phone", "pii:card", "pii:ip", "pii:address", "pii:token")
KINDS = (*CATEGORIES, "decoy")
MASK_STYLES = ("blur", "pixelate", "solid_fill")
FLOAT_MAX = 3.4028234663852886e38
INT64_MIN, INT64_MAX = -(2**63), 2**63 - 1
UINT32_MAX = 2**32 - 1


class ContractError(ValueError):
    pass


@dataclass(frozen=True)
class Box:
    x: float
    y: float
    w: float
    h: float


@dataclass(frozen=True)
class Instance:
    id: str
    kind: str
    box: Box
    visibility: float

    @property
    def sensitive(self) -> bool:
        return self.kind != "decoy"


@dataclass(frozen=True)
class GroundTruthFrame:
    frame: int
    timestamp_ns: int
    width: int
    height: int
    instances: tuple[Instance, ...]


@dataclass(frozen=True)
class MaskRect:
    box: Box
    style: str


@dataclass(frozen=True)
class MaskPlan:
    timestamp_ns: int
    fallback: bool
    rects: tuple[MaskRect, ...]


def _f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


class _Reader:
    def __init__(self, value: object, doc: str, path: str, allowed: tuple[str, ...]) -> None:
        self.doc, self.path = doc, path
        where = f"'{path}'" if path else "document"
        if not isinstance(value, dict):
            raise ContractError(f"{doc}: {where} must be an object")
        if any(key not in allowed for key in value):
            # The unknown name is not echoed: it is input and could be secret.
            raise ContractError(f"{doc}: unknown field in {where} (allowed: {', '.join(allowed)})")
        self.value = value

    def fail(self, key: str, message: str) -> ContractError:
        return ContractError(f"{self.doc}: '{self.path + '.' if self.path else ''}{key}' {message}")

    def require(self, key: str) -> object:
        if key not in self.value:
            raise self.fail(key, "is required")
        return self.value[key]

    def integer(self, key: str, lo: int, hi: int) -> int:
        v = self.require(key)
        if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
            raise self.fail(key, f"must be an integer in [{lo}, {hi}]")
        return v

    def float32(self, key: str, lo: float, hi: float, rule: str) -> float:
        v = self.require(key)
        if not isinstance(v, (int, float)) or isinstance(v, bool):
            raise self.fail(key, "must be a number")
        # Range first: an int too large for a float would make math.isfinite raise
        # OverflowError. Python compares ints with floats exactly.
        if isinstance(v, float) and not math.isfinite(v):
            raise self.fail(key, "must be finite")
        if not lo <= v <= hi:
            raise self.fail(key, rule)
        return _f32(float(v))

    def string(self, key: str) -> str:
        v = self.require(key)
        if not isinstance(v, str):
            raise self.fail(key, "must be a string")
        return v

    def boolean(self, key: str) -> bool:
        v = self.require(key)
        if not isinstance(v, bool):
            raise self.fail(key, "must be true or false")
        return v

    def array(self, key: str) -> list:
        v = self.require(key)
        if not isinstance(v, list):
            raise self.fail(key, "must be an array")
        return v


def _parse_document(text: str | bytes, doc: str) -> object:
    if isinstance(text, bytes):
        try:
            text = text.decode("utf-8")
        except UnicodeDecodeError:
            raise ContractError(f"{doc}: not valid UTF-8") from None

    def no_duplicates(pairs: list[tuple[str, object]]) -> dict:
        result = dict(pairs)
        if len(result) != len(pairs):
            raise ContractError(f"{doc}: an object repeats a field name")
        return result

    def no_constants(_: str) -> object:
        raise ContractError(f"{doc}: numbers must be finite")

    try:
        return json.loads(text, object_pairs_hook=no_duplicates, parse_constant=no_constants)
    except json.JSONDecodeError as exc:
        # The library message is not forwarded; only the offset is.
        raise ContractError(f"{doc}: invalid JSON at offset {exc.pos}") from None


def _header(root: object, doc: str, allowed: tuple[str, ...]) -> _Reader:
    r = _Reader(root, doc, "", allowed)
    if r.integer("schema", 0, INT64_MAX) != SCHEMA:
        raise ContractError(f"{doc}: unsupported schema version (expected {SCHEMA})")
    if r.string("space") != "source":
        raise r.fail("space", 'must be "source"')
    return r


def _box(value: object, doc: str, path: str) -> Box:
    r = _Reader(value, doc, path, ("x", "y", "w", "h"))
    in_range, non_negative = "must be within the float range", "must not be negative and must be within the float range"
    return Box(r.float32("x", -FLOAT_MAX, FLOAT_MAX, in_range), r.float32("y", -FLOAT_MAX, FLOAT_MAX, in_range),
               r.float32("w", 0.0, FLOAT_MAX, non_negative), r.float32("h", 0.0, FLOAT_MAX, non_negative))


def parse_ground_truth_frame(text: str | bytes) -> GroundTruthFrame:
    doc = "ground truth"
    r = _header(_parse_document(text, doc), doc,
                ("schema", "space", "frame", "timestamp_ns", "width", "height", "instances"))
    instances, ids = [], set()
    for i, item in enumerate(r.array("instances")):
        path = f"instances[{i}]"
        ir = _Reader(item, doc, path, ("id", "kind", "box", "visibility"))
        ident, kind = ir.string("id"), ir.string("kind")
        if kind not in KINDS:
            raise ir.fail("kind", 'must be "decoy" or a category name')
        box = _box(ir.require("box"), doc, path + ".box")
        visibility = ir.float32("visibility", 0.0, 1.0, "must be in [0, 1]")
        if not ident:
            raise ContractError(f"{doc}: instance {i}: id must not be empty")
        if ident in ids:
            raise ContractError(f"{doc}: instance {i}: id repeats an earlier instance")
        ids.add(ident)
        instances.append(Instance(ident, kind, box, visibility))
    return GroundTruthFrame(r.integer("frame", 0, INT64_MAX), r.integer("timestamp_ns", INT64_MIN, INT64_MAX),
                            r.integer("width", 1, UINT32_MAX), r.integer("height", 1, UINT32_MAX),
                            tuple(instances))


def parse_mask_plan(text: str | bytes) -> MaskPlan:
    doc = "mask plan"
    r = _header(_parse_document(text, doc), doc, ("schema", "space", "timestamp_ns", "fallback", "rects"))
    rects = []
    for i, item in enumerate(r.array("rects")):
        path = f"rects[{i}]"
        ir = _Reader(item, doc, path, ("box", "style"))
        box = _box(ir.require("box"), doc, path + ".box")
        style = ir.string("style")
        if style not in MASK_STYLES:
            raise ir.fail("style", 'must be "blur", "pixelate" or "solid_fill"')
        rects.append(MaskRect(box, style))
    return MaskPlan(r.integer("timestamp_ns", INT64_MIN, INT64_MAX), r.boolean("fallback"), tuple(rects))


def _box_json(box: Box) -> dict:
    return {"x": box.x, "y": box.y, "w": box.w, "h": box.h}


def _dump(obj: dict) -> str:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False, allow_nan=False)


def serialize_ground_truth_frame(frame: GroundTruthFrame) -> str:
    """One line, no trailing newline, accepted by both parsers (checked by re-parsing)."""
    text = _dump({
        "schema": SCHEMA, "space": "source", "frame": frame.frame, "timestamp_ns": frame.timestamp_ns,
        "width": frame.width, "height": frame.height,
        "instances": [{"id": i.id, "kind": i.kind, "box": _box_json(i.box), "visibility": i.visibility}
                      for i in frame.instances]})
    parse_ground_truth_frame(text)
    return text


def serialize_mask_plan(plan: MaskPlan) -> str:
    text = _dump({"schema": SCHEMA, "space": "source", "timestamp_ns": plan.timestamp_ns,
                  "fallback": plan.fallback,
                  "rects": [{"box": _box_json(r.box), "style": r.style} for r in plan.rects]})
    parse_mask_plan(text)
    return text


def read_jsonl(path, parse):
    """Parse one document per non-empty line; errors carry the line number."""
    out = []
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            if line.strip():
                try:
                    out.append(parse(line))
                except ContractError as exc:
                    raise ContractError(f"{path}:{number}: {exc}") from None
    return out
