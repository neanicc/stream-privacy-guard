#!/usr/bin/env python3
"""Helpers for scripts/package.sh (SPG-17): bundle models, check a staged
package, write the archive.

    package_tool.py bundle-models --stage DIR --platform P --cache DIR
    package_tool.py check --stage DIR --platform P [--with-models]
    package_tool.py archive --stage DIR --out DIST

`check` is the package's test: it fails when the module, its data or a
license is missing, when the module imports a library the package neither
ships nor can expect from OBS or the OS, when the package ships a library the
module does not import, when a macOS rpath points outside the bundle, or when
a model file is present without --with-models. Standard library only; it
calls otool/codesign on macOS and readelf on Linux, and reads Windows imports
from the PE file itself.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

PLATFORMS = ("macos-arm64", "windows-x64", "linux-x64")
MODULE = "obs-spg"

# Libraries the module may import without the package shipping them: OBS
# provides libobs, the OS provides the rest. Matched case-insensitively
# against the bare file name.
OBS_LIBS = {
    "macos-arm64": [r"libobs\.framework/.*"],
    "linux-x64": [r"libobs\.so\.\d+"],
    "windows-x64": [r"obs\.dll"],
}
SYSTEM_LIBS = {
    "macos-arm64": [],  # absolute /usr/lib and /System paths, checked separately
    "linux-x64": [r"libc\.so\.6", r"libm\.so\.6", r"libstdc\+\+\.so\.6", r"libgcc_s\.so\.1",
                  r"libpthread\.so\.0", r"libdl\.so\.2", r"librt\.so\.1", r"ld-linux-x86-64\.so\.2"],
    "windows-x64": [r"kernel32\.dll", r"user32\.dll", r"advapi32\.dll", r"ole32\.dll",
                    r"oleaut32\.dll", r"shell32\.dll", r"shlwapi\.dll", r"ws2_32\.dll",
                    r"bcrypt\.dll", r"dbghelp\.dll", r"version\.dll", r"d3d11\.dll", r"dxgi\.dll",
                    r"api-ms-win-.*\.dll", r"ucrtbase\.dll", r"vcruntime140(_1)?\.dll",
                    r"msvcp140(_\d+)?\.dll"],
}


class PackageError(Exception):
    pass


@dataclass
class Layout:
    """Where things live in a staged package for one platform."""
    platform: str
    root: Path

    @property
    def module(self) -> Path:
        if self.platform == "macos-arm64":
            return self.root / f"{MODULE}.plugin" / "Contents" / "MacOS" / MODULE
        suffix = ".dll" if self.platform == "windows-x64" else ".so"
        return self.root / MODULE / "bin" / "64bit" / f"{MODULE}{suffix}"

    @property
    def lib_dir(self) -> Path:
        """Where libraries the module loads are shipped."""
        if self.platform == "macos-arm64":
            return self.root / f"{MODULE}.plugin" / "Contents" / "Frameworks"
        return self.module.parent

    @property
    def data_dir(self) -> Path:
        if self.platform == "macos-arm64":
            return self.root / f"{MODULE}.plugin" / "Contents" / "Resources"
        return self.root / MODULE / "data"


@dataclass
class Imports:
    """What the module asks the dynamic loader for."""
    libraries: list[str] = field(default_factory=list)
    rpaths: list[str] = field(default_factory=list)


def is_shared_library(path: Path, platform: str) -> bool:
    name = path.name.lower()
    if platform == "windows-x64":
        return name.endswith(".dll")
    if platform == "macos-arm64":
        return name.endswith(".dylib") or name.endswith(".framework")
    return re.search(r"\.so(\.\d+)*$", name) is not None


def _matches(patterns: list[str], name: str) -> bool:
    return any(re.fullmatch(p, name, re.IGNORECASE) for p in patterns)


def dependency_problems(platform: str, imports: Imports, shipped: list[str]) -> list[str]:
    """Problems with the module's imports against the libraries shipped
    beside it (`shipped`: file names in Layout.lib_dir, the module excluded)."""
    problems: list[str] = []
    used: set[str] = set()
    for lib in imports.libraries:
        if platform == "macos-arm64":
            if lib.startswith(("/usr/lib/", "/System/")):
                continue
            match = re.fullmatch(r"@(?:rpath|loader_path/\.\./Frameworks)/(.+)", lib)
            if not match:
                problems.append(f"imports {lib}, which is outside the bundle and the OS")
                continue
            name = match.group(1)
            if _matches(OBS_LIBS[platform], name):
                continue
            top = name.split("/", 1)[0]
            if top in shipped:
                used.add(top)
            else:
                problems.append(f"imports {lib}, but Contents/Frameworks has no {top}")
            continue
        if _matches(OBS_LIBS[platform], lib) or _matches(SYSTEM_LIBS[platform], lib):
            continue
        found = next((s for s in shipped if s.lower() == lib.lower()), None)
        if found:
            used.add(found)
        else:
            problems.append(f"imports {lib}, which the package does not ship and OBS or the OS does not provide")
    for name in shipped:
        if name not in used:
            problems.append(f"ships {name}, which the module does not import")
    if platform == "macos-arm64":
        for rpath in imports.rpaths:
            if not rpath.startswith("@loader_path/"):
                problems.append(f"rpath {rpath} points outside the bundle")
        if used and "@loader_path/../Frameworks" not in imports.rpaths:
            problems.append("ships libraries but has no @loader_path/../Frameworks rpath")
    if platform == "linux-x64" and used:
        if not any("$ORIGIN" in r for r in imports.rpaths):
            problems.append("ships libraries but its RUNPATH has no $ORIGIN")
    return problems


# Reading imports --------------------------------------------------------------

def _run(args: list[str]) -> str:
    try:
        result = subprocess.run(args, capture_output=True, text=True, check=False)
    except FileNotFoundError as exc:
        raise PackageError(f"{args[0]} is not installed") from exc
    if result.returncode != 0:
        raise PackageError(f"{' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def macho_imports(path: Path) -> Imports:
    libs = [line.strip().split(" (compatibility")[0]
            for line in _run(["otool", "-L", str(path)]).splitlines()[1:] if line.strip()]
    rpaths = re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset \d+\)",
                        _run(["otool", "-l", str(path)]))
    return Imports(libs, rpaths)


def elf_imports(path: Path) -> Imports:
    out = _run(["readelf", "-d", str(path)])
    libs = re.findall(r"\(NEEDED\)\s+Shared library: \[(.+?)\]", out)
    rpaths: list[str] = []
    for value in re.findall(r"\((?:RPATH|RUNPATH)\)\s+Library (?:rpath|runpath): \[(.+?)\]", out):
        rpaths.extend(value.split(":"))
    return Imports(libs, rpaths)


def pe_imports(data: bytes) -> Imports:
    """DLL names in a PE32+ file's import and delay-import directories."""
    def u16(off: int) -> int:
        return struct.unpack_from("<H", data, off)[0]

    def u32(off: int) -> int:
        return struct.unpack_from("<I", data, off)[0]

    try:
        if data[:2] != b"MZ":
            raise PackageError("not a PE file (no MZ header)")
        pe = u32(0x3C)
        if data[pe:pe + 4] != b"PE\0\0":
            raise PackageError("not a PE file (no PE signature)")
        sections_count = u16(pe + 6)
        optional = pe + 24
        if u16(optional) != 0x20B:
            raise PackageError("not a 64-bit PE file")
        directories = optional + 112
        sections = optional + u16(pe + 20)
        table = [(u32(s + 12), max(u32(s + 8), u32(s + 16)), u32(s + 20))
                 for s in (sections + 40 * i for i in range(sections_count))]

        def offset(rva: int) -> int:
            for start, size, raw in table:
                if start <= rva < start + size:
                    return raw + rva - start
            raise PackageError(f"RVA {rva:#x} is in no section")

        def cstring(rva: int) -> str:
            start = offset(rva)
            return data[start:data.index(b"\0", start)].decode("ascii")

        names: list[str] = []
        # Import directory (1): 20-byte descriptors, name RVA at +12.
        # Delay-import directory (13): 32-byte descriptors, name RVA at +4.
        for index, size, name_at in ((1, 20, 12), (13, 32, 4)):
            rva = u32(directories + 8 * index)
            if rva == 0:
                continue
            entry = offset(rva)
            while any(data[entry:entry + size]):
                names.append(cstring(u32(entry + name_at)))
                entry += size
        return Imports(names, [])
    except (struct.error, ValueError, IndexError) as exc:
        raise PackageError(f"malformed PE file: {exc}") from exc


