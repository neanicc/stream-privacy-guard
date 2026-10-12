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

## 2026-10-12 — SPG-13 OBS plugin shell (#26)

- Decision / change: `plugin/` holds `obs-spg`, a C++20 OBS module in the
  obs-plugintemplate layout (`src/`, `data/locale/`, `buildspec.json` at the
  root for name and bundle id, helper scripts in `build-aux/`), built when
  `-DSPG_BUILD_PLUGIN=ON`; `scripts/ci/run.sh` turns it on unless
  `SPG_BUILD_PLUGIN=0`. It registers the video filter `spg_privacy_guard_filter`
  ("Privacy Guard") with a properties stub (status notice, lookahead 500 ms,
  hold 750 ms; saved, not yet used) and links `spg-core` (now built with
  `CMAKE_POSITION_INDEPENDENT_CODE`). With no pipeline yet the filter fails
  closed: `video_render` never draws its target and covers the source with an
  opaque fill (the plan's "solid fill" fallback, since the blur effect is
  SPG-14). libobs: OBS 32.1.2 for every OS. Linux links the official
  `OBS-Studio-32.1.2-Ubuntu-24.04-x86_64.deb` release asset (headers, CMake
  package and `libobs.so` only; SIMDe 0.8.2 headers alongside), unpacked by
  `fetch_deps.py`. macOS and Windows follow the template: `build-aux/build_libobs.py`
  builds `libobs` and `obs-frontend-api` from the `OBS-Studio-32.1.2-Sources`
  release against obs-deps 2025-08-23 (the version OBS 32.1.2 pins) and installs
  the Development component into `$SPG_CACHE_DIR/deps/libobs/`, cached by an
  input marker; CMake refuses a cached libobs whose marker differs from the
  current obs-studio, obs-deps (and x86 obs-deps) hashes or builder digest, and
  asks for `setup`. Every archive is in `deps/manifest.json` with SHA-256; OBS's own
  configure-time downloads (obs-deps, Qt, CEF) are disabled by an override
  `buildspec.cmake`, and the 32-bit helper configure OBS always runs on Windows
  gets the pinned x86 obs-deps archive placed in `<source>/.deps`. 32.1.2 rather
  than 32.2.2: 32.2 requires Xcode 26.5 and the `macos-15` runner tops out at
  26.3. OBS 32.1 enables Swift only from a plugin directory, so the macOS build
  injects `enable_language(Swift)` (`CMAKE_PROJECT_INCLUDE`) for libobs-metal.
  `fetch_deps.py` gained `.deb` unpacking, `include` member filters (recorded
  in the unpack marker, so changing the filter unpacks again), explicit
  `{"unused": reason}` platform entries, `"unpack": false` and `--skip-group`.
- Frame-access spike (plan question 1, recommended default kept): readback
  through `gs_texrender` plus `gs_stagesurface`, not an async filter. Display,
  window and game capture are synchronous GPU sources, and OBS documents
  `filter_video` as "only used with asynchronous video filters", so an async
  filter never sees them; a filter on them only gets `video_render`. The
  readback plan for SPG-14: in `video_render`, render the target into a
  downscaled `gs_texrender`, `gs_stage_texture` it into one of two (or three)
  stage surfaces and `gs_stagesurface_map` the surface staged on the previous
  frame, because OBS notes staging stalls unless given a frame to complete;
  the mapped pixels are copied into a bounded queue for the workers and the
  render thread never waits on them. Full-resolution frames for the lookahead
  ring are kept as textures (question 2 default). Audio: a filter cannot delay
  its source's audio, so the default is guidance, not automation: the
  properties will show the Sync Offset (`lookahead_ms`) to set on the audio
  sources tied to the filtered video (Advanced Audio Properties); the plugin
  does not change other sources' offsets itself. Nothing here was measured:
  render-thread cost and stalls need a live OBS run in SPG-14.
- OpenCV PIC check (Linux, SPG-01 note): the occ-ai
  `opencv-linux-v4.9.0-1-Release` static libraries are position-independent.
  `objdump -r` finds no `R_X86_64_32`/`R_X86_64_32S` relocations in
  `libopencv_core.a`, `libopencv_imgproc.a` or `3rdparty/libzlib.a`, and
  `libopencv_core.a` uses `R_X86_64_REX_GOTPCRELX` (1224) and `PLT32`, so they
  can be linked into the plugin module. The shell does not link OpenCV yet.
- Evidence (automated, dev Mac, macOS 26.5 arm64, Xcode 26.6): all five new
  pins downloaded and matched SHA-256 (`fetch_deps.py --platform all
  --no-extract`); the real `.deb` unpacks to 1.8 MB with the include filter;
  `run.sh setup` built libobs 32.1.2 from source with Xcode; `run.sh build`
  and `test` pass 14 tests, including `plugin.exports` (module exports every
  `obs_module_*` entry point) and `plugin.loads` (headless libobs:
  `obs_open_module`/`obs_init_module` succeed, the filter is a registered
  video filter, locale name, defaults and properties as expected);
  `SPG_BUILD_PLUGIN=0` setup/build/test pass 12 tests. CI results are in the PR.
  Not tested: the fail-closed render path (needs a graphics context), loading
  in the OBS app, and `plugin.loads` on Linux (the Ubuntu `libobs.so` needs
  system FFmpeg 6, PulseAudio and Wayland libraries that the pinned inputs do
  not provide, so Linux runs `plugin.exports` only).
- Eval delta: none (no pipeline change; `run.sh eval` still NOT RUN).
- Open issues or follow-ups: live load in OBS on the dev Mac (human);
  packaging the module and its data per OS (SPG-17); readback, ring buffer,
  blur and audio guidance (SPG-14).

## 2026-10-12 — SPG-05 Face detector (#29)

- Decision / change: `spg-detect` (`core/src/detect/`, lane:vision) with
  `FaceDetector`: YuNet 2023mar through `spg::runtime::Session`. The pinned
  model has a static 640x640 input, so the whole readback frame is letterboxed
  top-left into it (uniform scale, black padding right and bottom, as OpenCV's
  `FaceDetectorYN` pads) with `ModelInputMapping(..., Fit::LetterboxTopLeft)`;
  boxes go back through `spg::convert()` only, then are clipped to the source
  (far edge narrowed outward) and dropped when wholly in the padding.
  `detect(frame, source)` takes the readback pixels plus the source size, so
  `FrameView` and the contract headers are unchanged. Preprocessing
  (`frame_image.cpp`, OpenCV private): crop must be whole pixels inside the
  frame; area resize when downscaling, bilinear when upscaling; BGR raw 0-255
  NCHW (OpenCV `blobFromImage` defaults); NV12/I420 decoded as BT.709 limited
  range (OBS default), odd sizes supported. Decode and NMS (`yunet.cpp`) follow
  OpenCV 4.x `face_detect.cpp` (score sqrt(clamp(cls) * clamp(obj)), center
  offset and log size per stride 8/16/32, `NMSBoxes` semantics with top_k),
  with one SPG change: a non-finite score anywhere, or a non-finite box on a
  kept candidate, throws `DecodeError` so the frame fails closed instead of
  silently dropping a face. Defaults: score threshold 0.6 (OpenCV's demo uses
  0.9; lowered for recall over precision), NMS IoU 0.3, top_k 5000; SPG-12
  tunes them on the faces suite. Landmarks are decoded by YuNet but unused.
  Content size in the model input rounds to the nearest pixel, so a model
  coordinate maps back within half a model pixel on the non-limiting axis
  (under 1.5 Source px for 1080p), far below the tracker margin.
