#!/usr/bin/env python3
"""Write the fixtures spg-runtime-tests reads, into one output directory.

    scale_static.onnx   y = 2 * x, x: float32 [2, 3]
    scale_dynamic.onnx  y = 2 * x, x: float32 [batch, 4]
    not_a_model.onnx    bytes that are not an ONNX model
    text_wrong_shape.onnx  DBNet-shaped (x [1, 3, h, w], y [1, 1, oh, ow]) but
                        y is SpaceToDepth(2) of the channel mean: [1, 4, h/2, w/2],
                        as many values as the right shape
    text_sqrt.onnx      DBNet-shaped, y = sqrt(channel mean of x): NaN where
                        the input is darker than mid gray
    models.txt          one line per model in models/manifest.json:
                        name<TAB>cache path<TAB>warmup shapes<TAB>metadata keys

The ONNX models are a few hundred bytes of hand-encoded protobuf, built
here at build time so no binary enters Git and no `onnx` package is needed.
Field numbers follow onnx/onnx.proto (IR version 8, opset 13).

    python3 -I make_runtime_fixtures.py --out DIR --manifest models/manifest.json --cache ~/.cache/spg
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

FLOAT = 1  # TensorProto.DataType.FLOAT


def varint(value: int) -> bytes:
    if value < 0:
        value += 1 << 64  # int64 fields encode negatives as ten-byte varints
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def field_varint(number: int, value: int) -> bytes:
    return varint(number << 3) + varint(value)


def field_bytes(number: int, payload: bytes | str) -> bytes:
    data = payload.encode("utf-8") if isinstance(payload, str) else payload
    return varint((number << 3) | 2) + varint(len(data)) + data


def value_info(name: str, dims: list[int | str]) -> bytes:
    shape = b""
    for dim in dims:
        # Dimension: dim_value = 1, dim_param = 2
        entry = field_varint(1, dim) if isinstance(dim, int) else field_bytes(2, dim)
        shape += field_bytes(1, entry)
    tensor_type = field_varint(1, FLOAT) + field_bytes(2, shape)  # elem_type, shape
    type_proto = field_bytes(1, tensor_type)  # TypeProto.tensor_type
    return field_bytes(1, name) + field_bytes(2, type_proto)  # ValueInfoProto


def scale_model(dims: list[int | str], metadata: dict[str, str]) -> bytes:
    # TensorProto: data_type = 2, name = 8, raw_data = 9 (scalar, no dims)
    two = field_varint(2, FLOAT) + field_bytes(8, "two") + field_bytes(9, struct.pack("<f", 2.0))
    # NodeProto: input = 1, output = 2, name = 3, op_type = 4
    node = field_bytes(1, "x") + field_bytes(1, "two") + field_bytes(2, "y") + field_bytes(3, "scale") \
        + field_bytes(4, "Mul")
    # GraphProto: node = 1, name = 2, initializer = 5, input = 11, output = 12
    graph = field_bytes(1, node) + field_bytes(2, "spg_scale") + field_bytes(5, two) \
        + field_bytes(11, value_info("x", dims)) + field_bytes(12, value_info("y", dims))
    opset = field_bytes(1, "") + field_varint(2, 13)  # OperatorSetIdProto
    # ModelProto: ir_version = 1, producer_name = 2, graph = 7, opset_import = 8, metadata_props = 14
    model = field_varint(1, 8) + field_bytes(2, "spg-tests") + field_bytes(7, graph) + field_bytes(8, opset)
    for key, value in metadata.items():
        model += field_bytes(14, field_bytes(1, key) + field_bytes(2, value))
    return model


def attribute_int(name: str, value: int) -> bytes:
    # AttributeProto: name = 1, i = 3, type = 20 (INT = 2)
    return field_bytes(1, name) + field_varint(3, value) + field_varint(20, 2)


def attribute_ints(name: str, values: list[int]) -> bytes:
    # AttributeProto: name = 1, ints = 8, type = 20 (INTS = 7)
    return field_bytes(1, name) + b"".join(field_varint(8, v) for v in values) + field_varint(20, 7)


def text_model(name: str, ops: list[tuple[str, list[bytes]]]) -> bytes:
    """A DBNet-shaped chain: x float32 [1, 3, h, w] -> ops -> y [1, 1, oh, ow].

    The first op takes x, each next op the previous output, and the last one
    writes y. Output dims are symbolic, so TextDetector accepts the model when
    it opens and only detect() can see what the model really returns.
    """
    nodes = b""
    previous = "x"
    for i, (op_type, attributes) in enumerate(ops):
        output = "y" if i == len(ops) - 1 else f"t{i}"
        node = field_bytes(1, previous) + field_bytes(2, output) + field_bytes(3, f"n{i}") + field_bytes(4, op_type)
        for attribute in attributes:
            node += field_bytes(5, attribute)  # NodeProto.attribute
        nodes += field_bytes(1, node)
        previous = output
    graph = nodes + field_bytes(2, name) + field_bytes(11, value_info("x", [1, 3, "h", "w"])) \
        + field_bytes(12, value_info("y", [1, 1, "oh", "ow"]))
    opset = field_bytes(1, "") + field_varint(2, 13)
    return field_varint(1, 8) + field_bytes(2, "spg-tests") + field_bytes(7, graph) + field_bytes(8, opset)


def models_listing(manifest: Path, cache: Path) -> str:
    data = json.loads(manifest.read_text(encoding="utf-8"))
    lines = []
    for name, model in data["models"].items():
        path = cache / "models" / name / model["version"] / model["file"]
        shapes = ";".join("x".join(str(d) for d in shape) for shape in model["warmup_inputs"])
        keys = ",".join(model.get("metadata", []))
        lines.append(f"{name}\t{path.as_posix()}\t{shapes}\t{keys}\n")
    return "".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    args = parser.parse_args(argv)

    args.out.mkdir(parents=True, exist_ok=True)
    meta = {"spg_fixture": "scale-by-two"}
    (args.out / "scale_static.onnx").write_bytes(scale_model([2, 3], meta))
    (args.out / "scale_dynamic.onnx").write_bytes(scale_model(["batch", 4], meta))
    (args.out / "not_a_model.onnx").write_bytes(b"this is not an ONNX model\n")
    # Channel mean (keepdims), then SpaceToDepth: the right number of values in
    # the wrong shape, so only the shape check can reject it.
    channel_mean = ("ReduceMean", [attribute_ints("axes", [1])])
    (args.out / "text_wrong_shape.onnx").write_bytes(text_model("spg_text_wrong_shape", [
        channel_mean, ("SpaceToDepth", [attribute_int("blocksize", 2)])]))
    # sqrt of the channel mean: finite on light pixels, NaN on dark ones.
    (args.out / "text_sqrt.onnx").write_bytes(text_model("spg_text_sqrt", [channel_mean, ("Sqrt", [])]))
    (args.out / "models.txt").write_text(models_listing(args.manifest, args.cache.expanduser()),
                                         encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