def read_imports(layout: Layout) -> Imports:
    if layout.platform == "macos-arm64":
        return macho_imports(layout.module)
    if layout.platform == "linux-x64":
        return elf_imports(layout.module)
    return pe_imports(layout.module.read_bytes())


# Models -----------------------------------------------------------------------

def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_models(manifest: Path) -> dict:
    return json.loads(manifest.read_text(encoding="utf-8"))["models"]


def model_problem(path: Path, model: dict) -> str | None:
    if not path.is_file():
        return f"{path} is missing"
    if path.stat().st_size != model["size"]:
        return f"{path}: size {path.stat().st_size}, manifest pins {model['size']}"
    if sha256_of(path) != model["sha256"]:
        return f"{path}: SHA-256 does not match the manifest"
    return None


MODEL_LICENSES = {
    "yunet": "yunet-2023mar-LICENSE.txt",
    "ppocr-det-en": "rapidocr-3.9.2-LICENSE.txt",
    "ppocr-rec-en": "rapidocr-3.9.2-LICENSE.txt",
}


def bundle_models(layout: Layout, manifest: Path, cache: Path, licenses: Path) -> list[str]:
    """Copy the pinned models from the local cache into data/models/, checked
    by size and SHA-256, with their license texts. Never downloads."""
    models = load_models(manifest)
    copied = []
    target_licenses = layout.data_dir / "licenses" / "models"
    target_licenses.mkdir(parents=True, exist_ok=True)
    for name, model in models.items():
        relative = Path(name) / model["version"] / model["file"]
        source = cache / "models" / relative
        problem = model_problem(source, model)
        if problem:
            raise PackageError(f"{name}: {problem}; run scripts/fetch_models.py first")
        license_file = MODEL_LICENSES.get(name)
        if license_file is None:
            raise PackageError(f"{name}: no license text in build-aux/package/licenses for this model")
        dest = layout.data_dir / "models" / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
        if model_problem(dest, model):
            raise PackageError(f"{name}: copy at {dest} does not match the manifest")
        shutil.copyfile(licenses / license_file, target_licenses / f"{name}-LICENSE.txt")
        copied.append(f"{name} {model['version']} ({model['license']})")
    return copied


