# Stream Privacy Guard

An OBS Studio filter that blurs sensitive things on a live source before they air:
faces, strings you put on a watch-list (your real name, your address, alt
accounts), and structured personal data such as phone numbers, emails and card
numbers.

It is a **safety net, not a guarantee**. A short lookahead delay lets the
detector catch things before the frame is encoded, masks are applied retroactively
to every buffered frame, and when the pipeline cannot keep up it fails closed by
blurring the whole source. Some things will still get through; the eval in this
repo measures how often.

> Status: pre-alpha. The build foundation exists (`spg-core`, `spg-cli`, tests
> and pinned dependencies); the detection pipeline and the OBS plugin do not yet.
> Nothing here protects a stream today.

## Quickstart (developers)

Needs CMake 3.25+, a C++20 compiler (Xcode clang on macOS, GCC or Clang on
Linux, Visual Studio 2022 or newer on Windows), Python 3.12+ and `curl`.
Supported hosts: Linux x64, macOS on Apple Silicon, Windows x64. From Git Bash
on Windows:

```sh
bash scripts/ci/run.sh setup   # fetch pinned ONNX Runtime, OpenCV, ccache and models, SHA-256 verified
bash scripts/ci/run.sh build   # configure and build into build/ (Release, warnings as errors)
bash scripts/ci/run.sh test    # run all tests with ctest
build/cli/spg-cli --version                 # Linux and macOS
build/cli/Release/spg-cli.exe --version     # Windows (Visual Studio puts binaries under Release/)
```

`setup` downloads the archives listed in [deps/manifest.json](deps/manifest.json)
once into `$SPG_CACHE_DIR` (default `~/.cache/spg`) and refuses any whose hash
does not match. An unpacked dependency is trusted once its `.spg-fetch.json`
marker matches the manifest; delete `$SPG_CACHE_DIR/deps/<name>/` to force a
fresh unpack. Catch2 is fetched by CMake, also pinned by hash. ccache from the
manifest is used automatically and keeps its cache in `$SPG_CACHE_DIR/ccache`.
The ONNX models in [models/manifest.json](models/manifest.json) (YuNet, PP-OCR
detection and recognition) go to `$SPG_CACHE_DIR/models/` the same way, through
`scripts/fetch_models.py`; `--verify` rechecks the cache without downloading.
Weights never enter Git, and nothing downloads them at runtime.
To build without ONNX Runtime and OpenCV, configure with `-DSPG_WITH_DEPS=OFF`.
[docs/ci.md](docs/ci.md) describes the CI checks these commands mirror.

## How it works

1. The filter reads back a downscaled copy of each frame.
2. Worker threads run a face detector (YuNet) and a text detector and recognizer
   (PP-OCR via ONNX Runtime).
3. A policy engine fuzzy-matches recognized text against your watch-list and
   checks it against PII patterns.
4. A tracker keeps masks on moving content and holds them briefly after the last
   detection.
5. Full-resolution frames wait in a lookahead buffer; masks found later are
   applied to earlier buffered frames, then a GPU effect blurs the masked regions.

Details: [docs/plan.md](docs/plan.md). Accuracy targets and how they are
measured: [docs/eval.md](docs/eval.md) and [eval/thresholds.toml](eval/thresholds.toml).

## Privacy

Runs entirely on your machine. No accounts, telemetry or network calls at
runtime. Watch-list entries are never logged. Models are downloaded once from
pinned URLs and verified by SHA-256.

## How this repository is built

Most code here is written by Claude Code agents working through GitHub issues
and pull requests. Each PR gets CI on Linux, macOS and Windows, a Claude staff
review against [AGENTS.md](AGENTS.md), and a Greptile review. A merge gate
decides what lands; a human owns the thresholds, CI workflows and live testing.
See [docs/autopilot.md](docs/autopilot.md) for the workflow and
[docs/agent-log.md](docs/agent-log.md) for the decision history.

## License

GPL-2.0, because OBS plugins link against libobs. See [LICENSE](LICENSE).
