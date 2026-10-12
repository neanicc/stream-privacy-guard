#!/usr/bin/env python3
"""Generate an eval suite: PNG frames, ground truth and the suite's policy config.

    eval/generate.py text --out eval-out/text --seed 1 --profile ci
    eval/generate.py faces --out eval-out/faces --seed 1 --profile full   # fetches the dataset

The same seed and profile always give byte-identical ground truth. Pillow and
the faces dataset are fetched on first use into $SPG_CACHE_DIR (pinned and
hash-verified; see eval/spg_eval/fetch.py). Nothing printed names an item's text.
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from spg_eval import fetch  # noqa: E402

DEFAULT_SEED = 20261012


def default_cache() -> Path:
    return Path(os.environ.get("SPG_CACHE_DIR") or Path.home() / ".cache" / "spg").expanduser().resolve()


def generate_suite(name: str, out: Path, seed: int, profile: str, cache: Path, frames: bool = True) -> dict:
    fetch.use_pydeps(cache)
    if name == "text":
        from spg_eval import text_suite
        return text_suite.generate(out, seed, profile, frames)
    if name == "faces":
        from spg_eval import faces_suite
        dataset = fetch.dataset_entry("lfw")
        root = fetch.ensure_dataset("lfw", cache)
        return faces_suite.generate(out, seed, root, dataset, profile, frames)
    raise ValueError(f"unknown suite {name!r}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("suite", choices=("text", "faces"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--profile", choices=("ci", "full"), default="ci")
    parser.add_argument("--no-frames", action="store_true", help="write ground truth only")
    parser.add_argument("--cache", type=Path, default=default_cache())
    args = parser.parse_args(argv)
    try:
        index = generate_suite(args.suite, args.out, args.seed, args.profile, args.cache, not args.no_frames)
    except (fetch.FetchError, OSError, ValueError) as exc:
        print(f"generate: error: {exc}", file=sys.stderr)
        return 1
    frames = sum(s["frames"] for s in index["sequences"])
    print(f"{args.suite}: {len(index['sequences'])} sequences, {frames} frames, seed {args.seed} -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