# Check ------------------------------------------------------------------------

LICENSES = ("obs-spg-LICENSE.txt", "THIRD-PARTY-NOTICES.txt", "nlohmann-json/LICENSE.txt",
            "opencv/LICENSE.txt", "onnxruntime/LICENSE.txt", "onnxruntime/ThirdPartyNotices.txt")
TOP_LEVEL = ("INSTALL.txt", "LICENSE.txt", "scripts/fetch_models.py", "scripts/fetch_deps.py",
             "models/manifest.json")


def layout_problems(layout: Layout, manifest: Path, with_models: bool) -> list[str]:
    problems = []
    required = [layout.module, layout.data_dir / "locale" / "en-US.ini",
                layout.data_dir / "effects" / "spg-mask.effect"]
    if layout.platform == "macos-arm64":
        required.append(layout.root / f"{MODULE}.plugin" / "Contents" / "Info.plist")
    required += [layout.data_dir / "licenses" / name for name in LICENSES]
    required += [layout.root / name for name in TOP_LEVEL]
    problems += [f"missing {p.relative_to(layout.root)}" for p in required if not p.is_file()]
    third_party = layout.data_dir / "licenses" / "opencv" / "3rdparty"
    if not third_party.is_dir() or not any(third_party.iterdir()):
        problems.append("missing OpenCV's third-party licenses (licenses/opencv/3rdparty)")

    onnx = sorted(p for p in layout.root.rglob("*") if p.suffix.lower() in (".onnx", ".ort"))
    if not with_models:
        problems += [f"model file {p.relative_to(layout.root)} in a package built without --with-models"
                     for p in onnx]
        if (layout.data_dir / "models").exists():
            problems.append("data/models exists in a package built without --with-models")
    else:
        expected = set()
        for name, model in load_models(manifest).items():
            path = layout.data_dir / "models" / name / model["version"] / model["file"]
            expected.add(path)
            problem = model_problem(path, model)
            if problem:
                problems.append(problem)
            if not (layout.data_dir / "licenses" / "models" / f"{name}-LICENSE.txt").is_file():
                problems.append(f"missing the license of model {name}")
        problems += [f"unpinned model file {p.relative_to(layout.root)}" for p in onnx if p not in expected]
    return problems


