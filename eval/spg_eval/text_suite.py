"""Seeded generator for the text suite (docs/eval.md).

Five UI templates, each with its own motion: a chat panel that scrolls, a
notification toast that slides in and fades out, a browser address bar where a
string is typed (partial reveal), an account window that fades in or stays
static, and a game HUD over a panning scene that fades in and out. Sequences
alternate 1080p and 720p, and every fourth sequence is clean (filler text and
decoys only). Everything derives from (seed, sequence index), so the same seed
gives byte-identical ground truth.
"""
from __future__ import annotations

import json
import random
from pathlib import Path

from . import synth
from .render import Item, Layer, desktop, fit_size, game, write_sequence
from .suite import INDEX

FPS = 60
PROFILES = {"ci": {"sequences": 10, "frames": 40}, "full": {"sequences": 30, "frames": 90}}
TEMPLATES = ("chat", "notification", "address_bar", "window", "hud")
RESOLUTIONS = ((1920, 1080), (1280, 720))


class _Items:
    """Allocates the planted strings of one sequence with ids unique in it."""

    def __init__(self, rng: random.Random, entries: list[str], clean: bool, first_kind: str) -> None:
        self.rng, self.entries, self.clean, self.n = rng, entries, clean, 0
        self.next_kind = first_kind

    def _make(self, kind: str, text: str) -> Item:
        self.n += 1
        prefix = {"watchlist": "wl", "decoy": "decoy"}.get(kind, "pii")
        return Item(f"{prefix}-{self.n}", kind, text)

    def secret(self, kinds: tuple[str, ...] = ("watchlist", *synth.PII_KINDS)) -> Item | str:
        """A sensitive item, or filler text in a clean sequence."""
        if self.clean:
            return synth.filler(self.rng, 2, 3)
        if self.next_kind in kinds:
            kind, self.next_kind = self.next_kind, ""
        elif "watchlist" in kinds and self.rng.random() < 0.5:
            kind = "watchlist"  # half of all secrets: the watch-list is the main feature
        else:
            kind = self.rng.choice(kinds)
        text = self.rng.choice(self.entries) if kind == "watchlist" else synth.pii(self.rng, kind)
        return self._make(kind, text)

    def decoy(self) -> Item:
        return self._make("decoy", synth.decoy(self.rng, self.entries))

    def maybe(self, p_secret: float = 0.35, p_decoy: float = 0.2) -> Item | str:
        roll = self.rng.random()
        if roll < p_secret:
            return self.secret()
        if roll < p_secret + p_decoy:
            return self.decoy()
        return synth.filler(self.rng, 2, 5)


def _ease(t: float) -> float:
    t = min(max(t, 0.0), 1.0)
    return 1 - (1 - t) ** 3


