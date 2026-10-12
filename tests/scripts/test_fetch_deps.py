"""Offline tests for scripts/fetch_deps.py and deps/manifest.json. Run by ctest
(scripts.fetch_deps) or directly: python3 -I tests/scripts/test_fetch_deps.py
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("fetch_deps", ROOT / "scripts" / "fetch_deps.py")
assert _spec and _spec.loader
fetch_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fetch_deps)

URL = "https://example.invalid/pkg-1.0.tar.gz"


def make_tar(members: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def make_zip(members: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, data in members.items():
            zf.writestr(name, data)
    return buf.getvalue()


class FetchDepsTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.cache = Path(self._tmp.name) / "cache"
        (self.cache / "downloads").mkdir(parents=True)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def seed(self, data: bytes, url: str = URL) -> str:
        (self.cache / "downloads" / url.rsplit("/", 1)[1]).write_bytes(data)
        return hashlib.sha256(data).hexdigest()

    def no_network(self, *_args, **_kwargs):
        self.fail("unexpected download")

    def test_unpacks_verified_archive_and_reuses_it(self) -> None:
        sha = self.seed(make_tar({"pkg-1.0/include/pkg.h": b"// header\n"}))
        entry = {"url": URL, "sha256": sha, "root": "pkg-1.0"}
        with mock.patch.object(fetch_deps, "download", self.no_network):
            first = fetch_deps.install("pkg", "1.0", "linux-x64", entry, self.cache, True)
            second = fetch_deps.install("pkg", "1.0", "linux-x64", entry, self.cache, True)
        dest = self.cache / "deps" / "pkg" / "1.0-linux-x64"
        self.assertIn("unpacked", first)
        self.assertIn("cached", second)
        self.assertEqual((dest / "include" / "pkg.h").read_bytes(), b"// header\n")
        marker = json.loads((dest / fetch_deps.MARKER).read_text(encoding="utf-8"))
        self.assertEqual(marker["sha256"], sha)

    def test_refuses_hash_mismatch_and_discards_download(self) -> None:
        good = make_tar({"pkg-1.0/a": b"a"})
        expected = hashlib.sha256(good).hexdigest()

        def tampered(_url: str, dest: Path, attempts: int = 3) -> None:
            dest.write_bytes(good + b"tampered")

        entry = {"url": URL, "sha256": expected, "root": "pkg-1.0"}
        with mock.patch.object(fetch_deps, "download", tampered):
            with self.assertRaisesRegex(fetch_deps.FetchError, "SHA-256 mismatch"):
                fetch_deps.install("pkg", "1.0", "linux-x64", entry, self.cache, True)
        self.assertFalse((self.cache / "downloads" / "pkg-1.0.tar.gz").exists())
        self.assertFalse((self.cache / "deps" / "pkg" / "1.0-linux-x64").exists())

    def test_redownloads_when_cached_archive_is_corrupt(self) -> None:
        good = make_tar({"pkg-1.0/a": b"a"})
        expected = hashlib.sha256(good).hexdigest()
        self.seed(b"corrupt")
        calls = []

        def fresh(_url: str, dest: Path, attempts: int = 3) -> None:
            calls.append(dest)
            dest.write_bytes(good)

        entry = {"url": URL, "sha256": expected, "root": "pkg-1.0"}
        with mock.patch.object(fetch_deps, "download", fresh):
            fetch_deps.install("pkg", "1.0", "linux-x64", entry, self.cache, True)
        self.assertEqual(len(calls), 1)

    def test_rejects_tar_escaping_destination(self) -> None:
        sha = self.seed(make_tar({"../escape.txt": b"x"}))
        entry = {"url": URL, "sha256": sha, "root": "."}
        with self.assertRaises(fetch_deps.FetchError):
            fetch_deps.install("pkg", "1.0", "linux-x64", entry, self.cache, True)
        self.assertFalse((self.cache / "deps" / "pkg" / "escape.txt").exists())

    def test_rejects_zip_escaping_destination(self) -> None:
        url = "https://example.invalid/pkg-1.0.zip"
        sha = self.seed(make_zip({"../escape.txt": b"x"}), url)
        entry = {"url": url, "sha256": sha, "root": "."}
        with self.assertRaisesRegex(fetch_deps.FetchError, "unsafe paths"):
            fetch_deps.install("pkg", "1.0", "windows-x64", entry, self.cache, True)

    def test_missing_root_is_an_error(self) -> None:
        sha = self.seed(make_tar({"other/a": b"a"}))
        entry = {"url": URL, "sha256": sha, "root": "pkg-1.0"}
        with self.assertRaisesRegex(fetch_deps.FetchError, "not found"):
            fetch_deps.install("pkg", "1.0", "linux-x64", entry, self.cache, True)

    def test_refuses_plain_http(self) -> None:
        with self.assertRaisesRegex(fetch_deps.FetchError, "non-HTTPS"):
            fetch_deps.download("http://example.invalid/x.tgz", self.cache / "x.tgz")


class ManifestTest(unittest.TestCase):
    def test_every_entry_is_pinned_for_every_platform(self) -> None:
        manifest = json.loads((ROOT / "deps" / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], 1)
        platforms = manifest["platforms"]
        self.assertTrue(manifest["deps"])
        for name, dep in manifest["deps"].items():
            with self.subTest(dep=name):
                self.assertRegex(dep["version"], r"^\d+\.\d+\.\d+$")
                self.assertTrue(dep["license"])
                self.assertIn(dep["kind"], ("library", "tool"))
                self.assertEqual(sorted(dep["platforms"]), sorted(platforms))
                for plat, entry in dep["platforms"].items():
                    self.assertTrue(entry["url"].startswith("https://"), plat)
                    self.assertIn(dep["version"], entry["url"], plat)
                    self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$", plat)
                    self.assertIn("root", entry, plat)


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False, verbosity=2).result.wasSuccessful() else 1)