- Evidence (automated, dev Mac, macOS 26.5 arm64, AppleClang): `scripts/ci/run.sh
  build` and `test` pass, 78/78 ctest tests, 26 new `detect.*` cases. Unit cases
  use synthetic tensors (decode per stride, clamping, inclusive threshold, NaN/
  overflow fail closed, NMS threshold/top_k/ties), the mapping (four
  source/readback/aspect combinations, clipping, padding drop, 200 fractional
  boxes contain their exact Source image) and rendering (BGRA/NV12/I420 colors,
  stride padding never read, odd sizes, upscaling, invalid crops). Model cases
  (skip when YuNet is not cached) find no face in 24 empty synthetic frames and
  find a drawn cartoon face (ellipses, no real likeness) in six scenes, on CPU
  and the default provider (CoreML here), with the Source box centered within
  0.15 face sizes; three faces stay three; four threads agree. A YuNet probe on
  the drawn face scored about 0.86. Mutation check: ignoring the readback scale
  in `detect()` failed 3 cases (every downscaled scene, both providers).
- Eval delta: none. The detector is not wired into a pipeline yet (SPG-10/11),
  and the eval stage was not run here (the eval driver has not landed).
- Open issues or follow-ups: `Session::run` allocation per call is not
  measured; leave it to `spg-cli bench` (SPG-11/16). Small faces (recall gap,
  staff review P2): a 1080p frame is scaled by 1/3, so faces under about 30
  Source px may be missed, and a missed face airs. SPG-12 must treat tiling or
  a larger input as a recall item, not an option, and the 0.6 score threshold
  is untuned until then. No recall claim is made for small or distant faces
  before SPG-12 measures them on the faces suite. The text
  detector (SPG-06) can reuse `render_model_input()` for its tiles.
  `docs/contracts.md` describes `FrameView` as Source space; detectors also
  use it for the readback copy, so lane:policy may want to say so there.
- Review fixes (Greptile): `decode` also throws `DecodeError` when a kept
  candidate's raw box values are not finite or its decoded width or height is
  not positive (a -inf or underflowing log size used to decode to an empty box
  that clipping dropped silently). `stride_outputs()` checks every run's
  output shapes are exactly `[1, cells, 1|4]`, since the model may declare
  dynamic output dimensions and a same-count transposed tensor would otherwise
  be read as boxes. The test `OwnedFrame` is move-only so its view cannot
  point into another frame's buffers.

## 2026-10-12 — SPG-08 Policy engine (#30)

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
  (7) Review fixes, all recall misses: punctuation and symbols of every
  script (Unicode P, S, Z, generated `separator_table.inc`) separate words,
  so "vex" + Arabic comma matches a short entry; `+ 44` and `+ (44)` reach
  the E.164 rule; a hex-ending label (`ipv6:`) no longer hides an IPv6
  address; a JWT with a short header and payload is a token.
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

## 2026-10-12 — SPG-03 Synthetic eval suite and scorer (#32)

- Decision / change: eval tooling in Python under `eval/` (package
  `eval/spg_eval/`). `run_eval.py` is the driver `run.sh eval` already calls:
  it generates the suites for the mode, checks the scorer on them with three
  reference pipelines built from ground truth (no-op must fail recall,
  blur-everything must fail the clean-frame budget, an oracle that masks exactly
  the visible sensitive boxes must pass), then runs the pipeline.
  `run_pipeline()` raises until SPG-11 provides `spg-cli run`, so the CI log
  still ends `THRESHOLDS: NOT RUN` (exit 0 in ci mode, 1 in local mode, as
  before). `score.py` prints one line per limit and exactly one
  `THRESHOLDS: PASS/FAIL`; `generate.py` writes a suite. Python reader for
  ground truth and mask plans mirrors the C++ parser (schema 1, unknown and
  duplicate fields, integer types, float32 range check then narrowing) and
  parses `tests/fixtures/contracts/*.json`. Scoring choices the docs left open
  are in `docs/eval.md` "Scoring rules": unpaired or duplicate frames make the
  input unscorable (FAIL); boxes are clipped to the frame; a clean frame lists
  no sensitive instance at all, and generators list every planted item on
  every frame of its sequence (visibility 0 when it cannot show), so hold and
  retro masks around a fading item are not charged to the clean budget; a
  limit with no data fails; unknown threshold keys are errors.
  Rendering uses Pillow 12.3.0 pinned per interpreter (cp311 to cp314) and
  platform by SHA-256 in `eval/pydeps.json`, unpacked into
  `$SPG_CACHE_DIR/deps/py-pillow/` with `scripts/fetch_deps.py`'s verified
  install (no pip, nothing installed into Python); the font is Pillow's bundled
  Aileron Regular (CC0). Faces dataset: LFW (`lfw.tgz`, figshare mirror,
  SHA-256 055f7d9c..., the pin scikit-learn uses), recorded with its license
  under `datasets` in `models/manifest.json` as AGENTS.md asks
  (`fetch_models.py` ignores that key); face box = the central 114 px square of
  each 250 px image (LFW crops are detections enlarged 2.2x), checked by eye
  on composited frames. Synthetic PII follows the SPG-08 validator formats
  (NANP 3-3-4 with 2-9 area code and exchange, or unspaced E.164; Luhn-valid
  16-digit cards); decoys include dates and times. Half of planted secrets are
  watch-list entries. Profiles: text ci 10 x 40 frames, full 30 x 90; faces
  ci 6 x 30, full 24 x 60; 60 fps; 1080p and 720p alternate; every fourth
  sequence is clean.
- Evidence (automated, dev Mac, macOS 26.5 arm64): `run.sh setup`, `build`,
  `test` pass with 54 ctest tests, including `scoring.scorer` (20 tests:
  fixtures, strictness cases from `json_test.cpp`, sentinel never quoted,
  union area, metric definitions, verdict rules against the real
  `thresholds.toml`) and `scoring.generators` (15 tests: byte-identical ground
  truth for a seed, content coverage, ground-truth consistency, reference
  pipelines on a generated suite, face boxes land on the drawn face in a fake
  dataset, Luhn/phone formats, look-alike decoys beyond the fuzzy limits,
  hash-mismatch refusal). Both also pass under Python 3.11 and 3.14. Forcing
  every instance to count as covered makes 3 scorer tests fail.
  `run_eval.py --mode ci`: 400 frames in about 4 s, sanity checks ok, NOT RUN,
  exit 0. `--mode local`: text 2700 frames, faces 1440 frames (LFW fetched and
  verified), sanity checks ok, exit 1. No-op reference on the full text suite:
  watch-list pairs 0/3999, first-frame 0/53 items, PII pairs 0/4162, 630 clean
  frames, 4654 decoy pairs; faces 0/2207 face pairs, 360 clean frames.
