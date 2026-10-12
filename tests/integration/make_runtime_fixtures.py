#!/usr/bin/env python3
"""Write the fixtures spg-runtime-tests reads, into one output directory.

    scale_static.onnx   y = 2 * x, x: float32 [2, 3]
    scale_dynamic.onnx  y = 2 * x, x: float32 [batch, 4]
    not_a_model.onnx    bytes that are not an ONNX model
    models.txt          one line per model in models/manifest.json:
                        name<TAB>cache path<TAB>warmup shapes<TAB>metadata keys

The two ONNX models are a few hundred bytes of hand-encoded protobuf, built
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
    (args.out / "models.txt").write_text(models_listing(args.manifest, args.cache.expanduser()),
                                         encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
