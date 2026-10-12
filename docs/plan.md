# Stream Privacy Guard implementation plan

Snapshot written 2026-10-12. Tickets, thresholds and recommendations below are
planning inputs, not evidence. Current behavior is whatever the code, tests and
eval output on `main` show.

## 1. Product direction

A streamer adds the **Privacy Guard** filter to a capture source in OBS (display
capture, window capture, browser source, game capture). While live, the filter
finds sensitive content and blurs it before the frame reaches the encoder:

- **Faces** in the filtered source (bystanders in IRL footage, faces in a video
  the streamer is reacting to). The webcam is a separate source and is only
  filtered if the user adds the filter to it.
- **Watch-listed strings** the user types in: real name, address, phone number,
  school, workplace, alt account names. Fuzzy-matched so small OCR errors and
  partial reveals still match.
- **Structured PII** by pattern: email addresses, phone numbers, payment card
  numbers (Luhn-checked), IP addresses, street addresses, common API token formats.

Honest framing is a product requirement. It is a safety net that cuts exposure.
A short lookahead delay buys the pipeline time, and when the pipeline cannot keep
up it fails closed by blurring the whole source. The README, UI and PR text never
claim it catches everything.

Tier 1 (this plan) is the dependable version above. Tier 2 (section 13) adds
semantic judgment about arbitrary text and is research-shaped; it starts only
after Tier 1 acceptance.

## 2. Prior art and wedge

| Project | What it does | Gap SPG targets |
| --- | --- | --- |
| PrivaStream | Real-time face blur and PII detection with stream delay, closed product | Open source, local-only, user watch-lists, game/desktop UI text |
| obs-detect (royshil/occ-ai) | OBS plugin, EdgeYOLO + YuNet faces, blur/pixelate masks | Archived late 2024, no OCR or text policy, box-only |
| obs-backgroundremoval | OBS plugin bundling ONNX Runtime + OpenCV across OSes | Reference for packaging and GPU readback, not privacy |
| OBS OCR (Tesseract) | Per-frame OCR to a mask | Weak on UI text, no notion of what is sensitive |

Wedge: retroactive masking inside a lookahead buffer, fail-closed scheduling,
watch-list fuzzy matching, and a reproducible leak-rate eval that anyone can rerun.

## 3. Architecture

```
            OBS render thread                       worker threads (spg-core)
 source ─► [obs-spg filter] ──readback (downscaled)──► detect faces ─┐
              │   ▲                                    detect text ──┼─► OCR ─► policy ─► tracker ─► mask plan
              │   └────────── mask plan per frame ◄────────────────────────────────────────────────────┘
              ▼
   lookahead ring buffer (GPU textures, `lookahead_ms`)
              │  retroactive masks applied to every buffered frame still unsent
              ▼
   GPU blur effect over mask rects ─► encoder / preview
```

- `core/` (`spg-core`) is pure C++20 with an injected `Clock` and no OBS
  headers. Everything that decides what to blur lives here and is testable on any
  CI runner with synthetic frames.
- `plugin/` (`obs-spg`) is a thin adapter: register the filter, read back a
  downscaled copy of each frame, hold full-resolution frames in the lookahead
  buffer, draw blur with a GPU effect, and expose settings.
- `cli/` (`spg-cli`) drives the core on PNG sequences: `run` writes mask JSON,
  `bench` measures stage latency and fail-closed rate under a simulated real-time
  feed, `eval` runs a suite end to end.
- `eval/` generates synthetic suites with exact ground truth and scores them
  against `eval/thresholds.toml`.

Planned layout:

```
CMakeLists.txt  CMakePresets.json  buildspec.json
core/include/spg/   core/src/{frame,runtime,detect,ocr,policy,track,schedule,mask}/
cli/   plugin/{src,data/effects,data/locale}/   tests/{unit,integration,fixtures}/
eval/{gen_synthetic.py,score.py,thresholds.toml,README.md}
models/manifest.json   deps/manifest.json   scripts/{ci/run.sh,fetch_models.py,...}
```

