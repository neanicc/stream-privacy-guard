"""Offline tests for scripts/fetch_models.py and models/manifest.json. Run by
ctest (scripts.fetch_models) or directly: python3 -I tests/scripts/test_fetch_models.py
"""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("fetch_models", ROOT / "scripts" / "fetch_models.py")
assert _spec and _spec.loader
fetch_models = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fetch_models)

PAYLOAD = b"not really an onnx model, just pinned bytes"


def entry(payload: bytes = PAYLOAD, **overrides) -> dict:
    model = {
        "version": "1.0",
        "file": "tiny.onnx",
        "url": "https://example.invalid/tiny.onnx",
        "size": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "license": "MIT",
        "source": "test",
        "warmup_inputs": [[1, 3, 8, 8]],
    }
    model.update(overrides)
    return model


class FetchModelsTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.cache = Path(self._tmp.name) / "cache"
        self.dest = self.cache / "models" / "tiny" / "1.0" / "tiny.onnx"
        self.downloads: list[str] = []

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def serving(self, payload: bytes):
        def fake_download(url: str, dest: Path, attempts: int = 3) -> None:
            self.downloads.append(url)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(payload)
        return mock.patch.object(fetch_models.fetch_deps, "download", fake_download)

    def test_installs_verified_model_then_reuses_it(self) -> None:
        with self.serving(PAYLOAD):
            first = fetch_models.install("tiny", entry(), self.cache)
            second = fetch_models.install("tiny", entry(), self.cache)
        self.assertIn("installed", first)
        self.assertIn("cached", second)
        self.assertEqual(len(self.downloads), 1)
        self.assertEqual(self.dest.read_bytes(), PAYLOAD)
        self.assertEqual(list(self.dest.parent.iterdir()), [self.dest])  # no partial file left

    def test_refuses_hash_mismatch_and_installs_nothing(self) -> None:
        tampered = PAYLOAD[:-1] + b"!"  # same size, different hash
        with self.serving(tampered):
            with self.assertRaisesRegex(fetch_models.FetchError, "SHA-256"):
                fetch_models.install("tiny", entry(), self.cache)
        self.assertFalse(self.dest.exists())
        self.assertEqual(list(self.dest.parent.iterdir()), [])

    def test_refuses_size_mismatch(self) -> None:
        with self.serving(PAYLOAD + b"extra"):
            with self.assertRaisesRegex(fetch_models.FetchError, "size"):
                fetch_models.install("tiny", entry(), self.cache)
        self.assertFalse(self.dest.exists())

    def test_replaces_corrupt_cached_model(self) -> None:
        self.dest.parent.mkdir(parents=True)
        self.dest.write_bytes(b"corrupt")
        with self.serving(PAYLOAD):
            result = fetch_models.install("tiny", entry(), self.cache)
        self.assertIn("installed", result)
        self.assertEqual(self.dest.read_bytes(), PAYLOAD)

    def test_failed_replacement_keeps_nothing_unverified(self) -> None:
        self.dest.parent.mkdir(parents=True)
        self.dest.write_bytes(b"corrupt")
        with self.serving(b"also wrong"):
            with self.assertRaises(fetch_models.FetchError):
                fetch_models.install("tiny", entry(), self.cache)
        # The unverified file is gone rather than left for the runtime to load.
        self.assertFalse(self.dest.exists())
        with self.assertRaisesRegex(fetch_models.FetchError, "missing"):
            fetch_models.verify("tiny", entry(), self.cache)

    def test_verify_never_downloads(self) -> None:
        with self.serving(PAYLOAD):
            with self.assertRaisesRegex(fetch_models.FetchError, "missing"):
                fetch_models.verify("tiny", entry(), self.cache)
            fetch_models.install("tiny", entry(), self.cache)
            self.assertIn("verified", fetch_models.verify("tiny", entry(), self.cache))
        self.assertEqual(len(self.downloads), 1)

    def test_rejects_unsafe_or_incomplete_entries(self) -> None:
        cases = {
            "name": ("../escape", entry()),
            "file": ("tiny", entry(file="../../escape.onnx")),
            "version": ("tiny", entry(version="a/b")),
            "HTTPS": ("tiny", entry(url="http://example.invalid/tiny.onnx")),
            "sha256": ("tiny", entry(sha256="ABC")),
            "size": ("tiny", entry(size=0)),
            "warmup_inputs": ("tiny", entry(warmup_inputs=[[1, -1]])),
        }
        for label, (name, model) in cases.items():
            with self.subTest(label), self.serving(PAYLOAD):
                with self.assertRaisesRegex(fetch_models.FetchError, label):
                    fetch_models.install(name, model, self.cache)
        incomplete = entry()
        del incomplete["license"]
        with self.assertRaisesRegex(fetch_models.FetchError, "license"):
            fetch_models.install("tiny", incomplete, self.cache)
        self.assertEqual(self.downloads, [])

    def test_main_reports_unknown_model_and_bad_schema(self) -> None:
        manifest = Path(self._tmp.name) / "manifest.json"
        manifest.write_text(json.dumps({"schema": 1, "models": {"tiny": entry()}}), encoding="utf-8")
        err = io.StringIO()
        with redirect_stderr(err):
            code = fetch_models.main(["--cache", str(self.cache), "--manifest", str(manifest), "--only", "nope"])
        self.assertEqual(code, 1)
        self.assertIn("unknown model", err.getvalue())

        manifest.write_text(json.dumps({"schema": 2, "models": {}}), encoding="utf-8")
        err = io.StringIO()
        with redirect_stderr(err):
            self.assertEqual(fetch_models.main(["--cache", str(self.cache), "--manifest", str(manifest)]), 1)
        self.assertIn("schema", err.getvalue())

    def test_main_installs_from_manifest(self) -> None:
        manifest = Path(self._tmp.name) / "manifest.json"
        manifest.write_text(json.dumps({"schema": 1, "models": {"tiny": entry()}}), encoding="utf-8")
        out = io.StringIO()
        with self.serving(PAYLOAD), redirect_stdout(out):
            self.assertEqual(fetch_models.main(["--cache", str(self.cache), "--manifest", str(manifest)]), 0)
        self.assertIn("sha256 verified", out.getvalue())
        self.assertEqual(self.dest.read_bytes(), PAYLOAD)


class ManifestTest(unittest.TestCase):
    def test_every_model_is_pinned_and_valid(self) -> None:
        manifest = json.loads((ROOT / "models" / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], 1)
        self.assertTrue(manifest["models"])
        for name, model in manifest["models"].items():
            with self.subTest(model=name):
                fetch_models.validate(name, copy.deepcopy(model))
                self.assertTrue(model["license"])
                self.assertTrue(model["url"].endswith("/" + model["file"]), "URL names a different file")
                self.assertIsInstance(model.get("metadata", []), list)


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False, verbosity=2).result.wasSuccessful() else 1)
