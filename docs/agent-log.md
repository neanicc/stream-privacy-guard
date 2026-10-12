# Agent development log

Each PR that makes a substantive decision, design change or measurement appends
one entry at the end of this file in the same PR. Keep entries short and factual.
Earlier entries are history, not current instructions.

Template:

```
## YYYY-MM-DD — SPG-xx <title> (#PR)
- Decision / change:
- Evidence: commands run and results (automated / synthetic eval / live / human)
- Eval delta (if any): metric before → after
- Open issues or follow-ups:
```

## 2026-10-12 — Scaffold

- Decision / change: repository created with agent workflows (adapted from the
  Trail/teachAR delivery skills), plan, lanes, eval definitions, human-owned
  thresholds, CI structure (`check` aggregate, protected-path guard, actionlint,
  build/test/eval matrix calling `scripts/ci/run.sh`), Claude staff review on PRs
  and Greptile configuration.
- Evidence: `python3 scripts/check_agent_files.py` and actionlint passed on the
  scaffold. No product code exists yet.
- Open issues: plan section 12 open questions; license confirmed at bootstrap.

## 2026-10-12 — SPG-01 Build foundation (#20)

- Decision / change: root CMake project (C++20, CMake 3.25+) with `spg-core`,
  `spg-cli` and tests; Catch2 v3.16.0 via FetchContent pinned by SHA-256;
  `COMPILE_WARNING_AS_ERROR` plus `-Wall -Wextra -Wpedantic -Wshadow
  -Wconversion -Wsign-conversion -Wold-style-cast` (`/W4 /permissive-` on MSVC)
  on SPG targets only, third-party headers as SYSTEM. `deps/manifest.json` pins
  ONNX Runtime 1.30.0 (official CPU packages; `osx-arm64`, since 1.30 ships no
  universal2), OpenCV 4.9.0 (occ-ai minimal static build, plan question 5
  default) and ccache 4.13.6, each per platform (`linux-x64`, `macos-arm64`,
  `windows-x64`) with SHA-256. `scripts/fetch_deps.py` downloads with curl
  (urllib fallback), verifies the hash, unpacks safely into
  `$SPG_CACHE_DIR/deps/<name>/<version>-<platform>/` and writes a marker that
  CMake checks against the manifest. ccache runs as a compiler launcher for
  Ninja/Makefiles and masquerades as `cl.exe` for Visual Studio (ccache wiki
  method); its cache lives in `$SPG_CACHE_DIR/ccache` so CI restores it.
  macOS deployment target defaults to 14.0 because the ONNX Runtime dylib
  requires it. The `windows-2025` runner now ships Visual Studio 2026 (MSVC
  19.51) and no 2022, so `run.sh build` asks vswhere for the newest Visual Studio
  with C++ tools (`SPG_CMAKE_GENERATOR` overrides); the vc17 OpenCV static libs
  link fine with it. Review follow-up: `fetch_deps.py` holds a per-dependency
  OS file lock (`$SPG_CACHE_DIR/locks/`) from the cache check through the
  marker write, because worktrees share one cache and two concurrent `setup`
  runs could otherwise replace each other's install; the script tests require
  Python 3.12, matching CI and the repository's Python version.
- Evidence (automated, dev Mac, macOS 26.5 arm64, AppleClang 21): every one of
  the 9 pinned archives downloaded and matched its SHA-256
  (`fetch_deps.py --platform all`); `run.sh setup`, `build`, `test` pass with 12
  ctest tests (version, ONNX Runtime version and CPU/CoreML providers, OpenCV
  version and blur, spg-cli exit codes, fetch script hash-mismatch and
  path-traversal refusal); a second build in a fresh directory hit ccache
  112/112; `-DSPG_WITH_DEPS=OFF` builds and passes 10 tests; `run.sh eval`
  still prints `THRESHOLDS: NOT RUN`. CI Check run 38221963583 (head
  `43594ab`) passed on Ubuntu 24.04 (GCC 13.3, Ninja), macOS 15 (AppleClang 17)
  and Windows 2025 (MSVC 19.51, VS 2026), 12/12 tests each; ccache served 112
  of 112 compiles from the restored cache on Ubuntu and handled all 112
  compiles through the `cl.exe` substitution on Windows.
