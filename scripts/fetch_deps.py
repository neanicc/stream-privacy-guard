#!/usr/bin/env python3
"""Fetch the prebuilt dependencies pinned in deps/manifest.json.

Each archive is downloaded once into $SPG_CACHE_DIR/downloads/, its SHA-256 is
checked against the manifest (a mismatch is an error, never a warning), and it
is unpacked into $SPG_CACHE_DIR/deps/<name>/<version>-<platform>/. CMake reads
the same manifest to find those directories (cmake/SpgDeps.cmake).

    scripts/fetch_deps.py --cache ~/.cache/spg                 # this machine
    scripts/fetch_deps.py --cache /tmp/c --platform all --no-extract   # verify every pin

Standard library only. Called by `scripts/ci/run.sh setup`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent
MARKER = ".spg-fetch.json"
CHUNK = 1 << 20


class FetchError(Exception):
    pass


def host_platform() -> str:
    machine = platform.machine().lower()
    if sys.platform.startswith("linux") and machine in ("x86_64", "amd64"):
        return "linux-x64"
    if sys.platform == "darwin" and machine == "arm64":
        return "macos-arm64"
    if sys.platform in ("win32", "cygwin", "msys") and machine in ("amd64", "x86_64"):
        return "windows-x64"
    raise FetchError(f"unsupported host platform {sys.platform}/{machine}; "
                     "supported: linux-x64, macos-arm64, windows-x64")


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url: str, dest: Path, attempts: int = 3) -> None:
    if not url.startswith("https://"):
        raise FetchError(f"refusing non-HTTPS URL {url}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    # Prefer curl: it uses the OS trust store, which some Python installs lack.
    # Integrity comes from the SHA-256 check either way.
    curl = shutil.which("curl")
    for attempt in range(1, attempts + 1):
        fd, tmp_name = tempfile.mkstemp(prefix=dest.name + ".", suffix=".part", dir=dest.parent)
        tmp = Path(tmp_name)
        try:
            if curl:
                os.close(fd)
                result = subprocess.run(
                    [curl, "--fail", "--location", "--silent", "--show-error",
                     "--proto", "=https", "--connect-timeout", "30", "--output", str(tmp), url],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, check=False)
                if result.returncode != 0:
                    raise OSError(f"curl exit {result.returncode}: {result.stderr.strip()}")
            else:
                with os.fdopen(fd, "wb") as out, urllib.request.urlopen(url, timeout=120) as resp:
                    shutil.copyfileobj(resp, out, CHUNK)
            os.replace(tmp, dest)
            return
        except OSError as exc:  # URLError and timeouts are OSError subclasses
            tmp.unlink(missing_ok=True)
            if attempt == attempts:
                raise FetchError(f"download failed after {attempts} attempts: {url}: {exc}") from exc
            print(f"  retrying {url} ({exc})", file=sys.stderr)
            time.sleep(2 * attempt)


def fetch_verified(url: str, expected: str, downloads: Path) -> Path:
    archive = downloads / PurePosixPath(url).name
    if archive.exists() and sha256_of(archive) == expected:
        return archive
    download(url, archive)
    actual = sha256_of(archive)
    if actual != expected:
        archive.unlink(missing_ok=True)
        raise FetchError(f"SHA-256 mismatch for {url}\n  expected {expected}\n  actual   {actual}")
    return archive


def safe_member(name: str) -> bool:
    path = PurePosixPath(name.replace("\\", "/"))
    return not path.is_absolute() and ".." not in path.parts and not (path.parts and ":" in path.parts[0])


def extract(archive: Path, dest: Path) -> None:
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as zf:
            bad = [n for n in zf.namelist() if not safe_member(n)]
            if bad:
                raise FetchError(f"{archive.name}: unsafe paths in archive: {bad[:3]}")
            zf.extractall(dest)
        return
    try:
        with tarfile.open(archive, "r:*") as tf:
            if hasattr(tarfile, "data_filter"):
                # Rejects absolute paths, '..', links escaping dest and device files.
                tf.extractall(dest, filter="data")
            else:
                bad = [m.name for m in tf.getmembers()
                       if not safe_member(m.name) or m.isdev()
                       or (m.issym() or m.islnk()) and not safe_member(m.linkname)]
                if bad:
                    raise FetchError(f"{archive.name}: unsafe members in archive: {bad[:3]}")
                tf.extractall(dest)
    except tarfile.TarError as exc:
        raise FetchError(f"{archive.name}: cannot unpack safely: {exc}") from exc


def install(name: str, version: str, plat: str, entry: dict, cache: Path, unpack: bool) -> str:
    expected = entry["sha256"].lower()
    if len(expected) != 64:
        raise FetchError(f"{name}/{plat}: sha256 must be 64 hex characters")
    dest = cache / "deps" / name / f"{version}-{plat}"
    marker = dest / MARKER
    if unpack and marker.exists():
        try:
            if json.loads(marker.read_text(encoding="utf-8")).get("sha256") == expected:
                return f"{name} {version} {plat}: cached at {dest}"
        except (OSError, ValueError):
            pass
    archive = fetch_verified(entry["url"], expected, cache / "downloads")
    if not unpack:
        return f"{name} {version} {plat}: sha256 verified ({archive.name})"

    dest.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{name}-", dir=dest.parent))
    try:
        extract(archive, staging)
        root = (staging / entry.get("root", ".")).resolve()
        if not root.is_dir() or staging.resolve() not in (root, *root.parents):
            raise FetchError(f"{name}/{plat}: root {entry.get('root')!r} not found in {archive.name}")
        if dest.exists():
            shutil.rmtree(dest)
        shutil.move(str(root), str(dest))
        marker.write_text(json.dumps({"name": name, "version": version, "platform": plat,
                                      "url": entry["url"], "sha256": expected}, indent=2) + "\n",
                          encoding="utf-8")
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return f"{name} {version} {plat}: sha256 verified, unpacked to {dest}"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache", type=Path,
                        default=Path(os.environ.get("SPG_CACHE_DIR", Path.home() / ".cache" / "spg")),
                        help="cache directory (default: $SPG_CACHE_DIR or ~/.cache/spg)")
    parser.add_argument("--manifest", type=Path, default=ROOT / "deps" / "manifest.json")
    parser.add_argument("--platform", default=None,
                        help="platform key from the manifest, or 'all' (default: this machine)")
    parser.add_argument("--only", action="append", default=[], metavar="NAME",
                        help="fetch only this dependency (repeatable)")
    parser.add_argument("--no-extract", action="store_true",
                        help="download and verify hashes without unpacking")
    args = parser.parse_args(argv)

    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        if manifest.get("schema") != 1:
            raise FetchError(f"{args.manifest}: unsupported schema {manifest.get('schema')!r}")
        known = manifest["platforms"]
        if args.platform == "all":
            platforms = list(known)
        else:
            platforms = [args.platform or host_platform()]
        for plat in platforms:
            if plat not in known:
                raise FetchError(f"unknown platform {plat!r}; manifest lists {known}")
        deps = manifest["deps"]
        unknown = sorted(set(args.only) - set(deps))
        if unknown:
            raise FetchError(f"unknown dependency {unknown}; manifest lists {sorted(deps)}")
        cache = args.cache.expanduser().resolve()
        for name, dep in deps.items():
            if args.only and name not in args.only:
                continue
            for plat in platforms:
                entry = dep["platforms"].get(plat)
                if entry is None:
                    raise FetchError(f"{name} has no entry for platform {plat}")
                print(install(name, dep["version"], plat, entry, cache, not args.no_extract))
    except FetchError as exc:
        print(f"fetch_deps: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
