#!/usr/bin/env python3
"""Build and install libobs from the pinned OBS sources (macOS and Windows).

The OBS plugin template builds libobs and obs-frontend-api from the OBS source
release against the prebuilt obs-deps, installs their Development component and
links plugins against that. This script does the same, but every input comes
from deps/manifest.json and was already downloaded and SHA-256 verified by
scripts/fetch_deps.py; OBS's own configure-time downloads are switched off
(build-aux/obs-cmake/buildspec.cmake). Linux needs no build: the plugin links
libobs from the official Ubuntu package (manifest entry obs-studio-ubuntu).

    python3 build-aux/build_libobs.py --cache ~/.cache/spg [--generator NAME]

The install lands in $SPG_CACHE_DIR/deps/libobs/<version>-<platform>/ with a
.spg-build.json marker naming its inputs; a rerun with the same inputs does
nothing. Called by `scripts/ci/run.sh setup` after fetch_deps.py.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OVERRIDES = HERE / "obs-cmake"
BUILD_MARKER = ".spg-build.json"

_spec = importlib.util.spec_from_file_location("fetch_deps", ROOT / "scripts" / "fetch_deps.py")
assert _spec and _spec.loader
fetch_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fetch_deps)
FetchError = fetch_deps.FetchError


def unpacked(cache: Path, name: str, dep: dict, plat: str) -> Path:
    """The directory fetch_deps.py unpacked `name` into, checked against the manifest."""
    entry = dep["platforms"][plat]
    dest = cache / "deps" / name / f"{dep['version']}-{plat}"
    try:
        marker = json.loads((dest / fetch_deps.MARKER).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        marker = {}
    if marker.get("sha256") != entry["sha256"]:
        raise FetchError(f"{name} {dep['version']} for {plat} is not unpacked in {dest}; "
                         "run scripts/fetch_deps.py first")
    return dest


def builder_digest() -> str:
    digest = hashlib.sha256()
    for path in [Path(__file__).resolve(), *sorted(OVERRIDES.iterdir())]:
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def run(cmd: list[str], cwd: Path | None = None) -> None:
    print("+ " + " ".join(cmd), flush=True)
    result = subprocess.run(cmd, cwd=cwd, check=False)
    if result.returncode != 0:
        raise FetchError(f"command failed with exit code {result.returncode}: {cmd[0]} {cmd[1]}")


def build(cache: Path, manifest: dict, plat: str, generator: str | None) -> str:
    deps = manifest["deps"]
    obs, obs_deps = deps["obs-studio"], deps["obs-deps"]
    version = obs["version"]
    source = unpacked(cache, "obs-studio", obs, plat)
    prebuilt = unpacked(cache, "obs-deps", obs_deps, plat)

    if plat == "macos-arm64":
        generator = "Xcode"
    elif not generator:
        raise FetchError("--generator is required on Windows (scripts/ci/run.sh passes it)")

    inputs = {
        "obs-studio": obs["platforms"][plat]["sha256"],
        "obs-deps": obs_deps["platforms"][plat]["sha256"],
        "builder": builder_digest(),
        "generator": generator,
    }
    x86_archive = None
    if plat == "windows-x64":
        x86 = deps["obs-deps-x86"]["platforms"][plat]
        x86_archive = fetch_deps.fetch_verified(x86["url"], x86["sha256"], cache / "downloads")
        inputs["obs-deps-x86"] = x86["sha256"]

    prefix = cache / "deps" / "libobs" / f"{version}-{plat}"
    marker = prefix / BUILD_MARKER
    with fetch_deps.exclusive_lock(cache / "locks" / f"libobs-{version}-{plat}.lock"):
        try:
            if json.loads(marker.read_text(encoding="utf-8")).get("inputs") == inputs:
                return f"libobs {version} {plat}: cached at {prefix}"
        except (OSError, ValueError):
            pass

        work = cache / "build" / f"libobs-{version}-{plat}"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        # OBS's Windows configure always configures a 32-bit helper tree in
        # <source>/build_x86, whose own buildspec step looks for the x86
        # obs-deps archive in <source>/.deps before downloading it. Hand it the
        # verified copy so nothing is downloaded outside deps/manifest.json.
        shutil.rmtree(source / "build_x86", ignore_errors=True)
        if x86_archive is not None:
            (source / ".deps").mkdir(exist_ok=True)
            shutil.copy2(x86_archive, source / ".deps" / x86_archive.name)

        configure = [
            "cmake", "-S", str(source), "-B", str(work / "build"), "-G", generator,
            f"-DOBS_VERSION_OVERRIDE:STRING={version}",
            "-DOBS_CMAKE_VERSION:STRING=3.0.0",
            "-DENABLE_PLUGINS:BOOL=OFF",
            "-DENABLE_FRONTEND:BOOL=OFF",
            "-DENABLE_SCRIPTING:BOOL=OFF",
            # Third-party code built with a newer compiler than OBS tested; SPG's
            # own targets keep warnings as errors.
            "-DCMAKE_COMPILE_WARNING_AS_ERROR:BOOL=OFF",
            f"-DCMAKE_PREFIX_PATH:PATH={prebuilt.as_posix()}",
            f"-DCMAKE_MODULE_PATH:PATH={OVERRIDES.as_posix()}",
        ]
        native_args: list[str] = []
        if plat == "macos-arm64":
            # Deployment target from OBS's own macOS preset (CMakePresets.json).
            configure += ["-DCMAKE_OSX_ARCHITECTURES:STRING=arm64",
                          "-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=12.0",
                          f"-DCMAKE_PROJECT_INCLUDE:FILEPATH={(OVERRIDES / 'enable-swift.cmake').as_posix()}"]
            native_args = ["-quiet"]
        else:
            configure += ["-A", "x64"]
            native_args = ["/verbosity:minimal", "/nologo"]
        run(configure)
        # obs-frontend-api pulls in libobs (and w32-pthreads on Windows); the
        # Development install component needs all of them built.
        run(["cmake", "--build", str(work / "build"), "--target", "obs-frontend-api",
             "--config", "Release", "--parallel", "--", *native_args])

        prefix.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".libobs-", dir=prefix.parent))
        try:
            run(["cmake", "--install", str(work / "build"), "--component", "Development",
                 "--config", "Release", "--prefix", str(staging)])
            (staging / BUILD_MARKER).write_text(
                json.dumps({"name": "libobs", "version": version, "platform": plat,
                            "inputs": inputs}, indent=2) + "\n", encoding="utf-8")
            if prefix.exists():
                shutil.rmtree(prefix)
            os.replace(staging, prefix)
        finally:
            shutil.rmtree(staging, ignore_errors=True)
        # The build trees are large and CI caches $SPG_CACHE_DIR; keep only the install.
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(source / "build_x86", ignore_errors=True)
    return f"libobs {version} {plat}: built and installed to {prefix}"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache", type=Path,
                        default=Path(os.environ.get("SPG_CACHE_DIR", Path.home() / ".cache" / "spg")))
    parser.add_argument("--manifest", type=Path, default=ROOT / "deps" / "manifest.json")
    parser.add_argument("--generator", default=None, help="CMake generator for Windows (Visual Studio)")
    args = parser.parse_args(argv)
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        plat = fetch_deps.host_platform()
        if plat == "linux-x64":
            print("libobs: Linux links the official Ubuntu package (obs-studio-ubuntu); nothing to build")
            return 0
        if shutil.which("cmake") is None:
            raise FetchError("cmake not found on PATH")
        print(build(args.cache.expanduser().resolve(), manifest, plat, args.generator), flush=True)
    except FetchError as exc:
        print(f"build_libobs: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