## 4. Timing model

- Each frame gets a monotonic timestamp. The plugin holds frames for
  `lookahead_ms` (default 500) before release.
- Workers process the newest readback frame. A detection at frame time t is
  applied to every buffered frame with timestamp ≥ t − `retro_ms` (default equal
  to `lookahead_ms`) that has not been released, so a string that appears and is
  detected 200 ms later is blurred from its first frame.
- Between detection cycles the tracker predicts box motion; masks carry a margin
  and a minimum hold (`hold_ms`, default 750) after the last confirming detection.
- **Deadline:** a buffered frame due for release whose scan coverage is incomplete
  (no completed detection cycle at or after its timestamp, within tolerance)
  triggers the fail-closed fallback for that frame. Default fallback: blur the
  entire source. Alternatives: hold last fully-scanned frame, solid fill.
- Audio: delaying video inside a filter desyncs audio by `lookahead_ms`. The UI
  shows the matching Sync Offset the user must set (or an OBS-wide stream delay
  alternative). Resolve the exact approach in the SPG-13 spike.

## 5. Contracts

Defined in SPG-02 and documented in `docs/contracts.md` once written.

- `FrameView`: pointer, width, height, stride, pixel format (BGRA, NV12, I420),
  timestamp. Non-owning.
- `Box`: float rect plus a `Space` tag (`Source`, `Readback`, `ModelInput`).
  Only `core/src/frame/geometry.*` converts between spaces.
- `Detection`: box (Source space), kind (`Face`, `TextRegion`), score, detector id.
- `TextHit`: detection plus recognized string (kept in memory only) and the policy
  verdict; the verdict carries reason codes such as `WATCHLIST`, `PII_CARD`, never
  the matched text.
- `Track`: id, box, velocity, last confirmed time, hold expiry, kind.
- `MaskPlan`: for one frame timestamp, a list of rects in Source space with blur
  style, plus a `fallback` flag when fail-closed applies.
- `PolicyConfig`: enabled categories, watch-list entries, fuzzy distance limits,
  hold/margin/lookahead, fallback mode.
- Eval ground truth (JSON per frame): frame index, timestamp, instances with id,
  kind (`watchlist`, `pii:<type>`, `decoy`, `face`), box, and visibility.

## 6. Detection and decision

- **Runtime:** ONNX Runtime with the CoreML execution provider on macOS, CUDA or
  DirectML on Windows when present, CPU otherwise. Sessions are created once and
  warmed up.
- **Faces:** YuNet (OpenCV Zoo) at reduced input resolution, NMS, configurable
  score threshold.
- **Text:** PP-OCR detection (DBNet) and recognition (CTC) in ONNX form, as
  packaged by RapidOCR. Detection runs on tiles of the downscaled frame;
  recognition runs on new or changed text tracks only and is cached per track.
- **Policy:** normalize (NFKC, case-fold, whitespace, common OCR confusions such as
  0/O and 1/l/I), then bounded edit-distance matching against each watch-list
  entry over token windows, so partial reveals ("123 Mapl") still match. PII uses
  validated patterns (Luhn for cards, NANP/E.164 for phones, RFC-ish emails,
  IPv4/IPv6, street-address heuristics, known token prefixes).
- Every model and dependency is listed in a manifest with URL, version, SHA-256
  and license. Fetch scripts refuse a hash mismatch.

## 7. OBS integration