- Eval delta: none; no pipeline yet.
- Open issues or follow-ups: SPG-11 implements `run_pipeline()` with
  `spg-cli run`; SPG-16 supplies realtime latencies (`SuiteRun.latencies_ms`)
  and a realtime suite, until which `--local` fails as required. One font only;
  more fonts and styles (pinned) would make the text suite harder. LFW crops
  can include part of a second, unlabeled face; harmless for recall, but a
  dataset with per-face boxes (WIDER FACE) would allow tighter crops.
- Review fixes (Greptile, staff review): `load_truth()` requires each
  sequence's ground truth to hold exactly frames 0..n-1 for the count in
  `suite.json`, since a ground truth and plan file shortened alike would pair
  cleanly; the contract reader range-checks numbers before `math.isfinite`, so
  a 400-digit integer is a contract error rather than an `OverflowError`;
  regenerating into an existing output deletes old frame PNGs and mask plans
  in each sequence, and `run_eval.py` clears its plans directory before the
  pipeline runs. Docs call the scripts through `python3`. A fallback frame
  counts as covered in the text and faces suites with no fail-closed limit
  there; `docs/eval.md` records that the realtime suite must run in local
  mode once SPG-11/16 land. `load_thresholds()` rejects unknown metric names
  in every suite table, so a typo in the faces or realtime limits fails in ci
  mode too. Follow-up for SPG-11: remove the `PipelineUnavailable` / `NOT RUN`
  path in the PR that wires `spg-cli run`.

## 2026-10-12 — SPG-09 Tracker and temporal hold (#33)

- Decision / change: `spg::Tracker` (`core/include/spg/tracker.hpp`,
  `core/src/track/tracker.cpp`, [tracker.md](tracker.md)). Greedy IoU
  association per kind against the predicted or last box, constant-velocity
  prediction from box centers (first measurement as is, then smoothed), margin
  and hold. Every detection is held on its own rather than only the track's
  latest box: the mask for frame time t is the union, over detections seen in
  [t - hold, t], of the hull of the box and the box moved at its velocity for
  min(t - seen, max_prediction), grown by the margin and rounded outward to
  whole pixels. This makes the AGENTS.md hold invariant literal (a detection's
  region stays covered for the full hold even after the track is re-confirmed
  elsewhere) and makes updates purely additive, at the cost of a trail behind
  a moving object for the hold. Recall over precision; the clean-frame budget
  is unaffected because trails only follow detections.
- Whole-pixel mask rects: float rects narrowed independently can miss a larger
  rect's edge by one ulp, which broke strict containment in the property test;
  integer edges within +-2^23 are exact in float and match what
  `enclosing_pixels()` blurs (at most 1 px extra per edge).
- Late or same-frame detections are held but do not move the track. Resize:
  `rescale()` merges each held box with its scaled box and widens the velocity
  range, so masks only grow; detections after a resize must use the new size.
- Clock: hold and prediction use frame time; the injected `Clock` only drives
  `prune()`, which keeps a held detection until `seen + hold + retain`
  (`retain` = lookahead) so buffered frames can still be queried.
- Defaults (no plan value, placeholders for SPG-12 tuning): `iou_threshold`
  0.3, `velocity_smoothing` 0.5, `max_prediction` 500 ms; hold 750 ms (plan)
  and margin 4 px + 0.25 * min(w, h) (SPG-02 placeholder) come from
  `PolicyConfig`. No hold or margin floor: zero stays valid, as #28 owns that.
- Evidence (automated, dev Mac, macOS arm64): `run.sh setup`, `build` (no
  warnings) and `test` pass, 66 tests, 12 of them tracker cases. The property
  case runs 2000 seeded scenes (moving objects of both kinds, missed cycles,
  late deliveries, unqueried frames, resizes in about 30%, zero holds) and checks
  the hold, monotone-in-time and additive-update properties. A mutation check
  (hold only the latest box, exclusive hold end, half margin, wrong velocity
  range on resize, pruning without `retain`, prediction without the hull) made
  at least one tracker test fail for each mutation.
- Eval delta: none measured. Nothing calls the tracker until SPG-10/11, and the
  eval stage is not landed (`eval/run_eval.py` is SPG-03).
- Open issues or follow-ups: SPG-10 applies retroactive masking and decides what
  happens to detections in flight across a resize; SPG-12 tunes margin, IoU,
  smoothing and prediction horizon, and whether margin should grow while a
  track coasts; IoU fails for objects moving more than about half their size per
  detection cycle (new track, still held, no prediction).
- Review fixes: a 2x upscale shrank a held mask, because `rescale()` recomputed
  the margin from the merged box and `margin_px` does not scale (a 40 px box's
  `[-14, 54]`, scaled `[-28, 108]`, became `[-24, 104]`). Each held detection
  now keeps its margin per axis, and a resize grows it to cover the scaled
  margin plus one scaled pixel of rounding, with velocities rounded outward;
  tests now require the scaled old masks after a resize. `masks()` merges each
  track's held detections into at most `max_rects_per_track` (default 8) fixed
  time buckets, so output and work stay bounded for long holds without removing
  coverage, and gains a buffer overload. The monotone-in-time property check
  moved into the scene, where nothing it compares has been pruned. Velocity is
  still unbounded for very short confirmation intervals; that only over-blurs
  and is left to SPG-12 (#13). The injected clock must share the frames'
  `FrameClock` time base (documented on `Tracker`; SPG-10 must keep it). A
  mutation check (fresh margin on resize, no rounding slack, unstable bucket
  grouping, buckets without hulls, prediction shrinking over time) made at
  least one tracker test fail for each mutation.

## 2026-10-12 — SPG-06 Text detector (#35)

- Decision / change: `TextDetector` in `spg-detect` (`core/src/detect/`,
  lane:vision) runs the pinned PP-OCRv3 English DBNet
  (`en_PP-OCRv3_det_mobile.onnx`) through `spg::runtime::Session` on fixed
  640x640 tiles of the readback frame. `tile_crops()` lays out a grid of
  whole-pixel crops at most 640 on a side, first at the origin and last flush
  with the far edge, neighbours overlapping by at least 64 readback px (fewest
  tiles that keep it: 2 for 960x540, 8 for 1920x1080). Each crop is
  letterboxed top-left at scale 1 (a frame smaller than a tile is scaled up
  uniformly) with `render_model_input()`, so faces and text share one
  preprocessing path; the tensor is BGR planar `(v / 255 - 0.5) / 0.5`
  (RapidOCR DetPreProcess). Boxes go back through `spg::convert()` with that
  tile's mapping only, are clipped to the source (`clip_to()`, moved out of
  `face_detector.cpp` into `source_clip.cpp` so both detectors share it), and
  boxes from different tiles that intersect and overlap vertically by at least
  half the shorter one are united (one line cut by a tile edge becomes one
  box). Output: Source-space `TextRegion` detections, detector `PpOcrDet`,
  sorted top to bottom. DB post-processing (`db_postprocess.cpp`) follows
  RapidOCR v3.9.2 `DBPostProcess` (binarize > 0.3, 2x2 dilation, RETR_LIST
  contours, min-area rect, short side >= 3, "fast" mean score >= 0.5, unclip
  by area * ratio / perimeter, grown short side >= 5), with SPG changes, all
  toward covering more: axis-aligned output (bounding box of the grown rect
  plus one pixel on the far edges); no 1000-candidate cap (RapidOCR silently
  drops the rest); a non-finite probability anywhere or an output that is not
  exactly `[1, 1, 640, 640]` throws `db::DecodeError` so the frame fails
  closed; and `unclip_ratio` 2.0 instead of 1.6. Evidence for 2.0: on the
  synthetic lines of the coverage test (100 line checks over five scenes, two
  styles, CPU and default providers), 1.6 left a box edge inside the ink on 68
  lines, by up to 11% of the ink height; 2.0 on 20 lines, by at most 2%. The
  coverage tests therefore accept an edge up to 5% of the ink height inside
  the ink; the mask margin (`margin_px` 4 + `margin_ratio` 0.25 of the box's
  short side) is far larger. SPG-12 tunes thresholds, unclip, tile size and
  overlap on the text suite.
