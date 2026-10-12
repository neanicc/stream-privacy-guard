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
> and pinned dependencies), and the OBS filter loads the pinned detection models
> (`scripts/fetch_models.py` puts them in `~/.cache/spg/models`) to blur detected
> faces and sensitive text; without them it covers the whole source (fails
> closed). It has not been checked in a live OBS session yet, and its recall on
> real content is not measured: do not rely on it for a stream.

## Install

No release has been published. Every pull request's CI run builds unsigned
packages for each OS (the `packages-<os>` artifacts of the Check workflow);
`scripts/ci/run.sh package` builds the same thing locally (see
[Packaging](#packaging)). Each archive holds an `INSTALL.txt` with these steps.

| OS | Package | Copy this | Into |
| --- | --- | --- | --- |
| macOS 14+ on Apple Silicon | `obs-spg-<version>-macos-arm64.zip` | `obs-spg.plugin` | `~/Library/Application Support/obs-studio/plugins/` |
| Windows 10/11 x64 | `obs-spg-<version>-windows-x64.zip` | the `obs-spg` folder | `C:\ProgramData\obs-studio\plugins\` |
| Linux x86_64 (built on Ubuntu 24.04) | `obs-spg-<version>-linux-x64.tar.gz` | the `obs-spg` folder | `~/.config/obs-studio/plugins/` |

1. Quit OBS and copy the plugin as above. It needs OBS Studio 32.1 or a later
   32.x release; it is built against libobs 32.1.2 and other versions are
   untested. On Linux, use a native OBS install (not the Flatpak) with glibc
   2.39 or newer.
2. The packages are not signed with a developer certificate or notarized
   (no signing identity exists for this project). The macOS bundle carries
   only an ad-hoc signature, so a browser-downloaded copy is quarantined;
   clear it with
   `xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-spg.plugin`.
3. Fetch the detection models once. Packages carry no model weights. From the
   unpacked folder (or a checkout of this repository) run
   `python3 scripts/fetch_models.py` (`python scripts\fetch_models.py` on
   Windows). It downloads YuNet and PP-OCR from the URLs pinned in
   `models/manifest.json`, checks each file's size and SHA-256, and installs
   them in `~/.cache/spg/models` (`%USERPROFILE%\.cache\spg\models` on
   Windows, or `$SPG_CACHE_DIR/models`). The plugin looks in its own
   `data/models/` first, then in that cache, and checks each file's size and
   SHA-256 again before loading it. Nothing downloads models at runtime.
4. Start OBS. To remove the plugin, delete what you copied; watch-list files
   stay in OBS's `plugin_config/obs-spg/` folder and the models in the cache
   until you delete them.

## Usage

1. Right-click the source to protect (a display, window or camera capture),
   choose **Filters**, click **+** under Effect Filters and add **Privacy
   Guard**.
2. Add watch-list entries: type a real name, address, account name or other
   string into **New entry** (masked as you type) and click **Add entry**.
   Entries cannot be read back once added; the list shows numbered entries
   you can remove one by one or all at once.
3. Choose what to detect: faces, watch-list entries, email addresses, phone
   numbers, payment card numbers, IP addresses, street addresses, API and
   stream keys. All are on by default.
4. **Lookahead** (default 500 ms, 100 to 2000 ms) holds frames back so they
   can be scanned before they air. The filter cannot delay audio: in
   **Edit > Advanced Audio Properties**, set **Sync Offset** to the same
   number of milliseconds on every audio source that belongs with this video
   (the source's own audio, microphone, desktop audio). **Video memory for
   the lookahead** (default 512 MiB) caps the frames held: when the lookahead
   does not fit at the source's size and frame rate it is shortened and the
   properties show a warning with the actual value; set Sync Offset to that
   value, or raise the budget.
5. **Mask hold** (default 750 ms) keeps a region blurred after its last
   detection. **When a frame cannot be scanned in time** chooses the
   fallback: blur the whole source (default), repeat the last scanned frame,
   or a solid fill. There is no setting that shows an unscanned frame.
6. The properties show how many frames aired with only detected regions
   masked and how many were covered whole, with the last reason. The
   **Debug overlay** (off by default) draws mask outlines and a reason colour
   on the source itself, so it airs too.

Settings and their limits: [docs/settings.md](docs/settings.md). Render path
and fallbacks: [docs/plugin-render.md](docs/plugin-render.md).

## Limitations

Read these before you rely on the filter.

- **Anything missed has already aired.** No setting makes detection complete.
  Keep personal information off screen; the filter is a second line.
- **Detection needs the models.** The plugin loads them once, on its own
  thread, when the first filter is created. Until they load, and whenever one
  is missing or fails its size or SHA-256 check, every frame airs covered
  whole; after fetching them, restart OBS.
- **Not verified in a live OBS session.** CI loads the module into a headless
  libobs on macOS and Windows and checks its exports on Linux; nobody has run
  it in the OBS app on a real source yet ([docs/live-check.md](docs/live-check.md)).
  The GPU readback and blur need a graphics context that CI does not have.
- **Accuracy is not measured.** The end-to-end synthetic eval needs the CPU
  compositor and `spg-cli run` (SPG-11), which have not landed, so no recall
  or over-blur figure exists against [eval/thresholds.toml](eval/thresholds.toml).
  Text tests draw strings with OpenCV's Hershey fonts, which are not real UI
  fonts.
- **Known recall gaps.** At 1080p the face detector sees a frame scaled by 1/3,
  so faces under about 30 px may be missed. Text with a cap height under about
  10 px may be missed; text taller than the detector's tile overlap can be cut
  by a tile edge; upside-down text is not read. OCR misreads beyond the
  watch-list's fuzzy tolerance are missed.
- **Delay and audio.** Video airs one lookahead late, and you must set the
  audio Sync Offset yourself. Frames are held at full resolution: about 240 MB
  of video memory at 1080p60 with the default 500 ms. The default 512 MiB
  budget shortens the lookahead at high resolutions (500 ms becomes 216 ms
  at 2160p60), which leaves less time to scan and so covers more frames whole.
- **Fail-closed blur.** When a frame cannot be scanned in time (slow machine,
  models missing or damaged, an invalid configuration, a broken watch-list
  file), the whole source is blurred, possibly for long stretches.
- **Hardware.** On Windows and Linux inference runs on the CPU only (no
  DirectML or CUDA build is packaged); on macOS ONNX Runtime may use CoreML.
  HDR sources are captured as 8-bit and are untested.
- **Platforms.** macOS 14 or later on Apple Silicon only (no Intel build);
  Windows x64; Linux x86_64 built on Ubuntu 24.04 (no Flatpak). Built against
  libobs 32.1.2.
- **Unsigned packages** (see [Install](#install)).

## Privacy

- **No network at runtime.** The plugin makes no network calls and has no
  accounts, telemetry or crash upload. The only download is the one you run:
  `scripts/fetch_models.py`, from pinned URLs, verified by SHA-256.
- **Watch-list entries stay out of scene collections.** OBS stores filter
  settings in plain text in the scene collection, which people share for
  support, so the settings hold only a random reference. The entries live in
  a separate file in OBS's `plugin_config/obs-spg/watchlists/` folder, created
  readable by your user only on macOS and Linux (on Windows it inherits your
  profile's permissions). Files of deleted filters stay until you remove them.
- **Kept out of logs and, where the OS allows, crash dumps.** The plugin logs
  counts and fixed messages, never entries or recognized text. Entries and
  OCR text are held in memory that is zeroed on release, locked against
  swapping where the OS permits, and excluded from crash dumps on Linux
  (`MADV_DONTDUMP`) and from Windows Error Reporting dumps. macOS has no such
  exclusion, a debugger can still read the memory, and hibernation writes RAM
  to disk. Details and the remaining gaps (ONNX Runtime's buffers, pixels):
  [docs/settings.md](docs/settings.md#watch-list-text-in-memory) and
  [docs/policy.md](docs/policy.md#recognized-text-in-memory).
- **Frames are not written to disk.** The plugin has no frame capture
  option; frames live only in memory for the lookahead.

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
The OBS plugin (`plugin/`, module `obs-spg`) builds by default: `setup` unpacks
the official OBS 32.1.2 Ubuntu package on Linux and, on macOS and Windows, builds
libobs once from the pinned OBS sources (Xcode or Visual Studio, a few minutes;
cached in `$SPG_CACHE_DIR/deps/libobs`). Set `SPG_BUILD_PLUGIN=0` for `setup`
and `build` to skip it.
[docs/ci.md](docs/ci.md) describes the CI checks these commands mirror.

### Packaging

```sh
bash scripts/ci/run.sh package                      # after build: dist/obs-spg-<version>-<os>.(zip|tar.gz)
bash scripts/package.sh build dist --with-models    # local only: also bundle the cached models
```

`scripts/package.sh` installs the plugin's `obs-spg` CMake component
([cmake/SpgInstall.cmake](cmake/SpgInstall.cmake)) into `build/package-stage/`,
adds `INSTALL.txt`, the license, third-party notices (ONNX Runtime, OpenCV,
nlohmann/json) and the model fetch script, then checks the staged tree before
writing the archive and `SHA256SUMS.txt`: the module, its data and every
license are present, every library the module imports is shipped beside it
or comes from OBS or the OS, nothing unused is shipped, macOS rpaths stay
inside the bundle and its signature verifies, and no model file is present
unless `--with-models` was given. `--with-models` copies the pinned models
from `$SPG_CACHE_DIR` into the plugin's `data/models/` (the first place it
looks) with their MIT and Apache-2.0 licenses, for local use; CI never
passes it.
Draft notes for the first release: [docs/release-notes/0.1.0.md](docs/release-notes/0.1.0.md).

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

## How this repository is built

Most code here is written by Claude Code agents working through GitHub issues
and pull requests. Each PR gets CI on Linux, macOS and Windows, a Claude staff
review against [AGENTS.md](AGENTS.md), and a Greptile review. A merge gate
decides what lands; a human owns the thresholds, CI workflows and live testing.
See [docs/autopilot.md](docs/autopilot.md) for the workflow and
[docs/agent-log.md](docs/agent-log.md) for the decision history.

## License

GPL-2.0, because OBS plugins link against libobs. See [LICENSE](LICENSE).
Packages include the licenses of ONNX Runtime (MIT), OpenCV (Apache-2.0) and
nlohmann/json (MIT) in the plugin's `licenses/` folder.
