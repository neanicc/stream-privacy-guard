"""Layers, backgrounds and frame composition shared by the text and faces suites.

A frame is a static background plus layers (a chat panel, a toast, a webcam
overlay) pasted at an offset with an opacity. Each layer records where it drew
every planted item, so ground truth comes from the same numbers that placed the
pixels. Ground-truth rules (docs/eval.md):

- An item's box is its full box in Source pixels, clipped to its layer and to
  the frame, since nothing outside those can show. Partial reveal (typing) does
  not shrink the box: the rest of the string is about to appear there.
- visibility = opacity x revealed fraction x (clipped area / full area).
- Every planted item is listed on every frame of its sequence. When it cannot
  show (off-screen, scrolled out, faded out, not yet typed) its visibility is 0
  and its box is the unclipped one. Frames of clean sequences therefore are the
  only frames with no sensitive instance.

Requires Pillow; call fetch.use_pydeps() before importing this module.
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps, features

from .contracts import Box, GroundTruthFrame, Instance, serialize_ground_truth_frame
from .suite import TRUTH

if not features.check("freetype2"):
    raise RuntimeError("Pillow was built without FreeType; the pinned wheels include it")

Rect = tuple[float, float, float, float]  # x0, y0, x1, y1
_FONTS: dict[int, ImageFont.FreeTypeFont] = {}


def font(size: int) -> ImageFont.FreeTypeFont:
    """Pillow's bundled scalable default font (Aileron Regular, CC0)."""
    if size not in _FONTS:
        _FONTS[size] = ImageFont.load_default(size)
    return _FONTS[size]


@dataclass(frozen=True)
class Item:
    id: str
    kind: str  # a category name or "decoy"
    text: str


@dataclass
class Placed:
    item: Item
    box: Rect  # layer pixels, full text box
    revealed: float = 1.0


@dataclass
class Layer:
    image: Image.Image  # RGBA
    placed: list[Placed] = field(default_factory=list)

    @classmethod
    def blank(cls, w: int, h: int, fill: tuple = (0, 0, 0, 0)) -> Layer:
        return cls(Image.new("RGBA", (w, h), fill))

    def text(self, x: float, y: float, parts: list, size: int, color: tuple,
             reveal: dict[str, float] | None = None) -> float:
        """Draw strings and Items left to right on one line; return the end x."""
        draw, f = ImageDraw.Draw(self.image), font(size)
        for part in parts:
            text = part.text if isinstance(part, Item) else part
            shown = text
            x = round(x)
            if isinstance(part, Item):
                frac = (reveal or {}).get(part.id, 1.0)
                shown = text[:round(len(text) * frac)]
                l, t, r, b = draw.textbbox((x, y), text, font=f)
                self.placed.append(Placed(part, (math.floor(l), math.floor(t), math.ceil(r), math.ceil(b)),
                                          len(shown) / len(text)))
            if shown:
                draw.text((x, y), shown, font=f, fill=color)
            x += draw.textlength(text, font=f)
        return x

    def crop(self, x0: int, y0: int, w: int, h: int) -> Layer:
        moved = [Placed(p.item, (p.box[0] - x0, p.box[1] - y0, p.box[2] - x0, p.box[3] - y0), p.revealed)
                 for p in self.placed]
        return Layer(self.image.crop((x0, y0, x0 + w, y0 + h)), moved)


def fit_size(text: str, size: int, width: float) -> int:
    """Largest font size <= size whose rendering of `text` fits in `width`."""
    while size > 10 and font(size).getlength(text) > width:
        size -= 1
    return size


def _area(r: Rect) -> float:
    return max(0.0, r[2] - r[0]) * max(0.0, r[3] - r[1])