def signature_problems(layout: Layout) -> list[str]:
    """Apple Silicon refuses to load code whose signature is invalid; an
    install step that rewrites load commands, or a file added to the bundle
    after scripts/package.sh signed it, breaks it."""
    bundle = layout.root / f"{MODULE}.plugin"
    result = subprocess.run(["codesign", "--verify", "--strict", "--deep", str(bundle)],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        return [f"invalid code signature on {bundle.name}: {result.stderr.strip()}"]
    return []


def check(layout: Layout, manifest: Path, with_models: bool) -> list[str]:
    problems = layout_problems(layout, manifest, with_models)
    if not layout.module.is_file():
        return problems
    shipped = sorted(p.name for p in layout.lib_dir.iterdir()
                     if p != layout.module and is_shared_library(p, layout.platform)) \
        if layout.lib_dir.is_dir() else []
    problems += dependency_problems(layout.platform, read_imports(layout), shipped)
    if layout.platform == "macos-arm64":
        problems += signature_problems(layout)
    return problems


# Archive ----------------------------------------------------------------------

def archive(stage: Path, out: Path, platform: str) -> Path:
    """Write <out>/<stage name>.zip (macOS, Windows) or .tar.gz (Linux) with
    the stage directory as its single top-level folder. File modes are kept;
    owner names are not recorded."""
    out.mkdir(parents=True, exist_ok=True)
    if platform == "linux-x64":
        target = out / f"{stage.name}.tar.gz"

        def scrub(info: tarfile.TarInfo) -> tarfile.TarInfo:
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            return info

        with tarfile.open(target, "w:gz") as tar:
            tar.add(stage, arcname=stage.name, filter=scrub)
    else:
        target = out / f"{stage.name}.zip"
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(p for p in stage.rglob("*") if p.is_file()):
                arcname = Path(stage.name) / path.relative_to(stage)
                zf.write(path, arcname.as_posix())
    return target


def write_sums(out: Path) -> Path:
    sums = out / "SHA256SUMS.txt"
    lines = [f"{sha256_of(p)}  {p.name}\n" for p in sorted(out.iterdir())
             if p.is_file() and p.name.startswith(f"{MODULE}-") and p.name.endswith((".zip", ".tar.gz"))]
    sums.write_text("".join(lines), encoding="utf-8", newline="\n")
    return sums


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    root = Path(__file__).resolve().parent.parent
    for name in ("bundle-models", "check"):
        p = sub.add_parser(name)
        p.add_argument("--stage", type=Path, required=True)
        p.add_argument("--platform", choices=PLATFORMS, required=True)
        p.add_argument("--manifest", type=Path, default=root / "models" / "manifest.json")
    sub.choices["bundle-models"].add_argument("--cache", type=Path, required=True)
    sub.choices["bundle-models"].add_argument("--licenses", type=Path,
                                               default=root / "build-aux" / "package" / "licenses")
    sub.choices["check"].add_argument("--with-models", action="store_true")
    p = sub.add_parser("archive")
    p.add_argument("--stage", type=Path, required=True)
    p.add_argument("--platform", choices=PLATFORMS, required=True)
    p.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)

    try:
        if args.command == "bundle-models":
            for line in bundle_models(Layout(args.platform, args.stage), args.manifest,
                                      args.cache.expanduser(), args.licenses):
                print(f"package: bundled model {line}")
        elif args.command == "check":
            problems = check(Layout(args.platform, args.stage), args.manifest, args.with_models)
            for problem in problems:
                print(f"package: check failed: {problem}", file=sys.stderr)
            if problems:
                return 1
            print(f"package: check passed for {args.stage.name}")
        else:
            target = archive(args.stage, args.out, args.platform)
            sums = write_sums(args.out)
            print(f"package: wrote {target} ({target.stat().st_size} bytes); checksums in {sums}")
    except (PackageError, OSError, KeyError, ValueError) as exc:
        print(f"package: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