- Eval delta: none (no pipeline yet).
- Open issues or follow-ups: the Windows ONNX Runtime package is CPU-only
  (DirectML/CUDA are separate packages, for SPG-04 to decide); the OpenCV
  prebuilt dates from March 2024, so a refresh or source build may be needed
  later; OBS plugin builds may need the OpenCV libraries checked
  for position-independent code on Linux (SPG-13).

## 2026-10-12 — SPG-04 Model manifest and runtime (#24)

- Decision / change: `models/manifest.json` pins three ONNX models by URL,
  size, SHA-256 and license: YuNet `face_detection_yunet_2023mar` (MIT,
  Hugging Face `opencv/face_detection_yunet` at a commit), and PP-OCR English
  mobile detection `en_PP-OCRv3_det_mobile` and recognition
  `en_PP-OCRv4_rec_mobile` (Apache-2.0, RapidOCR v3.9.2 ONNX conversions on
  ModelScope; the hashes equal RapidOCR's own `default_models.yaml`). This is
  plan question 6's default; SPG-06/07/12 may swap variants (for example
  PP-OCRv5) through the manifest. The recognizer embeds its character
  dictionary in metadata key `character`, so no separate dictionary file is
  pinned. `scripts/fetch_models.py` reuses `fetch_deps.py`'s download, hashing
  and per-item OS lock, verifies size and SHA-256 on a temporary file before
  moving it to `$SPG_CACHE_DIR/models/<name>/<version>/<file>`, deletes a
  cached file that no longer matches, and has `--verify` (no download).
  `run.sh setup` already called it, so CI fetches the models (about 10 MB)
  into the cached `~/.cache/spg`.
- Decision / change: `spg::runtime` (`core/src/runtime/`, target
  `spg-runtime`) wraps ONNX Runtime behind a header with no ORT types.
  `Session::open` tries providers in order (default: CoreML on macOS,
  DirectML then CUDA on Windows, CUDA on Linux), skips ones the loaded build
  lacks, treats a provider as working only after warmup runs succeed, then
  falls back to CPU unless `cpu_fallback` is off. Failures are `RuntimeError`
  with codes (`ModelNotFound`, `ModelLoadFailed`, `ProviderUnavailable`,
  `InvalidInput`, `InferenceFailed`) and messages naming the model path,
  provider attempts, tensor names and shapes, never tensor values. Dynamic
  inputs need explicit warmup shapes (no guessed sizes). `run()` is const and
  safe to call from several threads. CoreML uses MLProgram, compute units ALL
  and `RequireStaticInputShapes=1`: without the last option the
  dynamic-shape PP-OCR models spent 3.4 s (det) and 4.4 s (rec) compiling
  for CoreML, logged an "unbounded dimension" error per layer and ran slower
  than CPU (single runs: det 40.7 ms vs 23.6 ms, rec 22.0 ms vs 6.5 ms).