- CoreML (issue note 1): the model's input dims are named
  (`DynamicDimension.0/1/2`), so ORT free-dimension overrides would make it
  static. A probe with Python onnxruntime 1.23.1 (not the pinned 1.30.0 C++
  build) on the dev Mac: with overrides CoreML took 144 of 172 nodes in 23
  partitions and a 640x640 tile took about the same time as without them
  (roughly 40 ms either way, CPU alone about 38 ms). No gain, so no override
  plumbing was added to `SessionOptions`; the fixed tile size keeps the option
  open. This is a probe, not a `bench` result; `spg-cli bench` measures it.
- Evidence (automated, dev Mac, macOS 26.5 arm64, AppleClang): `scripts/ci/run.sh
  build` and `test` pass, 111/111 ctest tests, 26 new `detect.*` text cases.
  Unit cases (no model): DB decode on synthetic probability maps (exact grown
  box for ratio 1.6 and 2.0, separate lines, binarize/box thresholds including
  the inclusive edge, specks, a 30-degree band, 1280 candidates all kept, NaN
  and +-inf anywhere and wrong sizes fail closed, param validation), the tile
  layout (1000 seeded random frames: coverage, flush edges, overlap, fewest
  tiles), tile-to-Source mapping with padding drop and clipping, upscaled
  small frames, merging (same line across tiles, same-tile and different
  lines kept apart, transitive chains, order), tensor layout, config. Model
  cases (skip when the model is not cached): no box on 12 text-free frames
  (black, gray, gradient, UI-like panels; BGRA/NV12/I420); every drawn line
  covered by one box with no box off the text in five scenes (1080p, 720p,
  641x479, portrait, 1440p; readback scales 1 and 1/2) x two styles x CPU and
  default providers (CoreML here); lines with readback cap height 6 to 12 px;
  a line crossing two tile edges merged into one box; four threads agree.
  Fixtures are made-up strings drawn at test time with OpenCV Hershey fonts
  (`tests/integration/synthetic_text.hpp`); no image is committed. A probe on
  960x540 found every line from cap height 5 readback px up and only some at
  4. Mutation checks: mapping boxes without the tile offset failed 2 model
  cases (89 assertions); skipping the cross-tile merge failed the tile-edge
  case.
- Eval delta: none. The detector is not wired into a pipeline yet (SPG-10/11),
  and the eval stage was not run here.
- Open issues or follow-ups: Hershey strokes are not real UI fonts; SPG-12
  must measure recall on the text suite before any recall claim. Small text:
  at a 1/2 readback scale, Source text under about 10 px cap height falls
  below the probed floor; SPG-12/SPG-14 decide readback scale or an upscaled
  tile pass. Per-frame cost is one model run per tile (2 to 8 for common
  sizes) plus a `Session::run` allocation each; `spg-cli bench` measures it.
  Recognition (SPG-07) consumes these boxes; rotated text gets an
  axis-aligned box, so SPG-07 may want the rotated rect later.
- Review fixes (Greptile, staff review), after merging SPG-03 from `main`:
  cross-tile merging compared grown boxes, so two adjacent lines seen by two
  tiles (60x12 kernels 4 px apart grow to 33 px boxes overlapping by 17) were
  joined into one box, which would hand SPG-07 two lines as one.
  `db::Candidate` now carries `line`, the fitted rectangle before growth, and
  `merge_across_tiles()` requires the grown boxes to intersect and the
  kernels to overlap vertically by `merge_line_overlap`; coverage is
  unchanged (boxes are still the grown ones). Merging sweeps boxes sorted by
  top edge and only compares those that overlap vertically, so the uncapped
  candidate list no longer costs every pair; a seeded test checks it against
  the all-pairs result on 20 frames of 600 boxes. `tile_overlap` is capped
  at `tile_size / 2` (639 of 640 meant 564,921 model runs for one 1080p
  frame). New fixture models built at test time (`text_wrong_shape.onnx`,
  as many values as the right shape but `[1, 4, 320, 320]`;
  `text_sqrt.onnx`, NaN on dark pixels) drive `detect()` itself: the wrong
  shape fails the frame, and a NaN in the second tile fails it even though
  the first decoded. The thread test now shares the default-provider session
  too. Mutation checks: comparing grown boxes again failed 3 merge cases;
  cutting the sweep short failed the all-pairs test; dropping the runtime
  shape check failed the wrong-shape case. `TextDetectorConfig` documents
  the large-text recall limit (staff review): text taller than the overlap
  can be cut by a horizontal tile edge, and SPG-12 must add a large-glyph
  case. Evidence (dev Mac): `run.sh build` and `test` pass, 137/137, no
  skips. Not changed: per-frame cost (one model run per tile, 8 at 1080p,
  28 at 2160p) stays for SPG-10/12 to measure with `spg-cli bench`;
  `detect()` now documents that it has no deadline or cancel check, so the
  scheduler must fail a frame closed when the call misses the lookahead
  deadline. The Check logs for 1ee7ab8 show `ppocr-det-en` cached on all
  three runners before the test stage and no skipped `detect.*` case, so
  the model-backed cases run in CI.

## 2026-10-12 — SPG-15 Settings and watch-list storage (#37)