def _chat(rng, w, h, n, items):
    s = h / 1080
    bg = game(rng, w, h)
    pw, ph = int(0.27 * w), int(0.78 * h)
    px, py = w - pw - int(0.02 * w), int(0.1 * h)
    size, line_h = int(rng.randint(19, 24) * s), int(34 * s)
    speed = rng.uniform(1.0, 2.5) * s
    content_h = ph + int(speed * n) + line_h
    content = Layer.blank(pw, content_h, (22, 22, 28, 235))
    for k in range(content_h // line_h):
        user = synth.username(rng)
        part = items.maybe()
        x = content.text(10, k * line_h + 6, [user + ": "], size, (150, 200, 255))
        avail = pw - x - 10
        if isinstance(part, str):
            part = part[:max(4, int(avail / (0.55 * size)))]
            content.text(x, k * line_h + 6, [part], size, (235, 235, 235))
        else:
            content.text(x, k * line_h + 6, [part], fit_size(part.text, size, avail), (235, 235, 235))
    # Scrolling up: the viewport moves down the content.
    return bg, lambda i: [(content.crop(0, int(speed * i), pw, ph), px, py, 1.0)]


def _notification(rng, w, h, n, items):
    s = h / 1080
    bg = desktop(rng, w, h)
    static = Layer.blank(int(0.4 * w), int(0.2 * h), (235, 235, 240, 255))
    for row in range(3):
        static.text(14, int((14 + 40 * row) * s), [synth.filler(rng, 1, 2) + ": ", items.maybe(0.3, 0.4)],
                    int(22 * s), (30, 30, 30))
    tw, th = int(0.26 * w), int(0.12 * h)
    toast = Layer.blank(tw, th, (38, 42, 52, 245))
    toast.text(16, int(12 * s), ["New message"], int(24 * s), (255, 255, 255))
    secret = items.secret()
    text = secret.text if isinstance(secret, Item) else secret
    toast.text(16, int(56 * s), [secret], fit_size(text, int(26 * s), tw - 32), (225, 225, 230))
    t_in, slide = rng.randint(0, n // 4), rng.randint(8, 16)
    t_out = n - rng.randint(8, 12)
    final_x, start_x, ty = w - tw - int(0.015 * w), w, int(0.04 * h)

    def frame(i):
        x = round(start_x + (final_x - start_x) * _ease((i - t_in) / slide)) if i >= t_in else start_x
        alpha = 1.0 - min(max((i - t_out) / 10, 0.0), 1.0)
        return [(static, int(0.05 * w), int(0.55 * h), 1.0), (toast, x, ty, alpha)]
    return bg, frame


def _address_bar(rng, w, h, n, items):
    s = h / 1080
    bg = desktop(rng, w, h)
    bar_h = int(0.08 * h)
    page = Layer.blank(w, h - bar_h - int(40 * s), (250, 250, 252, 255))
    page.text(int(0.08 * w), int(30 * s), [synth.filler(rng, 2, 4)], int(40 * s), (20, 20, 20))
    for row in range(6):
        page.text(int(0.08 * w), int((110 + 46 * row) * s), [synth.filler(rng, 1, 3) + " ", items.maybe()],
                  int(24 * s), (50, 50, 50))
    secret = items.secret(("watchlist", "pii:email", "pii:address"))
    prefix = rng.choice(("search.example.com/?q=", "mail.example.net/to/", "maps.example.org/place/"))
    t0, typing = rng.randint(2, n // 3), rng.randint(8, 20)

    def frame(i):
        bar = Layer.blank(w, bar_h, (222, 224, 228, 255))
        reveal = {secret.id: min(max((i - t0 + 1) / typing, 0.0), 1.0)} if isinstance(secret, Item) else None
        bar.text(int(0.1 * w), int(bar_h * 0.3), [prefix, secret], int(26 * s), (25, 25, 25), reveal)
        return [(bar, 0, 0, 1.0), (page, 0, bar_h, 1.0)]
    return bg, frame


def _window(rng, w, h, n, items):
    s = h / 1080
    bg = desktop(rng, w, h)
    ww, wh = int(0.45 * w), int(0.5 * h)
    win = Layer.blank(ww, wh, (245, 245, 247, 255))
    win.text(16, int(10 * s), ["Account details"], int(26 * s), (60, 60, 70))
    labels = ("Name", "Email", "Phone", "Card", "Ship to", "Server", "API key", "Notes")
    for row, label in enumerate(rng.sample(labels, 6)):
        part = items.secret() if row < 2 else items.maybe(0.3, 0.35)
        text = part.text if isinstance(part, Item) else part
        y = int((60 + 52 * row) * s)
        x = win.text(24, y, [label + ":  "], int(24 * s), (90, 90, 100))
        win.text(x, y, [part], fit_size(text, int(24 * s), ww - x - 16), (20, 20, 20))
    fade, t0 = rng.random() < 0.5, rng.randint(0, n // 3)
    x, y = rng.randint(0, w - ww), rng.randint(0, h - wh - int(40 * s))
    return bg, lambda i: [(win, x, y, min(max((i - t0) / 15, 0.0), 1.0) if fade else 1.0)]


def _hud(rng, w, h, n, items):
    s = h / 1080
    speed = rng.uniform(1.0, 3.0)
    scene = game(rng, w + int(speed * n) + 1, h)
    hw, hh = int(0.3 * w), int(0.22 * h)
    hud = Layer.blank(hw, hh, (0, 0, 0, 150))
    rows = [["Player  ", items.secret(("watchlist",))], ["Server  ", items.secret(("pii:ip",))],
            [f"Ping {rng.randint(8, 90)} ms   Score {rng.randint(100, 9999)}"], [items.maybe(0.2, 0.5)]]
    for k, row in enumerate(rows):
        hud.text(16, int((14 + 44 * k) * s), row, int(26 * s), (255, 255, 255))
    t_in, t_out = rng.randint(0, n // 4), n - rng.randint(6, 12)

    def frame(i):
        alpha = min(max((i - t_in) / 12, 0.0), 1.0) * (1.0 - min(max((i - t_out) / 8, 0.0), 1.0))
        background = Layer(scene.crop((int(speed * i), 0, int(speed * i) + w, h)).convert("RGBA"))
        return [(background, 0, 0, 1.0), (hud, int(0.02 * w), h - hh - int(0.03 * h), alpha)]
    return scene.crop((0, 0, w, h)), frame


_BUILDERS = {"chat": _chat, "notification": _notification, "address_bar": _address_bar,
             "window": _window, "hud": _hud}


def sequence_spec(index: int) -> dict:
    w, h = RESOLUTIONS[index % 2]
    return {"name": f"seq-{index:03d}", "template": TEMPLATES[index % len(TEMPLATES)],
            "width": w, "height": h, "clean": index % 4 == 3}


def generate(out: Path, seed: int, profile: str = "ci", frames: bool = True) -> dict:
    """Write the suite to `out`; return its index. frames=False writes ground truth only."""
    spec = PROFILES[profile]
    entries = synth.watchlist(random.Random(f"{seed}:watchlist"))
    out.mkdir(parents=True, exist_ok=True)
    # Synthetic entries only; this file is the pipeline's config for the suite.
    (out / "policy.json").write_text(json.dumps({"schema": 1, "watchlist": entries}, indent=2) + "\n",
                                     encoding="utf-8")
    sequences = []
    for index in range(spec["sequences"]):
        seq = sequence_spec(index)
        rng = random.Random(f"{seed}:{seq['name']}")
        n = spec["frames"]
        first_kind = "watchlist" if index % 2 == 0 else synth.PII_KINDS[(index // 2) % len(synth.PII_KINDS)]
        items = _Items(rng, entries, seq["clean"], first_kind)
        bg, frame_fn = _BUILDERS[seq["template"]](rng, seq["width"], seq["height"], n, items)
        write_sequence(out / seq["name"], bg, frame_fn, n, FPS, frames)
        sequences.append({**seq, "frames": n, "frames_dir": f"{seq['name']}/frames"})
    index = {"schema": 1, "suite": "text", "seed": seed, "profile": profile, "fps": FPS,
             "policy": "policy.json", "sequences": sequences}
    (out / INDEX).write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    return index