- Decision / change (Windows GPU): keep the pinned CPU package on Windows in
  this PR. DirectML is the vendor-neutral choice the plan names first, but
  Microsoft's DirectML ONNX Runtime builds stop at 1.24.4 (NuGet
  `Microsoft.ML.OnnxRuntime.DirectML`), so it would mean a second ORT version
  on Windows or a move to Windows ML execution-provider plugins. ORT 1.30.0
  does ship `win-x64-gpu_cuda12/13` zips, but they need NVIDIA GPUs plus a
  user-installed CUDA and cuDNN. Neither can be validated on GPU-less CI
  runners. The API already names `DirectMl` and `Cuda` (appended through
  ORT's generic `"DML"` registration and `AppendExecutionProvider_CUDA`), so
  packaging is a manifest and CMake change; proposed as a follow-up.
- Evidence (automated, dev Mac: Apple M4, macOS 26.5.2, AppleClang 21):
  `fetch_models.py` downloaded all three models and matched size and SHA-256;
  second run reported all cached; `--verify` passed. `run.sh setup`,
  `build`, `test`: 24/24 ctest tests pass, including 11 `runtime.*` cases
  (tiny Mul models generated at build time by
  `tests/integration/make_runtime_fixtures.py`, no binaries in Git) and
  `scripts.fetch_models` (10 offline tests: hash and size mismatch refused
  and nothing installed, corrupt cache replaced, unsafe names rejected,
  manifest valid). A fresh build against a cache without models reports the
  pinned-model case as Skipped; `-DSPG_WITH_DEPS=OFF` builds and passes
  11 tests. Measurement on the M4 (scratch harness over this wrapper, 50
  runs, constant 0.5 inputs, 3 warmup runs; not a `bench` result):
  YuNet 640x640 CoreML median 2.79 ms (open+warmup 258 ms) vs CPU 4.62 ms;
  det 640x640 CoreML-registered 24.4 ms vs CPU 23.6 ms; rec 48x320 5.5 ms vs
  5.1 ms. With static-shape enforcement CoreML takes no PP-OCR nodes, so
  those two run on CPU either way.
- Review fixes: `warmup()` now checks every warmup shape against the model
  (rank, fixed dimensions, positive dimensions, overflow-safe element count)
  with the same helper as `run()` before allocating any buffer, so a wrong or
  huge shape is `InvalidInput` instead of a multi-GB allocation; opening a
  session with a moved-from `Environment`, or calling any member of a
  moved-from `Session`, is `InvalidInput`, not a null dereference (so the
  `Session` accessors are no longer `noexcept`). The default-provider tests
  now check the recorded attempts
  (earlier providers failed with a reason, the selected one is last) so a
  listed GPU provider that fails on a machine is not a false failure; macOS
  still requires CoreML. Dev Mac: 25/25 ctest tests, 12 `runtime.*` cases.
- Eval delta: none (no pipeline yet; `run.sh eval` still prints
  `THRESHOLDS: NOT RUN`).
- Open issues or follow-ups: package a Windows GPU provider (DirectML via
  1.24.4 or Windows ML, or CUDA) with a Windows GPU test machine; give the
  PP-OCR detector fixed tile shapes (or ORT free-dimension overrides) so
  CoreML can accelerate it (SPG-06/SPG-16); `Session::provider()` names the
  registered provider, and ORT may still place unsupported nodes on CPU;
  `run()` allocates its input values and copies every output per call, so
  SPG-05/06 should measure that in `bench` and decide whether to add a
  caller-owned-output (or `Ort::IoBinding`) overload.

## 2026-10-12 — SPG-02 Core contracts and geometry (#23)

- Decision / change: plan section 5 types as headers in `core/include/spg/`
  (`time`, `frame`, `geometry`, `category`, `detection`, `mask`,
  `policy_config`, `ground_truth`, `secret`, `error`, `json`), documented in
  `docs/contracts.md`. Choices the plan left open: `Timestamp` is int64
  nanoseconds on an arbitrary monotonic epoch with an injected `Clock`
  (`ManualClock` for tests); `FrameView` carries up to three plane pointers and
  strides because NV12 and I420 are planar; one `Category` enum serves enabled
  categories, verdict reason codes (`WATCHLIST`, `PII_CARD`, ...) and
  ground-truth kinds (`watchlist`, `pii:card`, ..., plus `decoy`); `ModelInput`
  is a crop of the readback frame resized by stretch or letterbox (center or
  top-left), so tiles and whole-frame models share one mapping;
  `core/src/frame/geometry.cpp` is the only space converter and computes in
  double; `enclosing_pixels` rounds outward so pixel masks never shrink.
  Watch-list entries and OCR text are a `Secret` type with no stream operator or
  implicit conversion. JSON uses nlohmann/json 3.12.0 (MIT) via FetchContent
  pinned by SHA-256 (`json.tar.xz`, 42f6e95c...), linked privately into
  `spg-core`. Parsing is strict (schema 1, unknown and duplicate fields
  rejected, ranges checked) and errors never forward library text, which can
  quote input. Mask plan JSON is defined now too because the SPG-03 scorer reads
  it. Defaults with no plan value: fuzzy `max_edits` 2, `max_edit_ratio` 0.2,
  `min_partial_chars` 6; margin 4 px + 0.25 x min(w, h); visibility > 0 counts
  as visible. SPG-08/09/03 may tune these with eval evidence.
- Evidence (automated, dev Mac, macOS 26.5 arm64, AppleClang 21):
  `scripts/ci/run.sh setup`, `build`, `test` pass with 36 ctest tests (24 new
  unit tests: round trips between every pair of spaces over 5000 seeded
  mappings, direct vs chained conversion, containment, outward pixel rounding,
  JSON round trips and strictness, fixtures, sentinel test). Mutation check:
  forwarding the JSON library's parse message made the sentinel test fail.
- Review fixes: `convert` now rounds outward when narrowing to float (rounding
  each field to nearest could drop an edge pixel column), so round trips never
  shrink a box and the round-trip test checks that one-sided bound; JSON float
  fields are range-checked before narrowing (`1.00000001` no longer rounds into
  a valid visibility, `-1e-50` no longer passes as a width or margin); blank
  watch-list detection covers Unicode whitespace and zero-width characters;
  `hold_last_scanned` is documented as re-emitting the last released (masked)
  frame, falling back to `blur_source` when no such frame exists for the
  current source size and content. Zero lookahead, hold and margin stay valid; a floor is left to
  SPG-09/10.
- Eval delta: none; no pipeline stage exists yet (`run.sh eval` still prints
  `THRESHOLDS: NOT RUN`).
- Open issues or follow-ups: SPG-08 owns the normalization and matching that
  interpret `fuzzy`; SPG-09 owns the margin formula; SPG-03 should read the
  committed fixtures in `tests/fixtures/contracts/` to keep the Python side in
  step with the C++ parser.

## 2026-10-12 — SPG-08 Policy engine (#PR)

- Decision / change: `spg::PolicyEngine` (`core/include/spg/policy.hpp`,
  `core/src/policy/`) turns recognized text into a `Verdict` of reason codes;
  rules in `docs/policy.md`. Choices the plan left open: (1) no ICU or other new
  dependency; folding uses a generated table (`make_fold_table.py`, Python
  3.13 / Unicode 15.1) covering Latin, Greek, Cyrillic, combining marks,
  super/subscripts, letterlike and enclosed forms, ligatures, fullwidth and
  mathematical alphanumerics, mapping each to casefold(NFKC) without
  diacritics. It is not full NFKC. (2) OCR confusions merged in matching form:
  0/o, 1/i/l/|, 5/s, 8/b; punctuation separates words. (3) Fuzzy rules on the
  SPG-02 limits: an entry shorter than `min_partial_chars` matches whole words
  only (otherwise a 3-letter name with i->l merging hits "all" inside common
  words); a longer entry matches anywhere, even inside a handle; a partial
  reveal is a prefix from a word start or a suffix to a word end of at least
  `min_partial_chars`, with its own `k(m)` edit budget. (4) PII: NANP needs
  3-3-4 group breaks (a "2026-10-12 12" timestamp otherwise read as a phone);
  cards are Luhn plus 13-19 digits with no issuer check; tokens are a fixed
  prefix list including Twitch stream keys. (5) An entry with no letters or
  digits after normalization makes the engine throw (by index) rather than be
  silently ignored. (6) `LogSink` (a `std::function`) is injected rather than a
  global logger. Placeholder fuzzy defaults (2, 0.2, 6) kept per the issue.
- Evidence (automated, dev Mac, macOS 26.5 arm64, AppleClang 21):
  `scripts/ci/run.sh setup`, `build` (warnings as errors) and `test` pass, 72
  ctest tests; the new `[policy]` cases cover normalization, matching,
  partials, every PII validator with near misses, and the sentinel test.
  Mutation checks: logging the normalized entry, quoting it in a
  `ContractError`, or writing a hit to `std::clog` each failed the sentinel
  test; dropping the whole-word rule or the suffix partial failed the matcher
  tests.
- Eval delta: none measurable; no pipeline stage calls the engine yet and the
  CI stage prints `THRESHOLDS: NOT RUN`.
- Open issues or follow-ups: SPG-12 should measure the six-character, one-edit
  partial rule and short-entry collisions against the clean-frame budget;
  SPG-03's generator should emit NANP-valid phones (area code and exchange
  2-9, 3-3-4 grouping) or the scorer will count misses this validator rejects
  by design; postcodes and non-US address forms are not detected.
