"""Offline tests for build-aux/package_tool.py, the check and archive steps of
scripts/package.sh. Run by ctest (scripts.package_tool) or directly:
python3 -I tests/scripts/test_package_tool.py
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import re
import stat
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("package_tool", ROOT / "build-aux" / "package_tool.py")
assert _spec and _spec.loader
package_tool = importlib.util.module_from_spec(_spec)
sys.modules["package_tool"] = package_tool  # dataclasses look their module up
_spec.loader.exec_module(package_tool)

Imports = package_tool.Imports
Layout = package_tool.Layout
problems_for = package_tool.dependency_problems


def make_pe(imports: list[str], delay: list[str], magic: int = 0x20B) -> bytes:
    """A minimal PE32+ image: one section holding the import and delay-import
    descriptors and the DLL names."""
    image = bytearray(0x600)
    image[0:2] = b"MZ"
    image[0x3C:0x40] = (0x40).to_bytes(4, "little")
    pe = 0x40
    image[pe:pe + 4] = b"PE\0\0"
    coff = pe + 4
    image[coff:coff + 2] = (0x8664).to_bytes(2, "little")
    image[coff + 2:coff + 4] = (1).to_bytes(2, "little")
    image[coff + 16:coff + 18] = (240).to_bytes(2, "little")
    optional = pe + 24
    image[optional:optional + 2] = magic.to_bytes(2, "little")
    directories = optional + 112
    section = optional + 240
    va, raw = 0x1000, 0x400
    image[section:section + 8] = b".idata\0\0"
    image[section + 8:section + 12] = (0x200).to_bytes(4, "little")
    image[section + 12:section + 16] = va.to_bytes(4, "little")
    image[section + 16:section + 20] = (0x200).to_bytes(4, "little")
    image[section + 20:section + 24] = raw.to_bytes(4, "little")

    names_rva = va + 0x100

    def put_name(name: str) -> int:
        nonlocal names_rva
        rva = names_rva
        data = name.encode("ascii") + b"\0"
        image[raw + rva - va:raw + rva - va + len(data)] = data
        names_rva += len(data)
        return rva

    if imports:
        image[directories + 8:directories + 12] = va.to_bytes(4, "little")
        for i, name in enumerate(imports):
            entry = raw + 20 * i
            image[entry + 12:entry + 16] = put_name(name).to_bytes(4, "little")
    if delay:
        delay_rva = va + 0x80
        image[directories + 8 * 13:directories + 8 * 13 + 4] = delay_rva.to_bytes(4, "little")
        for i, name in enumerate(delay):
            entry = raw + (delay_rva - va) + 32 * i
            image[entry + 4:entry + 8] = put_name(name).to_bytes(4, "little")
    return bytes(image)


class PeImportsTest(unittest.TestCase):
    def test_reads_imports_and_delay_imports(self):
        data = make_pe(["obs.dll", "onnxruntime.dll", "KERNEL32.dll"], ["dbghelp.dll"])
        self.assertEqual(package_tool.pe_imports(data).libraries,
                         ["obs.dll", "onnxruntime.dll", "KERNEL32.dll", "dbghelp.dll"])

    def test_no_imports(self):
        self.assertEqual(package_tool.pe_imports(make_pe([], [])).libraries, [])

    def test_rejects_non_pe_and_32_bit(self):
        with self.assertRaises(package_tool.PackageError):
            package_tool.pe_imports(b"\x7fELF" + bytes(200))
        with self.assertRaises(package_tool.PackageError):
            package_tool.pe_imports(make_pe(["obs.dll"], [], magic=0x10B))
        with self.assertRaises(package_tool.PackageError):
            package_tool.pe_imports(make_pe(["obs.dll"], [])[:0x100])


class DependencyTest(unittest.TestCase):
    def test_windows_runtime_must_be_shipped(self):
        imports = Imports(["obs.dll", "KERNEL32.dll", "VCRUNTIME140.dll", "api-ms-win-crt-heap-l1-1-0.dll",
                           "onnxruntime.dll"])
        self.assertEqual(problems_for("windows-x64", imports, ["onnxruntime.dll"]), [])
        problems = problems_for("windows-x64", imports, [])
        self.assertEqual(len(problems), 1)
        self.assertIn("onnxruntime.dll", problems[0])

    def test_unknown_library_fails(self):
        problems = problems_for("windows-x64", Imports(["obs.dll", "opencv_world490.dll"]), [])
        self.assertEqual(len(problems), 1)
        problems = problems_for("linux-x64", Imports(["libobs.so.0", "libfoo.so.2"]), [])
        self.assertEqual(len(problems), 1)

    def test_shipped_but_unused_fails(self):
        problems = problems_for("windows-x64", Imports(["obs.dll"]), ["onnxruntime.dll"])
        self.assertEqual(problems, ["ships onnxruntime.dll, which the module does not import"])

    def test_linux_needs_origin_runpath(self):
        imports = Imports(["libobs.so.0", "libc.so.6", "libstdc++.so.6", "libonnxruntime.so.1"], [])
        problems = problems_for("linux-x64", imports, ["libonnxruntime.so.1"])
        self.assertEqual(problems, ["ships libraries but its RUNPATH has no $ORIGIN"])
        imports.rpaths = ["$ORIGIN"]
        self.assertEqual(problems_for("linux-x64", imports, ["libonnxruntime.so.1"]), [])

    def test_macos_rules(self):
        base = ["@rpath/libobs.framework/Versions/A/libobs", "/usr/lib/libc++.1.dylib",
                "/usr/lib/libSystem.B.dylib"]
        self.assertEqual(problems_for("macos-arm64", Imports(base, []), []), [])
        ort = base + ["@rpath/libonnxruntime.1.dylib"]
        good = Imports(ort, ["@loader_path/../Frameworks"])
        self.assertEqual(problems_for("macos-arm64", good, ["libonnxruntime.1.dylib"]), [])
        self.assertEqual(len(problems_for("macos-arm64", good, [])), 1)
        no_rpath = problems_for("macos-arm64", Imports(ort, []), ["libonnxruntime.1.dylib"])
        self.assertEqual(no_rpath, ["ships libraries but has no @loader_path/../Frameworks rpath"])
        build_path = Imports(base + ["/Users/someone/.cache/spg/deps/x/libfoo.dylib"],
                             ["/Users/someone/.cache/spg/deps/libobs/Frameworks"])
        self.assertEqual(len(problems_for("macos-arm64", build_path, [])), 2)


def write(path: Path, data: bytes = b"x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def staged(root: Path, platform: str = "linux-x64") -> Layout:
    layout = Layout(platform, root)
    write(layout.module)
    write(layout.data_dir / "locale" / "en-US.ini")
    write(layout.data_dir / "effects" / "spg-mask.effect")
    for name in package_tool.LICENSES:
        write(layout.data_dir / "licenses" / name)
    write(layout.data_dir / "licenses" / "opencv" / "3rdparty" / "zlib-LICENSE")
    for name in package_tool.TOP_LEVEL:
        write(root / name)
    if platform == "macos-arm64":
        write(root / "obs-spg.plugin" / "Contents" / "Info.plist")
    return layout


MODEL = b"pinned model bytes"


def manifest(path: Path) -> Path:
    path.write_text(json.dumps({"schema": 1, "models": {"yunet": {
        "version": "1", "file": "m.onnx", "size": len(MODEL),
        "sha256": hashlib.sha256(MODEL).hexdigest(), "license": "MIT"}}}), encoding="utf-8")
    return path


class LayoutTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.manifest = manifest(self.root / "manifest.json")

    def tearDown(self):
        self.tmp.cleanup()

    def test_complete_layouts_pass(self):
        for platform in package_tool.PLATFORMS:
            layout = staged(self.root / platform, platform)
            self.assertEqual(package_tool.layout_problems(layout, self.manifest, False), [], platform)

    def test_missing_pieces_fail(self):
        layout = staged(self.root / "s")
        (layout.data_dir / "licenses" / "onnxruntime" / "LICENSE.txt").unlink()
        (layout.root / "scripts" / "fetch_models.py").unlink()
        for path in (layout.data_dir / "licenses" / "opencv" / "3rdparty").iterdir():
            path.unlink()
        problems = package_tool.layout_problems(layout, self.manifest, False)
        self.assertEqual(len(problems), 3, problems)

    def test_models_only_with_flag(self):
        layout = staged(self.root / "s")
        write(layout.data_dir / "models" / "yunet" / "1" / "m.onnx", MODEL)
        problems = package_tool.layout_problems(layout, self.manifest, False)
        self.assertEqual(len(problems), 2, problems)
        self.assertTrue(any("without --with-models" in p for p in problems))

    def test_bundled_models_are_checked(self):
        layout = staged(self.root / "s")
        cache = self.root / "cache"
        write(cache / "models" / "yunet" / "1" / "m.onnx", MODEL)
        licenses = self.root / "lic"
        write(licenses / package_tool.MODEL_LICENSES["yunet"], b"MIT")
        package_tool.bundle_models(layout, self.manifest, cache, licenses)
        self.assertEqual(package_tool.layout_problems(layout, self.manifest, True), [])
        write(layout.data_dir / "models" / "yunet" / "1" / "m.onnx", b"tampered")
        write(layout.data_dir / "models" / "extra" / "x.onnx")
        problems = package_tool.layout_problems(layout, self.manifest, True)
        self.assertEqual(len(problems), 2, problems)

    def test_bundle_refuses_a_bad_cache(self):
        layout = staged(self.root / "s")
        cache = self.root / "cache"
        write(cache / "models" / "yunet" / "1" / "m.onnx", b"wrong")
        with self.assertRaises(package_tool.PackageError):
            package_tool.bundle_models(layout, self.manifest, cache, self.root)
        self.assertFalse((layout.data_dir / "models").exists())


class ArchiveTest(unittest.TestCase):
    def test_archives_keep_one_folder_and_modes(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp) / "stage" / "obs-spg-9.9.9-test"
            module = write(stage / "obs-spg" / "bin" / "64bit" / "obs-spg.so", b"\x7fELF")
            module.chmod(0o755)
            write(stage / "INSTALL.txt", b"install")
            out = Path(tmp) / "dist"
            tgz = package_tool.archive(stage, out, "linux-x64")
            with tarfile.open(tgz) as tar:
                members = {m.name: m for m in tar.getmembers()}
            self.assertTrue(all(n.split("/")[0] == stage.name for n in members))
            entry = members[f"{stage.name}/obs-spg/bin/64bit/obs-spg.so"]
            # Windows has no exec bit to set or keep; the Linux package is
            # built on Linux, where the mode must survive.
            if sys.platform != "win32":
                self.assertEqual(entry.mode & 0o777, 0o755)
            self.assertTrue(all(m.uid == 0 and m.uname == "" for m in members.values()))
            zipped = package_tool.archive(stage, out, "windows-x64")
            with zipfile.ZipFile(zipped) as zf:
                info = zf.getinfo(f"{stage.name}/obs-spg/bin/64bit/obs-spg.so")
                self.assertTrue(all(n.split("/")[0] == stage.name for n in zf.namelist()))
            if sys.platform != "win32":
                self.assertEqual(stat.S_IMODE(info.external_attr >> 16), 0o755)
            write(out / "unrelated.txt")
            sums = package_tool.write_sums(out).read_text(encoding="utf-8").splitlines()
            self.assertEqual([line.split("  ")[1] for line in sums], sorted([zipped.name, tgz.name]))
            self.assertEqual(sums[1].split("  ")[0], package_tool.sha256_of(zipped))


class SourcesTest(unittest.TestCase):
    """The committed notices and license texts match the pins."""

    def test_every_model_has_a_license_text(self):
        models = json.loads((ROOT / "models" / "manifest.json").read_text(encoding="utf-8"))["models"]
        licenses = ROOT / "build-aux" / "package" / "licenses"
        for name in models:
            self.assertIn(name, package_tool.MODEL_LICENSES)
            self.assertTrue((licenses / package_tool.MODEL_LICENSES[name]).is_file(), name)

    def test_notices_name_the_pinned_versions(self):
        deps = json.loads((ROOT / "deps" / "manifest.json").read_text(encoding="utf-8"))["deps"]
        notices = (ROOT / "build-aux" / "package" / "THIRD-PARTY-NOTICES.txt").read_text(encoding="utf-8")
        self.assertIn(f"ONNX Runtime {deps['onnxruntime']['version']}", notices)
        self.assertIn(f"OpenCV {deps['opencv']['version']}", notices)
        self.assertTrue((ROOT / "build-aux" / "package" / "licenses"
                         / f"opencv-{deps['opencv']['version']}-LICENSE.txt").is_file())
        json_pin = re.search(r"json/releases/download/v([\d.]+)/",
                             (ROOT / "core" / "CMakeLists.txt").read_text(encoding="utf-8"))
        self.assertIsNotNone(json_pin)
        self.assertIn(f"nlohmann/json {json_pin.group(1)}", notices)


if __name__ == "__main__":
    unittest.main()