- Decision / change: plan question 4 resolved with the recommended default.
  Watch-list entries never enter OBS filter settings (saved in plain text in
  the scene collection): settings keep `watchlist_ref` (32 random hex digits)
  and `watchlist_count`, and the entries live in
  `<OBS config>/plugin_config/obs-spg/watchlists/<ref>.txt`, `0600` in a `0700`
  directory on macOS/Linux (tightened again on load), the per-user `%APPDATA%`
  ACL on Windows; written to a user-only temporary file, synced (POSIX) and
  renamed. OS keychain stays a later option. Properties per plan section 7
  (`docs/settings.md`): masked entry (OBS password field) with Add, a numbered
  entry list with Remove selected and Remove all; category checkboxes;
  sensitivity (`standard` = spg-core fuzzy defaults, `high` = 3 edits, 0.3,
  partials from 5; no preset below the defaults; placeholders for SPG-12);
  lookahead, hold, fallback (no fail-open choice), debug overlay (off). Other
  choices: (1) clamps for values from scene files are written back to the
  settings: lookahead [100, 2000] ms, hold [250, 10000] ms (floors raise unsafe
  values; ceilings are safe and bound VRAM); unknown sensitivity or fallback
  names become the defaults. SPG-08b (#28) is choosing spg-core floors in
  parallel; if its floors are higher, the plugin constants must rise to match,
  and until then spg-core's rejection fails closed. (2) Fail closed on storage:
  a missing file while `watchlist_count` > 0, a malformed reference (never
  turned into a path), an unreadable or corrupt file, no absolute config
  directory, all categories off, or a config spg-core rejects all drop the
  PolicyConfig and show an error; Add/Remove are refused until Remove all
  writes a new empty list. (3) The masked field is moved into the file by Add
  and erased in the `save` callback, which libobs calls just before
  `obs_save_source` serializes settings, so a typed-but-unadded entry is not
  saved either. (4) A filter duplicated in OBS arrives with its original's
  reference; a process-wide registry forks the copy to its own file, so
  removing an entry from the copy cannot remove it from the original after a
  restart. (5) Video filters' `update` runs deferred on the video thread
  (libobs `obs_source_update`), so no file I/O runs while the filter mutex is
  held; the file is read only on create or a reference change. Limits: 256
  entries of 256 bytes. Settings logic is an OBS-free static library
  (`obs-spg-settings`) so it is tested on Linux too.
- Evidence (automated, dev Mac, macOS 26.5 arm64, AppleClang):
  `scripts/ci/run.sh build` and `test` pass, 119/119 ctest tests (14 new
  `plugin.settings.*` cases plus an extended `plugin.loads`). Unit cases cover
  clamping (0, negative, LLONG_MIN/MAX), unknown names, the PolicyConfig
  mapping, references, entry checks, file format round trip and every rejection,
  user-only modes after each save and after a loosened file, and a sentinel run
  with std streams captured. `plugin.loads` (headless libobs, macOS and Windows
  only) creates filters, clamps scene-file values, presses Add/Remove/Remove
  all through `obs_property_button_clicked`, duplicates and reloads a filter,
  and checks the captured libobs log, `obs_save_source` JSON and every file
  outside the watch-list directory for sentinel fragments. Mutation checks:
  dropping the `save`-callback erase failed 2 `plugin.loads` checks; logging
  the entry on Add failed the log check; creating the file `0644` failed the
  unit and `plugin.loads` mode checks (the unit test first passed because it
  only checked after a load had tightened the mode; fixed).
- Eval delta: none; no pipeline stage reads these settings yet (SPG-10/14).
- Open issues or follow-ups: live check in OBS on the dev Mac (human): the
  masked field, buttons and refresh in the real properties view, and that a
  saved scene collection holds no entry text. Watch-list files of deleted
  filters stay on disk (OBS gives no reliable delete-vs-undo signal). Entries
  sit in process memory and, if typed, in OBS's in-memory undo stack; crash
  dump exclusion is still open. Windows relies on the profile ACL rather than
  setting one. SPG-14 must read `FilterState::policy`/`issue` and keep the
  whole-source fallback whenever no PolicyConfig is set.
- Review fixes (Greptile and staff review on `706e941`): (1) a copy of a
  filter whose watch-list failed to load now records its file as expected
  (`watchlist_count` at least 1), so after a reload it reports Missing instead
  of reading the absent file as a healthy empty list. (2) Remove all entries
  falls back to a new reference when the old file cannot be replaced (a
  directory in its place) and releases the old reference; `update` releases a
  claimed reference it failed to install. (3) `update` skips the policy
  rebuild when neither the settings nor the watch-list changed. Declined:
  moving the reference-change reload off the video thread (bounded single
  file read, only on an external reference change; documented in
  `docs/settings.md`). Mutation checks: reverting (1) failed 2 `plugin.loads`
  checks, reverting the fallback in (2) failed 4, dropping the release failed 1.

## 2026-10-12 — SPG-07 Text recognizer (#41)

- Decision / change: `spg-ocr` (`core/src/ocr/`, lane:vision) reads the
  detector's Source-space text regions with the pinned PP-OCRv4 English
  recognizer (`en_PP-OCRv4_rec_mobile.onnx`) through `spg::runtime::Session`.
  Each region's box goes to Readback through `spg::convert()` and
  `enclosing_pixels()` only; preprocessing, CTC decoding and the alphabet
  follow RapidOCR v3.9.2 (height 48, aspect kept, `(v / 255 - 0.5) / 0.5`
  BGR planes, right padding with zeros to at least 320; greedy CTC with
  blank 0, the metadata `character` lines and an appended space: 97 classes,
  which the pinned model's output has; one step per 8 input columns,
  measured). Output is `Recognition{Secret text, score}`; nothing is dropped
  for a low score (RapidOCR cuts at 0.5), the policy decides.
- Crop and rectify (issue #8 notes): boxes stay axis-aligned. A region is read
  as several views and all texts are kept, joined by newlines: the whole
  crop; each line band when `split_lines()` finds rows without horizontal
  edges between text rows (cut in the middle of the gap, runs shorter than
  half the tallest joined to a neighbour so i-dots and underlines do not
  split); and both quarter turns when the crop is at least 1.5 times as tall
  as wide (RapidOCR turns one way; there is no direction classifier). This
  handles the cross-tile merge joining adjacent lines: the test "Two lines
  merged into one box are read line by line" fails 24 assertions without the
  line views. Lines that touch (no empty row) stay one band. Text turned 180
  degrees is not read.
- Long lines: a view wider than `max_width` 2560 input px is read in chunks
  overlapping by half a chunk, so any stretch up to 1280 input px (about 50
  characters) is whole in one chunk; a card number is never read only in
  pieces, and a longer watch-list entry is read in pieces over half a chunk
  each, which the policy's partial rule matches.
- Batching: crops are sorted by width and run in batches of `max_batch`,
  padded to the batch's widest. Default `max_batch` 1, measured: on the dev
  Mac (M4, ORT 1.30.0 CPU, scratch probe, median of 15 to 20 runs) one 48x320
  crop took 4.5 ms, while batches of 2 to 8 took 7.5 to 12 ms per crop (with
  one intra-op thread: 9.4 ms alone, 19 ms per crop batched). A fixture
  model that reads columns independently proves batch sizes 2, 4 and 64 give
  exactly the same text as 1.
- CoreML (dispatch note): fixed-width buckets were considered and not
  adopted. The pinned model's input is `[DynamicDimension.0, 3, ?,
  DynamicDimension.1]`; the height has no name, so free-dimension overrides
  cannot make the input static and CoreML with `RequireStaticInputShapes=1`
  takes no node. Probe with batch and width overridden: CoreML 4.55 ms
  (1x320) and 16.0 ms (1x1280) against CPU 4.56 ms and 15.2 ms. Widths are
  rounded up to a multiple of 8 instead, with no bucket padding. A static
  model would mean re-exporting it, a new pinned artifact.
- Per-track cache: `RecognitionCache` stores recognized text, never a
  verdict (the policy evaluates the text again every cycle). It reuses a
  track's text only when the entry was stored after a successful read, the
  frame time is within [read, read + 1 s], and the region's readback crop has
  the same size and identical decoded BGR pixels. The recognizer reads only
  those pixels, so a hit returns what a new read would. Any other change is a
  miss: one pixel, a colour change, a resized box or source, or a track id
  reused for other content. Same pixels moved with their box (scrolling) hit.
  Bounds (second review, resource exhaustion): one entry per track, at most
  `capacity` 1024 tracks, `max_entry_bytes` 4 MiB per entry and `max_bytes`
  64 MiB in all. A store drops the track's old entry, then every expired
  entry, then least recently used entries (a hit counts as a use) until the
  new one fits; a crop that is empty or over a byte bound is not stored. A
  full cache only causes misses, which are read again. `recognize_tracks()`
  sends all misses to one `recognize()` call and stores results only after it
  succeeds, so a failed frame leaves the cache unchanged. One owner, not
  synchronized. Tests: 20000 seeded operations over 200 track ids keep
  size, bytes and per-entry size within bounds and every hit fresh and
  current; six tracks through a two-entry cache all come back equal to an
  uncached read. Mutations: removing the capacity bound failed both; no
  expiry on store or no LRU touch on a hit each failed its own section.
- Security review fix (before review): the first version compared a luma
  thumbnail (six square cells across the short side, each within 6 levels)
  and allowed a 10% size change. A background review flagged it as a
  possible fail-open stale cache, and it was one: a colour change with the
  same brightness, faint text, or a one-pixel change in a large crop all
  matched and reused stale text for up to 1 s. Reinstating the thumbnail rule
  as a mutation fails four new unit sections (colour change in BGRA and NV12,
  faint text, one pixel); the exact comparison passes them.
- Fail closed: an output whose shape is not `[n, width / 8, 97]`, or that
  holds a value that is not a finite probability, throws `ocr::DecodeError`;
  a model without the dictionary or with a class count that does not match
  it is refused when opened. Messages carry sizes and indices only. The
  sentinel test reads a made-up sentinel string with the pinned model through
  the cache and the policy (verdict WATCHLIST) while capturing the standard
  streams, then fails two broken fixture recognizers on the same frame:
  nothing is printed and no error holds any fragment of it.
- Evidence (automated, dev Mac, macOS 26.5 arm64, AppleClang):
  `scripts/ci/run.sh build` (plugin included) and `test` pass, 197/197 after
  merging main at 87ed3f5, 32 new `ocr.*` cases, no skips. Model cases: six made-up
  strings on three panel styles, cap heights 10, 14 and 22 readback px,
  readback scale 1 and 1/2, BGRA/NV12/I420, each read within two edits
  (Hershey's 'a' reads as 'o' at cap height 8 to 10); merged lines; chunked
  long lines with the policy flagging the watch-list entry and the card;
  vertical text both ways; batched runs; detector boxes on a half-scale
  1280x720 NV12 frame feeding recognition and policy (watch-list and card
  lines flagged, the decoy not); four threads agree; the cache sequence. Six
  regions of a 900x420 panel take about 35 ms on CPU in the tests (not a
  `bench` result). Fixture models generated at build time
  (`make_runtime_fixtures.py`: `rec_softmax`, `rec_sqrt`, `rec_wrong_classes`,
  `rec_no_dictionary`) cover batching equality, NaN output, wrong step and
  class counts, and a missing dictionary. Mutation checks: no line views
  failed the merged-lines case; no rotated views failed the vertical case
  both ways; a cache that ignores content failed the unit and model cache
  cases; chunks without overlap failed the 2000-region planning property;
  clockwise chunks left top to bottom failed the reading-order planning case,
  the planning property (now half tall regions) and a chunked vertical model
  case (review follow-up).
- Eval delta: not measured. The recognizer is not wired into a pipeline yet
  (SPG-10/11), and the eval stage was not run in this session.
- Open issues or follow-ups: Hershey strokes are not UI fonts, so SPG-12 must
  measure recall on the text suite; 180-degree text needs PP-OCR's direction
  classifier (another pinned model); lines that touch stay one crop;
  `spg-cli bench` should measure recognition cost and the cache hit rate
  (any pixel change misses, so animated or noisy sources get no hits, which
  costs time, not recall).

## 2026-10-12 — SPG-10 Scheduler, lookahead and fail-closed (#40)

- Decision / change: `spg::Lookahead` (single-threaded ring buffer, bounded
  scan queue, cadence, coverage, retroactive masks, release and fallback),
  `spg::Scheduler` (that state machine behind a mutex plus worker threads) and
  `spg::run_scan()` (one cycle: faces, text regions, recognizer, policy), in
  `core/include/spg/{lookahead,scheduler,scan}.hpp`, `core/src/schedule/` and
  `core/src/mask/mask_rects.*`; rules in [scheduler.md](scheduler.md).
  Choices: (1) Release rule: a frame at `t` airs clean only when a complete
  cycle on a frame of the same segment (no resize or discontinuity between)
  with time in `[t, t + retro]` finished before release, the release is by
  `t + lookahead + release_grace`, and its timestamp increased and was within
  `clock_tolerance` (1 s) of the injected clock; otherwise the configured
  fallback. This is the issue's "scanned only by a cycle within retro_ms after
  it" note; a cycle before the frame does not count. (2) Deadlines are
  enforced at release, not by interrupting stages (detectors and the policy
  cannot be cancelled): a stalled, late or overrunning cycle simply has not
  finished when its frames come due. Its result is still applied later.
  (3) Any exception from any stage, of any type, and any invalid output
  (`validate()` failure, wrong kind, wrong text count) makes the cycle
  incomplete. (4) Retroactive rects are the found boxes, grown by the
  tracker's margin, on buffered frames in `[ts - retro, ts)`; static, no
  velocity back-projection (SPG-12 measures scroll sequences). (5) Resize: a
  result from before a resize is given to the tracker as the hull of its box
  scaled to every size since, and to older-size frames as the hull with its
  scaling to that size, so masks only grow; it does not count as a scan of
  new-size frames. (6) Recognized text over 512 bytes is not given to the
  policy; its region is masked. Without a recognizer or policy every text
  region is masked. (7) `hold_last_scanned` re-emits the last clean frame of
  the same segment (`ReleasedFrame::reemit`), else blurs the source.
  (8) Degenerate configs (#28 not merged): no floors imposed; zero lookahead
  or retro fail closed by the release rule, zero hold still masks the
  detecting frame and retro frames. (9) Tracker `retain` is raised to
  `lookahead + release_grace` so held detections outlive every frame that can
  still be released on time. (10) Defaults without a plan value: 2 workers,
  queue capacity 1 (newest frame, oldest dropped), cadence 0 (offer every
  frame), 600 buffered frames, 64 retro rects per frame (then one hull),
  500 ms release grace, 2 s shutdown timeout after which a worker still in a
  stage call is detached (it owns the stages via `shared_ptr` and never
  touches the scheduler or clock again).
- Evidence (automated, dev Mac, macOS 26.5.2 arm64, AppleClang):
  `scripts/ci/run.sh setup`, `build` (plugin included, warnings as errors) and
  `test` pass, 184/184 ctest tests, 33 new `[schedule]` cases.
  `lookahead_test.cpp`: 26 deterministic cases on a `ManualClock` with scripted
  stages (stall, late return, slow policy advancing the clock, every stage
  throwing `runtime_error`, `ContractError`, `bad_alloc` and a non-`std`
  type, invalid output, text cap, categories, retro window bounds, coverage
  window, resize with a cycle in flight, size round trip, bad timestamps,
  zero lookahead/retro/hold, buffer overflow, late release, bounded queue,
  cadence, hold-last-scanned, discontinuity, sentinel text never printed).
  `lookahead_property_test.cpp`: 1500 seeded scenes (random lookahead 0 to
  500 ms, retro, hold 0/100/750 ms, fallback modes, queue 1 to 3, cadence,
  buffer limits, cycles that start late, take up to twice the lookahead, fail
  20%, stall forever 10%, return out of order, render hiccups up to 1.5 s,
  resizes, content switches, bad timestamps); an oracle kept by the test
  checks every released frame: clean only with a qualifying complete cycle,
  fallback action matches the mode, every box of a complete cycle within the
  retro window after or the hold before the frame is covered (scaled across a
  resize), release order and count. Totals: 33,902 clean and 191,098
  fallback releases, 52,327 re-emits, 6,668 retro and 141,151 hold
  containment checks. `scheduler_test.cpp`: real threads with an atomic
  clock; render calls under a stalled detector and a stalled policy return
  within a real-time timeout, shutdown with a stalled worker returns in under
  3 s, a throwing worker keeps serving. Mutation checks: 16 mutations (no
  coverage check, no retro rects, coverage window before t, ignoring
  segments, no late check, no hull for late results, no retro scaling, no
  text cap, failed cycles counting, bad timestamps accepted, releasing before
  the deadline, no retain bump, hold-last across segments, invalid output
  accepted, ignoring categories, unknown jobs accepted) each failed at least
  one test.
- Eval delta: not measured. Nothing feeds the scheduler a pipeline until
  SPG-11 wires `spg-cli run`, and the eval stage was not run locally (the
  agent sandbox refuses that command); the PR's Check log has the CI eval
  stage output.
- Open issues or follow-ups: SPG-11 drives `Lookahead` (offline) or
  `Scheduler` and composes `ReleasedFrame`s; SPG-14 keeps the textures for
  `reemit`, calls `release_due()` every render, sizes its texture ring to
  `max_buffered_frames`, and must not unload the module while a detached
  worker is still in a stage call. SPG-16 measures workers, cadence and the
  fail-closed rate with `spg-cli bench`, and the cost of `complete()` (held
  under the lock) for many detections. SPG-12 measures retro back-projection
  for moving text and whether a short secret between two scans is missed
  (the coverage rule assumes content seen at `t` persists to the next scan).
- Review fixes (PR #40, after the first Check and staff review): (1) a null
  detector for an enabled category now fails the cycle
  (`ScanFailure::MissingStage`) instead of completing with no detections, and
  `DetectorAdapter` and `EnginePolicy` reject null pointers; (2) a readback is
  scanned for the buffered frame its timestamp names (a mapped stage surface
  lags one frame), never credited to the frame pushed with it; (3) a result
  that lands after a resize keeps its scaled margin, in the tracker and in
  retroactive rects; (4) a complete result that `complete()` cannot fully
  apply fails closed every frame it could have masked
  (`FallbackReason::ApplyFailed`: buffered frames from `ts - retro`, frames
  pushed up to `ts + hold`); (5) `take_job()` cannot throw (in-flight slots
  reserved, a job it cannot record is dropped); (6) the duplicate
  `SchedulerConfig::categories` is gone, `scan.categories` gates the stages;
  (7) the threaded timeout helper releases the stalled stage before failing,
  so a regression fails instead of hanging. The ubuntu-24.04 Check failure was
  two GCC-only warnings under `-Werror` in the new tests
  (`-Wsign-conversion`, `-Wrange-loop-construct`); AppleClang and MSVC do not
  emit them. Evidence: unit tests pass under AppleClang, GCC 13 and GCC 14 on
  the dev Mac; each fix was mutated back and failed its new test.

## 2026-10-12 — SPG-15b Watch-list text out of crash dumps and the undo stack (#38)

- Decision / change: a small protected heap in spg-core
  (`core/include/spg/secure_memory.hpp`, `core/src/secure/`) instead of a
  per-allocation `mmap`: page-aligned anonymous regions, 64 KiB shared by
  nine size classes (16 to 4096 bytes) plus one region per larger block.
  Each region is excluded from crash dumps before use
  (`madvise(MADV_DONTDUMP)` on Linux, `WerRegisterExcludedMemoryBlock` on
  Windows, nothing exists on macOS), locked with `mlock`/`VirtualLock` (best
  effort; a refusal is counted, not fatal), and every block is zeroed on
  release (`SecureZeroMemory`, or `memset` behind a compiler barrier). The OS
  calls sit behind an injectable `SecureMemoryPlatform` so tests record them.
  `SecureHeap::process()` is a never-destroyed function-local static: a
  standard allocator needs one shared heap, and leaking it keeps a `Secret`
  in static storage safe at exit (recorded exception to "no globals").
  `Secret` now stores its bytes there (never inline), `Secret(std::string)`
  zeroes the string it consumed, and `Secret::copy_of(string_view)` copies
  without a temporary. The policy engine's compiled entries, its folded,
  matching and reversed temporaries and the PII digit views use
  `SecureAllocator`; matching behavior is unchanged. The plugin's store
  reads and writes the file with unbuffered `read`/`write` (`_read`/`_write`
  and `_commit` on Windows) straight from protected memory. Undo stack:
  in OBS 32.1.2 the properties view records a filter undo step from the live
  settings 500 ms after the last edit, but runs a property's modified
  callback synchronously on each edit, so the masked field now has one that
  moves the text into `FilterState::pending_entry` (a `Secret`), overwrites
  the libobs item in place and erases the key; Add uses it. Settings, and so
  every undo snapshot and save, hold only `watchlist_ref`, `watchlist_count`
  and the plain settings. A side effect: a scene collection save while the
  user types no longer clears what was typed. Limits (Qt and libobs copies
  of each keystroke, short-string buffers on the stack, dumps WER does not
  write, macOS) are in `docs/settings.md`, "Watch-list text in memory".
- Evidence (automated, dev Mac, macOS 26.5 arm64, AppleClang):
  `scripts/ci/run.sh build` and `test` pass, 174/174 ctest tests. New unit
  cases: a recording platform shows map, exclude and lock before a block is
  handed out, and unlock, include and unmap when a large block is freed; the
  wipe observer sees every released block all zero (small, large, `Secret`
  destroyed and reassigned); refused exclusion or locking keeps the heap
  working and is counted; a failed map throws `std::bad_alloc`; the policy
  engine's entries are live blocks of the process heap; on Linux
  `/proc/self/smaps` must show `dd` on the mapping holding a `Secret` (CI
  only), on Windows WER registration must succeed, on macOS the method is
  `none`. `plugin.loads` replays the OBS 32.1.2 `ControlChanged` sequence
  key by key and checks the undo snapshots `FilterChangeUndoRedo` would
  store. Mutation checks: not registering the callback failed 8
  `plugin.loads` checks; not resetting `pending_entry` on a property build
  failed 2; a no-op wipe failed 4 unit cases; skipping exclusion failed 2.
- Eval delta: none expected (allocator change only). The eval stage was not
  run locally (the agent sandbox refuses that command); CI runs it.
- Open issues or follow-ups: live check in OBS on the dev Mac (human, steps
  added to `docs/live-check.md`): typing, undo and redo in the real Qt view.
  Not verified anywhere: that WER leaves the regions out of a real crash
  dump on Windows, or that a Linux core file omits them (the `dd` flag is
  checked, not a core file).
- Review fixes (Greptile and staff review on `40e98ad`): (1) the store reads
  the watch-list through one descriptor to end of file, capped one byte past
  the size limit, instead of trusting an earlier `file_size()`; a file that
  grew between the two could be cut at a line end and load as a shorter
  list. (2) Add rejects a field a script set to the empty string instead of
  falling back to text typed earlier. (3) Size classes now reach 16 KiB
  (eleven classes), so `evaluate()`'s temporaries for a line of up to 4096
  code points reuse shared blocks with no system call, and a large block's
  map, exclude, lock and their undo calls run outside the heap's mutex.
  (4) `docs/policy.md` records that `std::bad_alloc` from the engine must
  make the caller apply the fallback. (5) `append_utf8` is shared with
  `to_utf8` (templated over the string type). Mutation checks: restoring
  the old Add fallback failed 2 `plugin.loads` checks; mapping a large
  region under the lock failed the new lock-scope unit case (after its 5 s
  wait, no hang). The read race itself is not reproducible without a hook
  between stat and open; the new case reads a ~127 KB list across every
  buffer growth.
- Review fixes (staff review on `a2ff5a4`): a new shared region is now also
  mapped, excluded and locked outside the heap's mutex (a concurrent grow of
  the same class just adds both regions to the free list), and the
  large-block "not ours" path no longer decrements the live-block count.
  Merged `origin/main` (SPG-07, #41; its log entry first, in merge order).
  Measured `evaluate()` before (main, plain `std::u32string`) and after on
  the dev Mac with 10 synthetic entries: single-threaded unchanged (~26 µs
  per 170-character line, ~440-450 µs per 3000 characters); four threads at
  once, 170 characters: ~28 to ~33 µs from the shared lock; 3000
  characters: unchanged. Recorded in `docs/policy.md` for the scheduler
  ticket. Mutation check: mapping a shared region under the lock failed the
  lock-scope unit case.
- Review fix (staff review on `f18d74a`): `WerRegisterExcludedMemoryBlock`
  and its undo (Windows 10 1703 and later) are resolved from `kernel32.dll`
  with `GetProcAddress` instead of imported through `wer.lib`, so an older
  Windows loads the plugin and reports dump exclusion `none` instead of
  failing to load the DLL. The Windows branch was syntax-checked locally
  against a stub `windows.h` only; the Windows CI job is the build evidence.

## 2026-10-12 — SPG-15d Recognized text in wiped, dump-excluded memory (#46)

- Decision / change: the recognizer builds text in `ocr::SecureText`
  (`std::vector<char, SecureAllocator<char>>`) instead of `std::string`:
  CTC decoding appends straight into the caller's buffer (a decode error
  zeroes what it appended and cuts it off again), each read job has one, and a
  region's views are joined in another before `Secret::copy_of()`. A vector
  rather than `SecureString`, because a string keeps short text in its
  short-string buffer, inside the object, which for a temporary is on the
  stack; most OCR lines are short. The model's output probabilities (whose
  per-step argmax spells the text) are zeroed once a batch is decoded or
  fails. The cache already stored text only as `Secret` (unchanged; a
  `static_assert` keeps it so).
- Crop fingerprints: `Fingerprint` now zeroes its pixels, unused capacity
  included, when destroyed or assigned over, and a moved-from one is empty;
  the cache also zeroes each fingerprint it holds when the entry is replaced,
  erased, expires, is evicted or refused, and when the cache is destroyed,
  with a test-only observer like `SecureHeap`'s. The pixels stay in ordinary
  memory on purpose: up to 64 MiB of crops, most over the 16 KiB size
  classes, would each be a SecureHeap region of their own, using up
  `RLIMIT_MEMLOCK` and, on Windows, the 512 WER exclusions that keep
  watch-list regions out of dumps. `RecognitionCache` is no longer copyable
  or movable (one worker owns it; nothing copied one).
- Scan path: `run_scan()` gets `std::vector<Secret>`, checks sizes and passes
  each `Secret` to the policy by reference; `recognize_tracks()` moves them
  into `TextHit`s; `PolicyEngine::evaluate()` folds into `SecureU32String`s.
  No plain-string copy there, so `scan.cpp` is unchanged. Copies that remain
  (ONNX Runtime's output arena, pixel buffers, the policy's short-string
  buffers for lines of 3 or 4 code points) are in `docs/policy.md`,
  "Recognized text in memory".
- Behaviour: identical. A scratch probe (not committed) hashed every text and
  printed every score for 7 regions x 4 cap heights (10 to 22 px) x BGRA and
  NV12 with the pinned model, plus the `rec_softmax` fixture: the 58 lines
  match `main` (7ee5fd8) exactly.
- Timing (dev Mac, M4, macOS 26.5, ORT 1.30.0 CPU, Release build, scratch
  probe, median per call, 3 rounds x 2 runs each): one region read as one
  48x320 crop, `main` 4.54 to 5.10 ms, branch 4.59 to 4.67 ms (SPG-07: about
  4.5 ms); six regions of a 900x420 panel, `main` 32.5 to 34.3 ms, branch
  32.3 to 33.9 ms; `ctc_greedy_decode` of 40 steps x 97 classes alone,
  `main` 8.20 to 8.35 us, branch 8.19 to 8.32 us. No regression beyond
  noise. Six cache hits (100 KB of crops) take 11.3 to 13.4 us on the
  branch; `main` was not measured for that path (see open issues).
- Evidence (automated, dev Mac): new cases `CTC decode builds the text in
  protected memory and zeroes it`, `RecognitionCache zeroes every fingerprint
  it lets go of` (all seven ways out), `Fingerprint: moved-from and wiped
  fingerprints are empty`, and `Recognizer temporaries are protected blocks,
  zeroed when released` (fixture model: one live block per region after the
  call, at least two zeroed per region, all zero). The sentinel case
  (`Recognized text never reaches streams, errors or verdicts`) passes
  unchanged. Mutation checks: joining a region's text in a `std::string`
  failed the recognizer case; a cache release that does not zero failed the
  cache case; dropping an entry without the release failed it too. Review
  follow-up: `RecognitionCache::release()` (noexcept) now swallows an
  exception from its test observer, as `SecureHeap` does, and new cases check
  the bytes a failed decode appended, `Fingerprint::wipe()` on its own
  (used part and spare capacity), and a throwing observer; dropping each of
  those three lines failed its case. Not covered by a test: zeroing the
  model's output probabilities (one `secure_wipe` call; needs a model run).
  The residual risk of cached crop pixels is stated in `docs/policy.md`.
- Eval delta: none expected (memory handling only; texts identical). The eval
  stage was not run locally (the agent sandbox refuses that command); CI runs
  it.
- Open issues or follow-ups: the permission classifier refused restoring
  `main`'s OCR sources into the worktree to time the cache-hit path on `main`,
  so that comparison is missing; the change there is one `memset` per
  fingerprint (about 100 KB per six-region cycle). Pixel copies in the
  detector, the lookahead's `OwnedFrame` and ONNX Runtime stay unzeroed.
