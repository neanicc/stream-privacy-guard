"""Tests for the eval suite generators and the pinned eval downloads.
Run by ctest (scoring.generators) or directly: python3 -I tests/scripts/test_generators.py

Pillow comes from eval/pydeps.json: fetched once into $SPG_CACHE_DIR (pinned,
SHA-256 verified) and reused. The faces test uses a fake dataset of drawn
images, never the real one.
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import random
import shutil
import sys
import tarfile
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "eval"))

from spg_eval import fetch  # noqa: E402

CACHE = Path(os.environ.get("SPG_CACHE_DIR") or Path.home() / ".cache" / "spg").expanduser().resolve()
fetch.use_pydeps(CACHE)

from PIL import Image, ImageDraw  # noqa: E402

from spg_eval import contracts, faces_suite, scoring, suite, synth, text_suite  # noqa: E402

THRESHOLDS = scoring.load_thresholds(ROOT / "eval" / "thresholds.toml")


class SynthTests(unittest.TestCase):
    def test_cards_pass_luhn_and_decoy_cards_do_not(self) -> None:
        def luhn_ok(number: str) -> bool:
            digits = number.replace(" ", "")
            return synth.luhn_digit(digits[:-1]) == digits[-1]

        rng = random.Random(1)
        for _ in range(200):
            self.assertTrue(luhn_ok(synth.card(rng)))
            self.assertFalse(luhn_ok(synth.card(rng, valid=False)))

    def test_phones_match_the_policy_formats(self) -> None:
        import re
        nanp = re.compile(r"^(\([2-9]\d\d\) |[2-9]\d\d-)[2-9]\d\d-\d{4}$")
        e164 = re.compile(r"^\+1[2-9]\d\d[2-9]\d{6}$")
        rng = random.Random(2)
        for _ in range(200):
            phone = synth.pii(rng, "pii:phone")
            self.assertTrue(nanp.match(phone) or e164.match(phone), phone)

    def test_edit_distance(self) -> None:
        self.assertEqual(synth.edit_distance("kitten", "sitting"), 3)
        self.assertEqual(synth.edit_distance("", "abc"), 3)
        self.assertFalse(synth.far_from("Varen Toskel", ["Varen Toskell"]))  # one edit: a match
        self.assertFalse(synth.far_from("Varenx", ["Varen Toskell"]))  # shares a 5-character prefix

    def test_watchlist_lookalikes_are_far_from_every_entry(self) -> None:
        rng = random.Random(3)
        for _ in range(50):
            entries = synth.watchlist(rng)
            for _ in range(20):
                text = synth.lookalike(rng, entries)
                self.assertNotIn(text, entries)
                for entry in entries:
                    # Beyond the default fuzzy limit min(2, 0.2 n), and no shared 5-character prefix.
                    self.assertGreater(synth.edit_distance(text.lower(), entry.lower()),
                                       min(2, int(0.2 * len(entry))))
                    for ta in text.lower().split():
                        self.assertFalse(any(tb[:5] == ta[:5] for tb in entry.lower().split() if len(tb) >= 5))


class TextSuiteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = Path(cls.tmp.name)
        cls.index = text_suite.generate(cls.out / "a", 7, "ci", frames=False)
        cls.truth = suite.load_truth(cls.out / "a")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_same_seed_gives_byte_identical_ground_truth(self) -> None:
        text_suite.generate(self.out / "b", 7, "ci", frames=False)
        text_suite.generate(self.out / "c", 8, "ci", frames=False)
        for seq in self.index["sequences"]:
            first = (self.out / "a" / seq["name"] / suite.TRUTH).read_bytes()
            self.assertEqual(first, (self.out / "b" / seq["name"] / suite.TRUTH).read_bytes())
        self.assertEqual((self.out / "a" / "policy.json").read_bytes(), (self.out / "b" / "policy.json").read_bytes())
        self.assertNotEqual((self.out / "a" / "seq-000" / suite.TRUTH).read_bytes(),
                            (self.out / "c" / "seq-000" / suite.TRUTH).read_bytes())

    def test_suite_covers_the_planned_content(self) -> None:
        seqs = self.index["sequences"]
        self.assertEqual({(s["width"], s["height"]) for s in seqs}, {(1920, 1080), (1280, 720)})
        self.assertEqual({s["template"] for s in seqs}, set(text_suite.TEMPLATES))
        self.assertTrue(any(s["clean"] for s in seqs))
        kinds = {i.kind for frames in self.truth.values() for f in frames for i in f.instances if i.visibility > 0}
        self.assertTrue({"watchlist", "decoy", "pii:email", "pii:phone", "pii:card", "pii:ip", "pii:token"} <= kinds,
                        kinds)
        # Motion: partial reveal, fades and clipping all produce fractional visibility.
        self.assertTrue(any(0 < i.visibility < 1 for frames in self.truth.values() for f in frames
                            for i in f.instances))
        policy = json.loads((self.out / "a" / "policy.json").read_text())
        self.assertEqual(policy["schema"], 1)
        self.assertEqual(len(policy["watchlist"]), 4)

    def test_ground_truth_is_consistent(self) -> None:
        for name, frames in self.truth.items():
            seq = next(s for s in self.index["sequences"] if s["name"] == name)
            self.assertEqual([f.frame for f in frames], list(range(seq["frames"])))
            ids = None
            for f in frames:
                self.assertEqual(f.timestamp_ns, f.frame * 1_000_000_000 // 60)
                self.assertEqual((f.width, f.height), (seq["width"], seq["height"]))
                # Every planted item is listed on every frame of its sequence.
                ids = ids or {i.id for i in f.instances}
                self.assertEqual({i.id for i in f.instances}, ids)
                for i in f.instances:
                    if i.visibility > 0:
                        self.assertGreaterEqual(i.box.x, 0)
                        self.assertGreaterEqual(i.box.y, 0)
                        self.assertLessEqual(i.box.x + i.box.w, f.width)
                        self.assertLessEqual(i.box.y + i.box.h, f.height)
                        self.assertGreater(i.box.w * i.box.h, 0)
            if seq["clean"]:
                self.assertTrue(all(i.kind == "decoy" for f in frames for i in f.instances))

    def test_reference_pipelines(self) -> None:
        def report(pipeline):
            return scoring.evaluate(THRESHOLDS, {"text": suite.reference_run(self.out / "a", pipeline)}, "ci")

        noop = scoring.failed_limits(report("noop"), "text")
        self.assertTrue({"watchlist_pair_recall", "pii_pair_recall", "watchlist_first_frame_recall"} <= set(noop))
        self.assertIn("clean_frame_blur_area", scoring.failed_limits(report("blur_all"), "text"))
        oracle = report("oracle")
        self.assertEqual(oracle.verdict, "THRESHOLDS: PASS", oracle.lines)

    def test_frames_match_ground_truth(self) -> None:
        index = text_suite.generate(self.out / "frames", 7, "ci", frames=True)
        for seq in index["sequences"][:2]:
            frames = sorted((self.out / "frames" / seq["frames_dir"]).glob("*.png"))
            self.assertEqual(len(frames), seq["frames"])
            with Image.open(frames[-1]) as img:
                self.assertEqual((img.mode, img.size), ("RGB", (seq["width"], seq["height"])))
        # Rendering frames does not change the ground truth.
        self.assertEqual((self.out / "frames" / "seq-000" / suite.TRUTH).read_bytes(),
                         (self.out / "a" / "seq-000" / suite.TRUTH).read_bytes())


class FacesSuiteTests(unittest.TestCase):
    FACE_BOX = {"x": 68, "y": 68, "w": 114, "h": 114}
    FACE = (250, 20, 200)

    def test_faces_land_where_ground_truth_says(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp) / "data"
            for k in range(3):
                (data / f"p{k}").mkdir(parents=True)
                img = Image.new("RGB", (250, 250), (20, 120, 20))
                b = self.FACE_BOX
                ImageDraw.Draw(img).rectangle((b["x"], b["y"], b["x"] + b["w"] - 1, b["y"] + b["h"] - 1),
                                              fill=self.FACE)
                img.save(data / f"p{k}" / f"p{k}_0001.png")
            dataset = {"images": "*/*.png", "face_box": self.FACE_BOX}
            out = Path(tmp) / "faces"
            index = faces_suite.generate(out, 5, data, dataset, "ci")
            truth = suite.load_truth(out)
            seen = 0
            for seq in index["sequences"]:
                frames = truth[seq["name"]]
                if seq["clean"]:
                    self.assertTrue(all(not f.instances for f in frames))
                    continue
                for f in frames[:: max(1, len(frames) // 3)]:
                    with Image.open(out / seq["frames_dir"] / f"{f.frame:06d}.png") as img:
                        for i in f.instances:
                            if i.visibility < 1:
                                continue
                            self.assertEqual(i.kind, "face")
                            cx, cy = int(i.box.x + i.box.w / 2), int(i.box.y + i.box.h / 2)
                            self.assertLess(max(abs(a - b) for a, b in zip(img.getpixel((cx, cy)), self.FACE)), 40)
                            seen += 1
            self.assertGreater(seen, 0)
            again = faces_suite.generate(Path(tmp) / "again", 5, data, dataset, "ci", frames=False)
            self.assertEqual(index, {**again})
            for seq in index["sequences"]:
                self.assertEqual((out / seq["name"] / suite.TRUTH).read_bytes(),
                                 (Path(tmp) / "again" / seq["name"] / suite.TRUTH).read_bytes())


class FetchTests(unittest.TestCase):
    def test_pinned_wheels_cover_ci_and_supported_platforms(self) -> None:
        packages = json.loads((ROOT / "eval" / "pydeps.json").read_text())["packages"]
        for name, package in packages.items():
            for plat in ("linux-x64", "macos-arm64", "windows-x64"):
                self.assertIn(f"cp312-{plat}", package["wheels"], name)
            for key, wheel in package["wheels"].items():
                self.assertTrue(wheel["url"].startswith("https://files.pythonhosted.org/"), key)
                self.assertRegex(wheel["sha256"], r"^[0-9a-f]{64}$")
                self.assertIn(package["version"], wheel["url"])

    def test_every_dataset_is_pinned_with_a_license(self) -> None:
        manifest = json.loads((ROOT / "models" / "manifest.json").read_text())
        self.assertIn("lfw", manifest["datasets"])
        for name in manifest["datasets"]:
            entry = fetch.dataset_entry(name)
            self.assertTrue(entry["license"])

    def _archive(self, tmp: Path) -> tuple[Path, bytes]:
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tf:
            data = b"fake image"
            info = tarfile.TarInfo("set/a/a_0001.jpg")
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
        return tmp / "set.tgz", buf.getvalue()

    def _manifest(self, tmp: Path, payload: bytes, sha: str | None = None) -> Path:
        entry = {"purpose": "test", "version": "1", "url": "https://example.invalid/set.tgz", "size": len(payload),
                 "sha256": sha or hashlib.sha256(payload).hexdigest(), "license": "test", "source": "test",
                 "root": "set", "images": "*/*.jpg", "face_box": {"x": 0, "y": 0, "w": 1, "h": 1}}
        path = tmp / "manifest.json"
        path.write_text(json.dumps({"schema": 1, "models": {}, "datasets": {"tiny": entry}}))
        return path

    def test_dataset_is_verified_then_unpacked_and_reused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = Path(tmp_name)
            _, payload = self._archive(tmp)
            calls = []

            def fake_download(url, dest, attempts=3):
                calls.append(url)
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(payload)

            with mock.patch.object(fetch.fetch_deps, "download", fake_download):
                root = fetch.ensure_dataset("tiny", tmp / "cache", self._manifest(tmp, payload))
                self.assertTrue((root / "a" / "a_0001.jpg").is_file())
                self.assertEqual(fetch.ensure_dataset("tiny", tmp / "cache", self._manifest(tmp, payload)), root)
            self.assertEqual(len(calls), 1)

    def test_dataset_hash_mismatch_installs_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = Path(tmp_name)
            _, payload = self._archive(tmp)

            def fake_download(url, dest, attempts=3):
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(payload)

            with mock.patch.object(fetch.fetch_deps, "download", fake_download), redirect_stdout(io.StringIO()):
                with self.assertRaises(fetch.FetchError):
                    fetch.ensure_dataset("tiny", tmp / "cache", self._manifest(tmp, payload, "0" * 64))
            self.assertFalse((tmp / "cache" / "deps" / "dataset-tiny").exists()
                             and any((tmp / "cache" / "deps" / "dataset-tiny").iterdir()))

    def test_unpinned_interpreter_is_refused(self) -> None:
        with mock.patch.object(fetch, "interpreter_tag", return_value="cp27"):
            with self.assertRaises(fetch.FetchError):
                fetch.ensure_pydeps(Path(tempfile.gettempdir()) / "spg-never-used")
        shutil.rmtree(Path(tempfile.gettempdir()) / "spg-never-used", ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
