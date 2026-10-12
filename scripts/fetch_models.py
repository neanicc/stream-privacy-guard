#!/usr/bin/env python3
"""Fetch the ONNX models pinned in models/manifest.json.

Each model is downloaded to a temporary file, its size and SHA-256 are checked
against the manifest (a mismatch is an error, never a warning), and only then
is it moved to $SPG_CACHE_DIR/models/<name>/<version>/<file>. A file already in
the cache is reused when its hash still matches, and replaced when it does not.

    scripts/fetch_models.py                          # every model into ~/.cache/spg
    scripts/fetch_models.py --cache /tmp/c --only yunet
    scripts/fetch_models.py --verify                 # check the cache, download nothing

Standard library only. Shares download, hashing and locking with
scripts/fetch_deps.py. Called by `scripts/ci/run.sh setup`. Nothing at runtime
downloads models: the plugin and spg-cli only read files this script installed.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Loaded by path so this works under `python -I`, which drops the script
# directory from sys.path.
_spec = importlib.util.spec_from_file_location("spg_fetch_deps", Path(__file__).with_name("fetch_deps.py"))
assert _spec and _spec.loader
fetch_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fetch_deps)

FetchError = fetch_deps.FetchError
SAFE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
REQUIRED = ("version", "file", "url", "size", "sha256", "license", "source", "warmup_inputs")


def validate(name: str, model: dict) -> None:
    """Reject entries that are incomplete or could write outside the cache."""
    missing = [key for key in REQUIRED if key not in model]
    if missing:
        raise FetchError(f"{name}: manifest entry lacks {missing}")
    for label, value in (("name", name), ("version", model["version"]), ("file", model["file"])):
        if not isinstance(value, str) or not SAFE_NAME.match(value) or ".." in value:
            raise FetchError(f"{name}: unsafe {label} {value!r} (letters, digits, '.', '_', '-' only)")
    if not str(model["url"]).startswith("https://"):
        raise FetchError(f"{name}: URL must be HTTPS")
    if not re.fullmatch(r"[0-9a-f]{64}", str(model["sha256"])):
        raise FetchError(f"{name}: sha256 must be 64 lowercase hex characters")
    if not isinstance(model["size"], int) or model["size"] <= 0:
        raise FetchError(f"{name}: size must be a positive integer")
    shapes = model["warmup_inputs"]
    if not shapes or not all(isinstance(s, list) and s and all(isinstance(d, int) and d > 0 for d in s)
                             for s in shapes):
        raise FetchError(f"{name}: warmup_inputs must be a non-empty list of positive integer shapes")


def model_path(cache: Path, name: str, model: dict) -> Path:
    return cache / "models" / name / model["version"] / model["file"]


def check_file(path: Path, model: dict) -> str | None:
    """Return None when `path` holds the pinned model, else why it does not."""
    if not path.is_file():
        return "missing"
    size = path.stat().st_size
    if size != model["size"]:
        return f"size {size}, manifest pins {model['size']}"
    actual = fetch_deps.sha256_of(path)
    if actual != model["sha256"]:
        return f"SHA-256 {actual}, manifest pins {model['sha256']}"
    return None


def install(name: str, model: dict, cache: Path) -> str:
    validate(name, model)
    dest = model_path(cache, name, model)
    # Worktrees share one cache; the lock keeps two setups from racing on a file.
    with fetch_deps.exclusive_lock(cache / "locks" / f"model-{name}-{model['version']}.lock"):
        if check_file(dest, model) is None:
            return f"{name} {model['version']}: cached at {dest}"
        # Never leave an unverified file where the runtime looks for models.
        dest.unlink(missing_ok=True)
        partial = dest.with_name(dest.name + ".download")
        try:
            fetch_deps.download(model["url"], partial)
            problem = check_file(partial, model)
            if problem is not None:
                raise FetchError(f"{name}: downloaded file rejected ({problem})\n  url {model['url']}")
            os.replace(partial, dest)
        finally:
            partial.unlink(missing_ok=True)
    return f"{name} {model['version']}: sha256 verified, installed at {dest}"


def verify(name: str, model: dict, cache: Path) -> str:
    validate(name, model)
    dest = model_path(cache, name, model)
    problem = check_file(dest, model)
    if problem is not None:
        raise FetchError(f"{name}: {dest}: {problem}; run scripts/fetch_models.py")
    return f"{name} {model['version']}: verified at {dest}"


def load_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema") != 1:
        raise FetchError(f"{path}: unsupported schema {manifest.get('schema')!r}")
    models = manifest.get("models")
    if not isinstance(models, dict) or not models:
        raise FetchError(f"{path}: no models listed")
    return models


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache", type=Path,
                        default=Path(os.environ.get("SPG_CACHE_DIR", Path.home() / ".cache" / "spg")),
                        help="cache directory (default: $SPG_CACHE_DIR or ~/.cache/spg)")
    parser.add_argument("--manifest", type=Path, default=ROOT / "models" / "manifest.json")
    parser.add_argument("--only", action="append", default=[], metavar="NAME",
                        help="fetch only this model (repeatable)")
    parser.add_argument("--verify", action="store_true",
                        help="check cached files against the manifest without downloading")
    args = parser.parse_args(argv)

    try:
        models = load_manifest(args.manifest)
        unknown = sorted(set(args.only) - set(models))
        if unknown:
            raise FetchError(f"unknown model {unknown}; manifest lists {sorted(models)}")
        cache = args.cache.expanduser().resolve()
        for name, model in models.items():
            if args.only and name not in args.only:
                continue
            print(verify(name, model, cache) if args.verify else install(name, model, cache))
    except (FetchError, OSError, ValueError, KeyError) as exc:
        print(f"fetch_models: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