def compose(background: Image.Image, placements: list[tuple[Layer, int, int, float]]) -> tuple[Image.Image, list[Instance]]:
    """Paste (layer, x, y, opacity) in order; return the RGB frame and its instances."""
    frame = background.copy()
    fw, fh = frame.size
    instances = []
    for layer, ox, oy, alpha in placements:
        lw, lh = layer.image.size
        clip = (max(ox, 0), max(oy, 0), min(ox + lw, fw), min(oy + lh, fh))
        if alpha > 0 and _area(clip) > 0:
            img = layer.image
            if alpha < 1:
                img = img.copy()
                img.putalpha(img.getchannel("A").point(lambda v: round(v * alpha)))
            frame.paste(img, (ox, oy), img)
        for p in layer.placed:
            full = (p.box[0] + ox, p.box[1] + oy, p.box[2] + ox, p.box[3] + oy)
            shown = (max(full[0], clip[0]), max(full[1], clip[1]), min(full[2], clip[2]), min(full[3], clip[3]))
            visibility = alpha * p.revealed * _area(shown) / _area(full) if _area(shown) > 0 else 0.0
            box = shown if visibility > 0 else full
            # Rounded for stable output; never rounded down to "not visible".
            visibility = max(round(visibility, 4), 0.0001) if visibility > 0 else 0.0
            instances.append(Instance(p.item.id, p.item.kind,
                                      Box(float(box[0]), float(box[1]), float(box[2] - box[0]), float(box[3] - box[1])),
                                      visibility))
    return frame, instances


def _color(rng: random.Random, lo: int, hi: int) -> tuple[int, int, int]:
    return tuple(rng.randint(lo, hi) for _ in range(3))


def gradient(w: int, h: int, top: tuple, bottom: tuple) -> Image.Image:
    return ImageOps.colorize(Image.linear_gradient("L").resize((w, h)), top, bottom)


def desktop(rng: random.Random, w: int, h: int) -> Image.Image:
    """Muted wallpaper, a few app windows with filler text, a taskbar."""
    from .synth import filler

    img = gradient(w, h, _color(rng, 20, 90), _color(rng, 60, 160))
    draw = ImageDraw.Draw(img)
    s = h / 1080
    for _ in range(rng.randint(1, 3)):
        x, y = rng.randint(0, w // 2), rng.randint(0, h // 2)
        ww, wh = rng.randint(w // 5, w // 3), rng.randint(h // 5, h // 3)
        draw.rectangle((x, y, x + ww, y + wh), fill=_color(rng, 200, 240))
        draw.rectangle((x, y, x + ww, y + int(28 * s)), fill=_color(rng, 120, 170))
        for row in range(3):
            draw.text((x + 10, y + int((40 + 30 * row) * s)), filler(rng, 2, 4), font=font(int(18 * s)),
                      fill=(40, 40, 40))
    draw.rectangle((0, h - int(40 * s), w, h), fill=(25, 25, 30))
    draw.text((w - int(90 * s), h - int(32 * s)), f"{rng.randint(1, 12)}:{rng.randint(0, 59):02d}",
              font=font(int(20 * s)), fill=(230, 230, 230))
    return img


def game(rng: random.Random, w: int, h: int) -> Image.Image:
    """Saturated scene with shapes, standing in for gameplay."""
    img = gradient(w, h, _color(rng, 0, 255), _color(rng, 0, 255))
    draw = ImageDraw.Draw(img)
    for _ in range(rng.randint(8, 16)):
        x, y = rng.randint(-w // 8, w), rng.randint(-h // 8, h)
        r = rng.randint(h // 30, h // 5)
        shape = draw.ellipse if rng.random() < 0.5 else draw.rectangle
        shape((x, y, x + r * 2, y + r), fill=_color(rng, 0, 255))
    return img


def timestamp_ns(frame: int, fps: int) -> int:
    return frame * 1_000_000_000 // fps


def write_sequence(seq_dir: Path, background: Image.Image, frame_fn, n: int, fps: int, frames: bool = True) -> None:
    """Render n frames (frame_fn(i) -> placements) and their ground truth into seq_dir."""
    w, h = background.size
    (seq_dir / "frames").mkdir(parents=True, exist_ok=True)
    lines = []
    for i in range(n):
        image, instances = compose(background, frame_fn(i))
        if frames:
            image.convert("RGB").save(seq_dir / "frames" / f"{i:06d}.png", format="PNG", compress_level=1)
        lines.append(serialize_ground_truth_frame(GroundTruthFrame(i, timestamp_ns(i, fps), w, h, tuple(instances))))
    (seq_dir / TRUTH).write_text("".join(line + "\n" for line in lines), encoding="utf-8")
