"""Faces suite compositor (docs/eval.md).

Face crops from a dataset pinned in models/manifest.json (LFW by default) are
pasted as webcam-style overlays onto generated backgrounds, at face heights
from 40 to 320 px (at 1080p), one to three per frame, static or drifting, some
fading in. Each face gets its own column so overlays never hide each other.
The dataset's `face_box` (in the dataset image's pixels) maps to the ground
truth box through the same crop and scale that placed the pixels.

The output holds images of real people: it stays in the local output
directory, is never committed, and is never uploaded as a CI artifact.
"""
from __future__ import annotations

import json
import random
from pathlib import Path

from PIL import Image

from .render import Item, Layer, Placed, desktop, game, write_sequence
from .suite import INDEX
from .text_suite import FPS, RESOLUTIONS

PROFILES = {"ci": {"sequences": 6, "frames": 30}, "full": {"sequences": 24, "frames": 60}}


def _overlay(rng: random.Random, image_path: Path, face_box: dict, face_h: float, max_w: int,
             ident: str) -> Layer:
    with Image.open(image_path) as src:
        src = src.convert("RGB")
    fx, fy, fw, fh = face_box["x"], face_box["y"], face_box["w"], face_box["h"]
    m = rng.uniform(0.2, 0.5)
    x0, y0 = max(0, round(fx - m * fw)), max(0, round(fy - m * fh))
    x1, y1 = min(src.width, round(fx + fw + m * fw)), min(src.height, round(fy + fh + m * fh))
    border = 4
    scale = min(face_h / fh, (max_w - 2 * border) / (x1 - x0))
    cw, ch = max(1, round((x1 - x0) * scale)), max(1, round((y1 - y0) * scale))
    layer = Layer.blank(cw + 2 * border, ch + 2 * border, (30, 30, 30, 255))
    layer.image.paste(src.crop((x0, y0, x1, y1)).resize((cw, ch), Image.Resampling.BILINEAR), (border, border))
    sx, sy = cw / (x1 - x0), ch / (y1 - y0)
    box = (border + (fx - x0) * sx, border + (fy - y0) * sy, border + (fx + fw - x0) * sx, border + (fy + fh - y0) * sy)
    layer.placed.append(Placed(Item(ident, "face", ""), box))
    return layer


def generate(out: Path, seed: int, dataset_dir: Path, dataset: dict, profile: str = "full",
             frames: bool = True) -> dict:
    """Write the faces suite to `out`; return its index. `dataset` is the manifest entry."""
    images = sorted(dataset_dir.glob(dataset["images"]))
    if not images:
        raise FileNotFoundError(f"no images matching {dataset['images']!r} in {dataset_dir}")
    spec = PROFILES[profile]
    out.mkdir(parents=True, exist_ok=True)
    (out / "policy.json").write_text(json.dumps({"schema": 1}, indent=2) + "\n", encoding="utf-8")
    sequences = []
    for index in range(spec["sequences"]):
        name, (w, h) = f"seq-{index:03d}", RESOLUTIONS[index % 2]
        clean, n = index % 4 == 3, spec["frames"]
        rng = random.Random(f"{seed}:faces:{name}")
        bg = (desktop if rng.random() < 0.5 else game)(rng, w, h)
        count = 0 if clean else rng.randint(1, 3)
        slot_w = w // max(count, 1)
        tracks = []
        for k in range(count):
            layer = _overlay(rng, rng.choice(images), dataset["face_box"],
                             rng.uniform(40, 320) * h / 1080, slot_w - 8, f"face-{k}")
            lw, lh = layer.image.size
            xs = [k * slot_w + rng.randint(0, max(0, slot_w - lw)) for _ in range(2)]
            ys = [rng.randint(0, max(0, h - lh)) for _ in range(2)]
            if rng.random() < 0.5:  # static
                xs[1], ys[1] = xs[0], ys[0]
            fade = rng.randint(1, 12) if rng.random() < 0.3 else 0
            tracks.append((layer, xs, ys, fade))

        def frame_fn(i):
            t = i / max(n - 1, 1)
            return [(layer, round(xs[0] + (xs[1] - xs[0]) * t), round(ys[0] + (ys[1] - ys[0]) * t),
                     min(i / fade, 1.0) if fade else 1.0) for layer, xs, ys, fade in tracks]

        write_sequence(out / name, bg, frame_fn, n, FPS, frames)
        sequences.append({"name": name, "template": "webcam", "width": w, "height": h, "clean": clean,
                          "frames": n, "frames_dir": f"{name}/frames"})
    index = {"schema": 1, "suite": "faces", "seed": seed, "profile": profile, "fps": FPS,
             "policy": "policy.json", "sequences": sequences}
    (out / INDEX).write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    return index
