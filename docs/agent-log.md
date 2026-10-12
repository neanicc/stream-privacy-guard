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
  input marker. Every archive is in `deps/manifest.json` with SHA-256; OBS's own
  configure-time downloads (obs-deps, Qt, CEF) are disabled by an override
  `buildspec.cmake`, and the 32-bit helper configure OBS always runs on Windows
  gets the pinned x86 obs-deps archive placed in `<source>/.deps`. 32.1.2 rather
  than 32.2.2: 32.2 requires Xcode 26.5 and the `macos-15` runner tops out at
  26.3. OBS 32.1 enables Swift only from a plugin directory, so the macOS build
  injects `enable_language(Swift)` (`CMAKE_PROJECT_INCLUDE`) for libobs-metal.
  `fetch_deps.py` gained `.deb` unpacking, `include` member filters, explicit
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
