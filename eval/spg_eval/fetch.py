"""Pinned, hash-verified downloads for the eval tooling.

Two kinds of artifact, both installed under $SPG_CACHE_DIR by the same code
that installs the build dependencies (scripts/fetch_deps.py: HTTPS only,
SHA-256 checked before anything is unpacked, safe extraction, per-artifact lock):

- Python wheels pinned in eval/pydeps.json, one per interpreter and platform,
  unpacked into deps/py-<name>/<version>-<tag>-<platform>/ and put on sys.path.
  Nothing is installed into the interpreter itself.
- Evaluation datasets pinned under "datasets" in models/manifest.json with
  their license, unpacked into deps/dataset-<name>/<version>-any/. Datasets are
  for local evaluation only: never committed, redistributed or uploaded.
"""
from __future__ import annotations

import importlib.util
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PYDEPS_MANIFEST = ROOT / "eval" / "pydeps.json"
MODELS_MANIFEST = ROOT / "models" / "manifest.json"

# Loaded by path so this works under `python -I`.
_spec = importlib.util.spec_from_file_location("spg_fetch_deps", ROOT / "scripts" / "fetch_deps.py")
assert _spec and _spec.loader
fetch_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fetch_deps)

FetchError = fetch_deps.FetchError
_SAFE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
_DATASET_FIELDS = ("purpose", "version", "url", "sha256", "size", "license", "source", "root")


def interpreter_tag() -> str:
    return f"cp{sys.version_info.major}{sys.version_info.minor}"


def _load(path: Path, key: str) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != 1 or not isinstance(data.get(key), dict):
        raise FetchError(f"{path}: expected schema 1 with a '{key}' object")
    return data[key]


def _check_pin(label: str, entry: dict) -> None:
    if not str(entry.get("url", "")).startswith("https://"):
        raise FetchError(f"{label}: URL must be HTTPS")
    if not re.fullmatch(r"[0-9a-f]{64}", str(entry.get("sha256", ""))):
        raise FetchError(f"{label}: sha256 must be 64 lowercase hex characters")


def ensure_pydeps(cache: Path, manifest: Path = PYDEPS_MANIFEST) -> list[Path]:
    """Install (or reuse) every pinned package for this interpreter; return their paths."""
    plat = fetch_deps.host_platform()
    key = f"{interpreter_tag()}-{plat}"
    paths = []
    for name, package in _load(manifest, "packages").items():
        version = package["version"]
        if not _SAFE.match(name) or not _SAFE.match(version):
            raise FetchError(f"{name}: unsafe name or version")
        wheel = package["wheels"].get(key)
        if wheel is None:
            raise FetchError(f"{name} {version}: no wheel pinned for {key} "
                             f"(pinned: {', '.join(sorted(package['wheels']))})")
        _check_pin(f"{name} {key}", wheel)
        fetch_deps.install(f"py-{name}", version, key, {**wheel, "root": "."}, cache, True)
        paths.append(cache / "deps" / f"py-{name}" / f"{version}-{key}")
    return paths


def use_pydeps(cache: Path) -> None:
    """Make the pinned packages importable, fetching them on first use."""
    for path in ensure_pydeps(cache):
        if str(path) not in sys.path:
            sys.path.insert(0, str(path))


def dataset_entry(name: str, manifest: Path = MODELS_MANIFEST) -> dict:
    datasets = _load(manifest, "datasets")
    if name not in datasets:
        raise FetchError(f"unknown dataset {name!r}; manifest lists {sorted(datasets)}")
    entry = datasets[name]
    missing = [f for f in _DATASET_FIELDS if f not in entry]
    if missing:
        raise FetchError(f"dataset {name}: manifest entry lacks {missing}")
    if not _SAFE.match(name) or not _SAFE.match(entry["version"]):
        raise FetchError(f"dataset {name}: unsafe name or version")
    _check_pin(f"dataset {name}", entry)
    return entry


def ensure_dataset(name: str, cache: Path, manifest: Path = MODELS_MANIFEST) -> Path:
    """Install (or reuse) a pinned dataset; return its directory."""
    entry = dataset_entry(name, manifest)
    fetch_deps.install(f"dataset-{name}", entry["version"], "any", entry, cache, True)
    return cache / "deps" / f"dataset-{name}" / f"{entry['version']}-any"