- Filter type: video filter on any source. Recommended frame access is the
  `gs_texrender` + `gs_stagesurface` readback path used by obs-backgroundremoval,
  at a downscaled resolution, without stalling the render thread (double-buffered
  stage surfaces, map the previous frame's surface).
- Lookahead buffer: full-resolution textures in a ring. Memory cost is real
  (1080p BGRA ≈ 8 MB per frame; 500 ms at 60 fps ≈ 240 MB VRAM). Expose the limit
  and fall back to a shorter lookahead with a visible warning.
- Blur: one effect file (`plugin/data/effects/spg-mask-blur.effect`) blurring or
  pixelating inside mask rects; fallback blur covers the whole frame.
- Settings: categories, watch-list editor with masked entry, lookahead, hold,
  sensitivity, fallback mode, debug overlay (boxes and reason codes only, never text).
- Lifecycle: create/destroy/update, source resize, scene switches, plugin unload
  and worker shutdown must not leak threads or block OBS shutdown.

## 8. Security, privacy and service boundaries

- No runtime network access, accounts, telemetry or crash upload.
- Watch-list entries never reach logs, debug overlays, eval artifacts or errors.
  Tests assert this with a sentinel string.
- Debug frame capture is opt-in, writes to a user-chosen directory, and is
  labeled in the UI.
- Fetched artifacts are hash-pinned; licenses recorded; nothing downloaded at
  runtime by the plugin.

## 9. Tickets

Each ticket becomes a GitHub issue titled `SPG-xx: <title>` with labels `ticket`,
`tier-1` and its lane. One PR per ticket, `Closes #<issue>`. The autopilot may
split a ticket that grows beyond one reviewable PR into `SPG-xxa/b` issues that
carry the same lane and dependencies. Lanes are in [team-plan.md](team-plan.md).

| ID | Title | Lane | Depends on | Done when |
| --- | --- | --- | --- | --- |
| SPG-01 | Build foundation | build | — | CMake project with `spg-core`, `spg-cli`, tests (Catch2 pinned via FetchContent with hash); C++20, warnings as errors; ONNX Runtime and OpenCV fetched per OS from `deps/manifest.json` with SHA-256; ccache; `scripts/ci/run.sh setup/build/test` real; CI green on Ubuntu, macOS and Windows; README quickstart |
| SPG-02 | Core contracts and geometry | policy | 01 | Types in section 5; geometry conversions with round-trip property tests; JSON for config and ground truth; `docs/contracts.md` |
| SPG-03 | Synthetic eval suite and scorer | eval | 02 | Seeded generator for the text suite (UI-like backgrounds, chat, notifications, address bars, HUD panels; watch-list, PII, decoys; scroll, slide-in, fade; 1080p and 720p; clean frames); faces suite compositor with dataset fetched to cache; scorer implementing [eval.md](eval.md) and printing `THRESHOLDS: PASS/FAIL`; self-tests proving a no-op pipeline fails recall and a blur-everything pipeline fails the clean-frame budget |
| SPG-04 | Model manifest and runtime | vision | 01 | `models/manifest.json`, `scripts/fetch_models.py` (hash-verified, cache dir), ONNX Runtime session wrapper with EP selection and CPU fallback, warmup, clear errors |
| SPG-05 | Face detector | vision | 02, 04 | YuNet inference, preprocessing, NMS, detections in Source space; unit tests for mapping and empty frames |
| SPG-06 | Text detector | vision | 02, 04 | PP-OCR det on tiles, DB post-processing, boxes in Source space; tests on committed synthetic fixtures |
| SPG-07 | Text recognizer | vision | 06 | PP-OCR rec with CTC decode, crop/rectify, batching, per-track cache; tests on synthetic crops |
| SPG-08 | Policy engine | policy | 02 | Normalization, fuzzy watch-list matching, PII validators, reason codes; sentinel test proves watch-list text never reaches logs or errors |
| SPG-09 | Tracker and temporal hold | policy | 02 | IoU association, constant-velocity prediction, margin, hold; property tests that masks never shrink before hold expiry; injected clock |
| SPG-10 | Scheduler, lookahead and fail-closed | policy | 05, 06, 08, 09 | Ring buffer, worker pool, cadence, retroactive masks, deadlines, fallback modes, bounded queues; deterministic simulation tests with fake detectors and stalls |
| SPG-11 | CPU compositor and spg-cli | eval | 10 | CPU reference blur, `spg-cli run/bench/eval`, stage timing; `run.sh eval` wired end to end |
| SPG-12 | Accuracy pass | vision | 03, 07, 11 | Text and faces suites meet `eval/thresholds.toml`; each PR reports eval deltas; may be split |
| SPG-13 | OBS plugin shell | obs | 01 | obs-plugintemplate structure behind `SPG_BUILD_PLUGIN` with its helper scripts under `build-aux/` (never `.github/`); filter registers; properties stub; frame-access spike documented in agent-log; plugin builds in CI on all three OSes through `scripts/ci/run.sh` |
| SPG-14 | Lookahead buffer, readback and GPU blur | obs | 10, 13 | Readback without render stalls, texture ring, retroactive mask application, blur effect, visible fallback, audio offset guidance |
| SPG-15 | Settings and watch-list storage | obs | 08, 13 | Properties UI per section 7; storage per the open-question decision; masked entry; never logged |
| SPG-16 | Performance pass | build | 12, 14 | `spg-cli bench --realtime` meets `[realtime]` thresholds on the dev Mac; numbers and method recorded |
| SPG-17 | Packaging and docs | build | 14, 15 | `scripts/package.sh` writes installable artifacts per OS to `dist/` (CI already calls `run.sh package` and uploads them); README install, usage, limitations and privacy statement; draft release notes (no publishing) |
| SPG-18 | Leak scrubber and live QA protocol | eval | 11, 14 | `spg-cli scrub` reports frames in a recorded session where planted strings are still OCR-readable; [live-check.md](live-check.md) finalized; the live run itself is `needs-human` |

Parallel start: after SPG-01, lanes run SPG-02/04/13 together, then 03/05/06/08/09.

## 10. Capability limits

Not claimed in Tier 1: catching every frame; reading stylized, tiny, rotated or
heavily occluded text; understanding which arbitrary text is private without a
watch-list or pattern; audio redaction; protecting sources the filter is not on;
GPU validation on Windows hardware we do not have. Synthetic eval does not prove
live performance.

## 11. Verification strategy and acceptance checklist

Tier 1 is accepted when:

- [ ] All `tier-1` issues are closed by merged PRs or labeled `needs-human` with
      the exact remaining human step.
- [ ] `check` is green on the latest `main` commit (Ubuntu, macOS, Windows).
- [ ] `scripts/ci/run.sh eval --local` prints `THRESHOLDS: PASS` on the dev Mac
      (text, faces and realtime suites).
- [ ] The plugin loads in OBS on the dev Mac and blurs a planted watch-list
      string on a display capture (human, [live-check.md](live-check.md)).
- [ ] A recorded test session passes `spg-cli scrub` with zero leak frames for the
      planted strings (human-run, tool-verified).

## 12. Open questions

Autopilot resolves these with the recommended default unless marked **human**,
records the decision and evidence in `docs/agent-log.md`, and revisits only on
new evidence. Use `spg-grill-me` to challenge them interactively.

1. Frame access in OBS: texrender + stagesurf readback (recommended) vs async filter.
2. Lookahead memory: GPU ring at full resolution (recommended, with a cap) vs
   CPU-side copies vs shorter default.
3. Fail-closed default: blur whole source (recommended) vs freeze vs solid fill.
4. Watch-list storage: OBS filter settings are saved in plain text inside the scene
   collection JSON. Recommended: store entries in a separate file in the OBS
   config directory with user-only permissions and keep only a reference in
   filter settings; OS keychain is a later option.
5. OpenCV: pinned prebuilt minimal build as used by occ-ai plugins (recommended)
   vs source build in CI vs dropping OpenCV.
6. OCR models: PP-OCR mobile English/Latin via RapidOCR (recommended).
7. Hardware: Apple Silicon dev Mac is the only measured target in Tier 1.
8. **Human:** license. GPL-2.0-or-later is recommended because OBS plugins link
   libobs (GPL-2.0). Confirmed during bootstrap.

## 13. Tier 2 (after Tier 1 acceptance only)

Semantic sensitivity (NER plus a small local model deciding whether arbitrary text
is personal), game-HUD-aware detectors, audio PII bleeping, and a virtual-camera
delivery path for non-OBS apps. Each starts with a grill-me session and its own
eval suite.
