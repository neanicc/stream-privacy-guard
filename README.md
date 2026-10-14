<div align="center">

# Stream Privacy Guard

**An OBS Studio filter that blurs faces, your personal details and other sensitive data before they reach your stream.**

[![Check](https://github.com/neanicc/stream-privacy-guard/actions/workflows/check.yml/badge.svg?branch=main)](https://github.com/neanicc/stream-privacy-guard/actions/workflows/check.yml)
[![License: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)
![OBS Studio 32.1+](https://img.shields.io/badge/OBS%20Studio-32.1%2B-302E31?logo=obsstudio&logoColor=white)
![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey)
![Status: alpha](https://img.shields.io/badge/status-alpha-orange)

[How it works](#how-it-works) · [Install](#install) · [Usage](#usage) · [Results](#results) · [Limitations](#limitations) · [Privacy](#privacy) · [Build from source](#build-from-source)

<!--
  TODO (after the live test, #71): replace the block below with the demo.
  Record it with made-up details only (for example "Jordan Fakeperson",
  "742 Evergreen Terrace"), never your real ones. Example:

  <a href="https://www.youtube.com/watch?v=VIDEO_ID">
    <img src="docs/media/demo.gif" alt="Stream Privacy Guard blurring a planted name in OBS" width="760">
  </a>
  <br><em>▶ Watch the full demo</em>
-->
<table><tr><td align="center" width="760">
<br>
🎬 <b>Demo coming soon</b><br>
<sub>A recording of the filter running live in OBS will go here.</sub>
<br><br>
</td></tr></table>

</div>

---

Streamers leak personal details by accident: a notification with their real name, an address in a delivery app, a card number in a browser tab. Once a frame has aired, it can't be taken back.

Stream Privacy Guard holds each frame back for a moment, scans it, and blurs anything sensitive **before** OBS encodes it. If a frame can't be scanned in time, it blurs the whole source rather than let an unchecked frame through.

> [!IMPORTANT]
> **This is a safety net, not a guarantee.** It lowers the chance that something private airs, but some things will still get through. Keep personal information off screen; treat this filter as a second line of defence.

## Features

| | |
| --- | --- |
| 🙂 **Faces** | Blurs faces in camera, window and display captures (YuNet). |
| 📝 **Your watch-list** | Blurs strings you add, such as your real name, address or alt accounts. Matching is fuzzy, so small misreadings by the text reader still match. |
| 💳 **Personal data** | Detects email addresses, phone numbers, payment card numbers, IP addresses, street addresses, and API and stream keys. |
| ⏪ **Retroactive masking** | When something is found, the blur is also applied to the buffered frames before it. A secret is covered from its very first frame. |
| 🔒 **Fails closed** | If a frame can't be scanned before its deadline, the whole source is blurred. No setting airs an unscanned frame. |
| 🏃 **Follows motion** | Masks track scrolling chat and moving windows, and stay on for a short hold after the last detection. |
| 🛜 **Fully local** | No network calls, accounts or telemetry. Watch-list entries never reach logs or scene collections. |

## How it works

![Stream Privacy Guard architecture](docs/architecture.svg)

<sub>The diagram is an Excalidraw drawing. To edit it, open <a href="docs/architecture.excalidraw"><code>docs/architecture.excalidraw</code></a> (or the SVG itself, which embeds the scene) at <a href="https://excalidraw.com">excalidraw.com</a>.</sub>

1. **Capture.** The filter keeps each full-resolution frame in a **lookahead ring** on the GPU (500 ms by default) and reads back a downscaled copy for scanning.
2. **Scan.** On worker threads, the **scheduler** runs the face detector beside the text detector. The text recognizer reads the detected text, and only reads a region again when its pixels change.
3. **Decide.** The **policy engine** fuzzy-matches recognized text against your watch-list and checks it against personal-data patterns. The **tracker** keeps masks on moving content.
4. **Mask.** The **mask planner** applies new masks to every buffered frame that hasn't aired yet. The render thread only composes masks and blurs on the GPU; it never waits for detection.
5. **Fail closed.** If a frame reaches its deadline unscanned, the fallback applies: blur the whole source (the default), repeat the last scanned frame, or a solid fill.

The core (`spg-core`) has no OBS dependency, so the same pipeline runs headless in `spg-cli` and in CI. Design details: [docs/plan.md](docs/plan.md), [docs/scheduler.md](docs/scheduler.md), [docs/policy.md](docs/policy.md) and [docs/plugin-render.md](docs/plugin-render.md).

## Install

> [!NOTE]
> **No release has been published yet.** Until then, use the `packages-<os>` artifacts from a green [Check run](https://github.com/neanicc/stream-privacy-guard/actions/workflows/check.yml), or [build from source](#build-from-source).
<!-- TODO (after the live test): once v0.1.0 is published, replace the note above with:
> **[Download the latest release](https://github.com/neanicc/stream-privacy-guard/releases/latest)**
-->

| OS | Package | Copy this | Into |
| --- | --- | --- | --- |
| macOS 14+ (Apple Silicon) | `obs-spg-<version>-macos-arm64.zip` | `obs-spg.plugin` | `~/Library/Application Support/obs-studio/plugins/` |
| Windows 10/11 x64 | `obs-spg-<version>-windows-x64.zip` | the `obs-spg` folder | `C:\ProgramData\obs-studio\plugins\` |
| Linux x86_64 | `obs-spg-<version>-linux-x64.tar.gz` | the `obs-spg` folder | `~/.config/obs-studio/plugins/` |

1. **Quit OBS and copy the plugin** as shown above. You need OBS Studio 32.1 or a later 32.x release.
   - On Linux, use a native OBS install, not the Flatpak, with glibc 2.39 or newer.
   - On Windows, the plugin uses the Visual C++ 2015–2022 Redistributable (x64) that OBS already requires.
2. **macOS only: clear the quarantine flag.** The packages aren't notarized, so macOS blocks a downloaded copy until you run:
   ```sh
   xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-spg.plugin
   ```
3. **Fetch the detection models once.** The packages don't include model weights. From the unpacked folder, run:
   ```sh
   python3 scripts/fetch_models.py      # Windows: python scripts\fetch_models.py
   ```
   This downloads YuNet and PP-OCR from the pinned URLs in [`models/manifest.json`](models/manifest.json), checks each file's SHA-256, and installs them in `~/.cache/spg/models`. Nothing downloads at runtime.
4. **Start OBS.** To uninstall, delete what you copied. Watch-list files stay in OBS's `plugin_config/obs-spg/` folder and the models stay in the cache until you delete them.

## Usage

1. Right-click the source to protect (a display, window or camera capture) and choose **Filters**. Click **+** under *Effect Filters* and add **Privacy Guard**.
2. **Add watch-list entries.** Type your real name, address, account names or other strings into **New entry**, which is masked as you type, then click **Add entry**. An entry can't be read back after you add it.
3. **Choose what to detect.** Faces, watch-list entries and every personal-data category are on by default.
4. **Sync your audio.** The filter delays video by the lookahead (500 ms by default) but can't delay audio. In **Edit › Advanced Audio Properties**, set **Sync Offset** to the same number of milliseconds on every audio source that goes with this video.

<details>
<summary><b>Settings at a glance</b></summary>

| Setting | Default | What it does |
| --- | --- | --- |
| Lookahead | 500 ms (100–2000) | How long frames are held for scanning. Longer means more scanning time, but more stream delay. |
| Video memory for the lookahead | 512 MiB | Caps the frames held on the GPU. If the lookahead doesn't fit, it is shortened and a warning shows the actual value. |
| Mask hold | 750 ms | How long a region stays blurred after its last detection. |
| Watch-list sensitivity | Standard | Standard or High fuzzy-matching tolerance. |
| When a frame can't be scanned in time | Blur the whole source | Alternatives: repeat the last scanned frame, or a solid fill. No option shows an unscanned frame. |
| Debug overlay | Off | Draws mask outlines on the source. The outlines air too, so use it only for testing. |

The properties panel also counts the frames aired with only detected regions masked, the frames covered whole, and the last reason. Full reference: [docs/settings.md](docs/settings.md).

</details>

## Results

### Synthetic evaluation

The pipeline is scored on generated scenes: chat, documents, notifications, scrolling text and faces. The commit tested is `f5502ce`, on an Apple M4 MacBook Air, with `bash scripts/ci/run.sh eval --local`. Limits are set in [`eval/thresholds.toml`](eval/thresholds.toml), and the metrics are defined in [docs/eval.md](docs/eval.md).

| Suite | Metric | Result | Target |
| --- | --- | ---: | ---: |
| Text | Watch-list recall | **99.9%** | ≥ 97% |
| Text | Caught on the first frame shown | **96.2%** | ≥ 95% |
| Text | Personal-data recall | **99.9%** | ≥ 90% |
| Text | Frames with a leak | **0.36%** | ≤ 2% |
| Text | Blurred area on clean frames | **0.25%** | ≤ 3% |
| Faces | Face recall | **100%** | ≥ 90% |
| Faces | Frames with a leak | **0%** | ≤ 5% |
| Realtime (1080p60) | Frames covered by the fail-closed fallback | **0–1%** | ≤ 1% |
| Realtime (1080p60) | Detection latency, p95 | **255 ms** | ≤ 400 ms |

Synthetic results don't prove live behaviour: generated scenes are simpler than a real desktop.

### Live test

<!--
  TODO (after the live test, #71): fill in this table from docs/live-check.md
  and link the results entry. Keep recordings and screenshots of the test
  out of Git.
-->

| Check | Result |
| --- | --- |
| Tested on | _pending: Mac model, macOS, OBS version, canvas_ |
| Plugin loads; planted strings blurred from their first frame | _pending_ |
| Scrolling chat, typing into a field, alt-tab, scene switches | _pending_ |
| `spg-cli scrub` on the recorded session (leak frames) | _pending_ |
| Render cost (Stats dock, filter on vs off) | _pending_ |
| Fail-closed under CPU load | _pending_ |

The protocol is in [docs/live-check.md](docs/live-check.md).

## Limitations

Read these before you rely on the filter.

- **Anything missed has already aired.** No setting makes detection complete.
- **Not yet verified in a live session.** See [Live test](#live-test). CI loads the module into a headless libobs, but the GPU readback and blur need a real graphics context.
  <!-- TODO (after the live test): reword this bullet to state what was tested and on what hardware. -->
- **Heavy CPU load.** When the machine is busy, for example with a game and encoding, more frames miss their deadline and get covered whole. In the tests, 1.3% of frames fell back under two busy CPU loops.
- **Known recall gaps.**
  - Faces under about 30 px at 1080p may be missed.
  - Text with a cap height under about 10 px may be missed, and upside-down text isn't read.
  - Text the reader misreads beyond the fuzzy-match tolerance is missed.
- **Over-blurring near secrets.** Harmless text close to sensitive text, especially in scrolling chat, is often blurred too, because the filter prefers blurring too much to too little.
- **Delay and memory.** Video airs one lookahead late, and you set the audio Sync Offset yourself.
  - At 1080p60, the 500 ms lookahead holds about 240 MB of video memory.
  - At 4K, the default budget shortens the lookahead.
- **Hardware.**
  - On Windows and Linux, inference runs on the CPU only.
  - On macOS, ONNX Runtime may use Core ML.
  - HDR sources are captured as 8-bit and are untested.
- **Platforms.**
  - macOS 14+ on Apple Silicon (no Intel build).
  - Windows x64.
  - Linux x86_64, built on Ubuntu 24.04.
- **Unsigned packages.** See [Install](#install).

## Privacy

- **No network at runtime.** The plugin has no network calls, accounts, telemetry or crash upload. The only download is the one you run yourself: `scripts/fetch_models.py`, from pinned URLs, with SHA-256 checks.
- **Watch-list entries stay out of scene collections.** People often share scene collections for support, so the filter settings hold only a random reference. The entries live in a separate file that only your user can read on macOS and Linux.
- **Kept out of logs and, where the OS allows, out of crash dumps.**
  - Entries and recognized text are never logged.
  - They're held in memory that is wiped on release and locked against swapping where possible.
  - They're excluded from crash dumps on Linux and from Windows Error Reporting dumps.
  - macOS has no equivalent exclusion.
  - Details: [docs/settings.md](docs/settings.md#watch-list-text-in-memory).
- **Frames are never written to disk.** They live only in memory, for the lookahead.

## Build from source

Requirements:
- CMake 3.25+.
- A C++20 compiler: Xcode clang, GCC or Clang, or Visual Studio 2022+.
- Python 3.12+ and `curl`.

Supported hosts are Linux x64, macOS on Apple Silicon and Windows x64 (use Git Bash).

```sh
bash scripts/ci/run.sh setup   # fetch pinned ONNX Runtime, OpenCV, libobs and models (SHA-256 verified)
bash scripts/ci/run.sh build   # build into build/ (Release, warnings as errors)
bash scripts/ci/run.sh test    # run every test with ctest
bash scripts/ci/run.sh eval    # generate the CI text suite, run the pipeline on it and score it
```

The plugin builds to `build/plugin/`. The command-line tool `build/cli/spg-cli` runs the same pipeline on PNG frames: `run`, `bench`, `eval`, and `scrub` for checking a recording for leaks. See [docs/cli.md](docs/cli.md).

<details>
<summary><b>Dependencies, caching and packaging</b></summary>

- **Dependencies.** `setup` downloads the archives listed in [`deps/manifest.json`](deps/manifest.json) once into `$SPG_CACHE_DIR` (default `~/.cache/spg`), and refuses any file whose hash doesn't match. CMake fetches Catch2, nlohmann/json and stb_image, also pinned by hash.
- **Libobs.** On Linux, it comes from the official OBS 32.1.2 package. On macOS and Windows, it is built once from the pinned OBS sources and cached.
- **Configuration flags.** Set `SPG_BUILD_PLUGIN=0` to skip the plugin, or configure with `-DSPG_WITH_DEPS=OFF` to build without ONNX Runtime and OpenCV.
- **Models.** Weights never enter Git, and nothing downloads them at runtime.

```sh
bash scripts/ci/run.sh package                      # dist/obs-spg-<version>-<os>.(zip|tar.gz)
bash scripts/package.sh build dist --with-models    # local only: also bundle the cached models
```

`scripts/package.sh` stages the plugin with `INSTALL.txt`, the license, third-party notices and the model fetch script. Before writing the archive and `SHA256SUMS.txt`, it checks the staged tree:
- every library the plugin imports is shipped beside it, or comes from OBS or the OS;
- macOS rpaths stay inside the bundle and its signature verifies;
- no model file is included unless `--with-models` is given.

CI checks: [docs/ci.md](docs/ci.md). Draft release notes: [docs/release-notes/0.1.0.md](docs/release-notes/0.1.0.md).

</details>

## Roadmap

- [x] Faces, watch-list and personal-data detection in a fail-closed realtime pipeline
- [x] Retroactive masking, motion tracking and temporal hold
- [x] Synthetic evaluation passing every limit in [`eval/thresholds.toml`](eval/thresholds.toml)
- [ ] Live OBS test and human leak test ([#71](https://github.com/neanicc/stream-privacy-guard/issues/71))
- [ ] First release, v0.1.0
- [ ] More headroom on busy machines ([#86](https://github.com/neanicc/stream-privacy-guard/issues/86))
- [ ] Less over-blurring near secrets ([#97](https://github.com/neanicc/stream-privacy-guard/issues/97))
- [ ] Windows GPU inference ([#25](https://github.com/neanicc/stream-privacy-guard/issues/25))

## How this repository is built

Most of the code here is written by coding agents working through GitHub issues and pull requests.
- Every PR gets CI on Linux, macOS and Windows, a staff review against [AGENTS.md](AGENTS.md), and a Greptile review.
- A strict merge gate decides what lands.
- A human owns the thresholds, CI workflows and live testing.

The workflow is in [docs/autopilot.md](docs/autopilot.md), and the decision history is in [docs/agent-log.md](docs/agent-log.md).

## License

GPL-2.0, because OBS plugins link against libobs. See [LICENSE](LICENSE).

Packages include the licenses of ONNX Runtime (MIT), OpenCV (Apache-2.0) and nlohmann/json (MIT). The detection models are YuNet (MIT) and PP-OCR via RapidOCR (Apache-2.0); fetching them separately keeps their licenses alongside.
