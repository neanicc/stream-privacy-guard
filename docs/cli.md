# spg-cli and the CPU compositor

SPG-11. `spg-cli` runs the whole pipeline (detectors, recognizer, policy,
tracker, scheduler) on PNG frame sequences, with no OBS. SPG-18 adds
`scrub`, the leak check for recorded sessions ([below](#scrub)). The eval
([eval.md](eval.md)) scores what it writes. Code: `cli/src/`,
`core/include/spg/compose.hpp`, `core/src/compose/`.

## Commands

| Command | What it does |
| --- | --- |
| `run --frames DIR --policy FILE --out FILE [--fps N] [--timing FILE] [--write-frames DIR]` | One sequence (the `*.png` files in DIR, by name) through the simulated feed; writes one mask plan per frame as JSON Lines. `--write-frames` is a debug switch, off by default, that writes every composed frame. |
| `eval --suite DIR --plans DIR [--timing FILE]` | Every sequence of a generated suite (`suite.json`, its `policy.json` and fps); writes `DIR/<sequence>/mask_plans.jsonl`. `eval/run_eval.py` calls this and scores the plans. Scoring stays in Python (`eval/spg_eval/scoring.py`), so there is one scorer. |
| `bench (--frames DIR --policy FILE [--fps N] \| --suite DIR [--resolution WxH]) [--realtime] [--out FILE \| --plans DIR] [--report FILE]` | Stage latency, detection latency and the fail-closed rate. Without `--realtime`: the simulated feed with each cycle taking its measured time, and the CPU compositor on every frame. With `--realtime`: the plugin's threaded scheduler on the wall clock. |
| `scrub --frames DIR --planted FILE [--fps N] [--report FILE] [--sensitivity normal\|high]` | Every frame of a recorded session (the `*.png` files in DIR, by name) through the text detector and recognizer; reports each region whose text matches a planted string. Exits 3 when any does. See [Scrub](#scrub). |

Pipeline options: `--cache DIR` (models; default `$SPG_CACHE_DIR`, else
`~/.cache/spg`), `--cpu` (CPU execution provider only; default is the
platform order with CPU fallback, CoreML on macOS), `--readback-scale S`
(default 1; not `scrub`), `--workers N` (default 2; for `scrub` the frames
scanned at once), `--cycle-ms MS` (`run` and `eval` only, default 200). Exit
codes: 0 done (for `scrub`: no leak), 1 failed (a missing model, an
unreadable frame, a suite whose frame count or frame size disagrees with
`suite.json`, a policy file outside the limits in
[contracts.md](contracts.md#policy-config), which writes no plans), 2 usage,
3 `scrub` found a leak. Output, plans and reports hold counts and timings
only; recognized text and watch-list entries are never printed, and parse
errors name fields and offsets.

## Drivers

Both drive the SPG-10 scheduler ([scheduler.md](scheduler.md)) with the
policy's lookahead, retro window, hold, margin and fallback.

- **Simulated** (`run`, `eval`, `bench`): `spg::Lookahead` on a
  `ManualClock`, one thread. Frame `i` arrives at `i * 1e9 / fps` ns (the
  generators' timestamps). `workers` simulated workers each take the newest
  queued scan when free; a cycle runs `run_scan()` for real and finishes
  `cycle` of simulated time later, when its result is applied. Frames are
  released at frame ticks once due. With a fixed cycle (`run`, `eval`) the
  plans depend only on the frames, the policy and the model outputs, not on
  the speed of the machine, so CI and the dev Mac score the same schedule.
  `bench` uses each cycle's measured wall time instead.
- **Realtime** (`bench --realtime`): `spg::Scheduler` with worker threads and
  `SteadyClock`. A decoder thread keeps up to 16 frames ready; the feed pushes
  one frame every `1/fps` s and releases due frames, as the plugin's render
  callback will. Each frame is stamped with the time it was due, not the
  time it was pushed: a live source keeps its pace, so when decoding falls
  behind, the delay is spent from the frame's lookahead and counted in its
  detection latency (and fail-closed frames) rather than slowing the feed
  the workers see. Frames pushed more than one period late are counted as
  `late_frames`. `eval/run_eval.py` prints the count with the realtime line
  and on its own `unscored: realtime late_frames N` line. **Nothing scores
  it yet:** a late-frame limit needs a key in the human-owned
  `eval/thresholds.toml`, tracked in #68 (SPG-31). Until then a replay that
  feeds late but stays inside the lookahead passes; its delay still counts
  in detection latency and fail-closed frames, which are scored.
- **Padding.** After the last frame both keep pushing copies of it until every
  real frame is released. A live source keeps producing frames; without them
  the last `retro` of every sequence could never be covered by a later scan
  and would always fail closed, which no stream would do. Plans are written
  for real frames only, with their own timestamps.
- **Readback.** The frame is scaled by `--readback-scale` with area averaging
  (scale 1 scans the full frame). SPG-14 decides the plugin's readback size;
  the CLI default must follow it.
- Detection latency per cycle: from its frame's timestamp to its end (the
  simulated end, or in realtime the end of the cycle's last stage call,
  policy calls included; the apply under the scheduler's lock is not
  included).

### What each driver is evidence for

The CI eval (`scripts/ci/run.sh eval`, which runs `spg-cli eval`) uses the
simulated driver with a fixed 200 ms cycle. A cycle that takes 2 s of wall
time on a loaded runner still counts as 200 ms, so the CI eval is evidence
about plan logic only: retroactive coverage, hold and margin, fallback rules,
rect fitting, and recall and over-blur on the generated frames. It says
nothing about timing margin, about how close a cycle comes to the lookahead
deadline, or about failing closed when the machine is slow. Its detection
latency is simulated time.

Timing evidence comes from `bench --realtime` (the threaded scheduler on the
wall clock) and the local realtime suite (`scripts/ci/run.sh eval --local`),
which scores the fail-closed rate and p95 detection latency. Tuning them to
pass is SPG-16 (#17). Neither is live OBS evidence: the plugin draws with its
own GPU effect ([live-check.md](live-check.md)).

## Stage timing

Every stage is wrapped (`timed()`) and timed with `steady_clock`:
`decode` (PNG decode, including the compositor's reloads in simulated
`bench`), `readback` (scaling, when the scale is below 1), `face_detect`,
`text_detect`, `recognize`, `policy` (per text), `cycle` (one whole
`run_scan()`, simulated drivers) and `compose` (CPU compositor, simulated
`bench` and `--write-frames`). `--timing` / `--report` write count, total,
mean, nearest-rank p50 and p95, and max per stage, plus the detection
latencies, fail-closed count and rate, padding and late frames, the scheduler
counters and the providers each model runs on.

## CPU compositor

`spg::compose(image, plan, action)` draws a released frame on Source-size
BGRA pixels. It is the reference the GPU effect (SPG-14) must match or exceed.

- `FallbackAction::None` with `plan.fallback` false: each rect, rounded
  outward to whole pixels and clipped, in its style. Blur is a box blur run 3
  times per axis (close to a Gaussian), radius `max(12, 0.5 * shorter side)`,
  mixing only pixels of the same rect; pixelate uses blocks of
  `max(8, 0.25 * shorter side)`; solid fill is black. A blur or pixelate
  rect under 2 px on a side is filled instead, since a 1x1 blur or block
  returns the pixel unchanged. Blur and pixelate mix alpha with the colour
  channels and a filled pixel is opaque, so a source whose content is in its
  alpha (black text on a transparent background) is hidden too, as the GPU
  effect does ([plugin-render.md](plugin-render.md)).
- `BlurSource`: the whole frame blurred. `SolidFill`: the whole frame filled.
  `HoldLastScanned` is the caller's job (show the re-emitted frame; the CLI
  keeps the last clean composed frame); drawn by `compose()` it blurs the
  whole frame, as does a plan with `fallback` set and no action. A frame that
  failed closed is never shown as is.
- Rect capacity: `kMaskRectCapacity` = 64 rects per frame. The GPU effect
  draws a fixed array, so every plan is fitted with `fit_mask_rects()` before
  it is drawn or written: rects inside another are dropped, then the pair
  whose hull adds the least area beyond their union (overlap counted once)
  merges until 64 remain (stronger style wins).
  Every input rect stays inside some output rect, so fitting only adds blur.
  Fitting is O(n^3) in the worst case (each merge rescans all pairs); only
  `spg-cli` calls it, offline, once per released frame. The plugin does not.
  The scheduler's `max_retro_rects` (64) and the tracker's
  `max_rects_per_track` (8, so eight tracks fit unmerged) are capped at the
  capacity by `driver_config()`.

Whether a blur radius makes text unreadable to OCR is measured by
`spg-cli scrub` (below) on synthetic frames in
`tests/integration/scrub_pipeline_test.cpp`, and on a real recording in
[live-check.md](live-check.md) part F.

## Scrub

SPG-18. `scrub` answers one question about a recorded session: in which
frames is a planted test string still readable? It is the automated half of
the leak test in [live-check.md](live-check.md); a human still looks at every
flagged frame and checks faces by eye. Code: `cli/src/scrub.cpp`.

- **Inputs.** `--frames`: the recording as PNG frames (`ffmpeg -i rec.mkv -vf
  fps=60 -start_number 0 DIR/%06d.png`), any size, sorted by file name; frame
  `i` is at `i / fps` s (`--fps`, default 60). `--planted`: a UTF-8 file with
  one planted string per line (LF or CR LF; blank lines skipped). Strings are
  numbered from 1 in file order.
- **Per frame**, at full resolution: the text detector, the recognizer on
  every region, then each region's text against each planted string with a
  `PolicyEngine` restricted to the watch-list category, one engine per
  string, so a hit names the string by number. Matching is the policy
  engine's own (normalization, OCR confusions, edit budget, partial reveals,
  [policy.md](policy.md)), at the plugin's **High** sensitivity by default
  (3 edits, ratio 0.3, partials from 5 characters); `--sensitivity normal`
  uses spg-core's defaults. A leak check errs toward reporting: a false alarm
  costs a look at one frame.
- **Offline settings** (`StageOptions::thorough`): text detector tiles overlap
  by half a tile (320 px instead of 64), so a line up to 320 px tall is never
  cut by a tile edge, and candidate boxes are kept from a mean probability of
  0.3 instead of 0.5. `run`, `eval` and `bench` keep the plugin's settings.
- **Output.** One `leak: frame <i> (<file>, <s> s): planted #<n> at x y, WxH`
  line per matching region, a line per planted string (frames, first and
  last), the stage table, then `PASS: ...`, `LEAK: ...` or, when no
  text region was read in any frame, `NOTHING CHECKED: ...` (exit 1: a wrong
  folder or a detector that sees nothing is not a pass). `--report` writes
  the same as JSON (`result`, `leak_frame_indices`, `leaks` with frame, file,
  time, box, detector score and planted numbers, per-string counts, stage
  timing). Progress goes to stderr every 100 frames.
- **Secrecy.** Planted strings and recognized text are `Secret`s; the
  planted file (at most 1 MiB) is read into one buffer of its exact size,
  never reallocated, and wiped once split. Output, errors and the report
  hold frame numbers, file names, times, boxes and planted numbers only; a
  planted string that cannot be matched (no letters or digits) is named by
  number. `scrub` writes no frames.
- **Fail closed.** A frame that cannot be decoded, a stage that throws or a
  recognizer that returns the wrong count stops the scrub with exit 1 and no
  verdict: an unchecked frame never counts as clean. Frames are scanned on
  `--workers` threads; the output is in frame order whatever the count.
- **Limits.** Scrub finds what this OCR can read. Text the detector misses
  (unusual fonts, very small text, strong perspective) is not reported, so the
  live check includes a positive control (the same planted strings recorded
  without the filter must all be flagged) and a by-eye pass. Faces are not
  checked. The planted strings are compared one region at a time, as the
  policy does; a string split across two detected regions is matched only
  through the partial-reveal rule.

## Dependencies

PNG decoding and encoding use stb_image 2.30 and stb_image_write 1.16 (MIT or
public domain), downloaded by `cli/CMakeLists.txt` from commit `31c1ad37` and
checked against a pinned SHA-256 per header, compiled in their own target
without the project's warning flags. The pipeline commands need the
`SPG_WITH_DEPS` build; without it they exit 1 with a message.

## Evidence

`tests/unit/compose_test.cpp` (`unit.*`, `[compose]`): fitting never loses
coverage (200 seeded scenes up to 300 rects), pixels outside the rects never
change, detail inside is gone, in colour and in alpha, fallbacks cover the
whole frame. `tests/unit/cli_test.cpp` (`cli.*`): the simulated driver against
scripted stages (timestamps, retroactive coverage from the first frame,
padding, determinism with a fixed cycle, fail closed on slow, throwing or
missing stages, capacity), the realtime driver, and `run`, `eval` and `bench`
through `run_cli()` with fake stages, including the watch-list sentinel never
reaching output. `tests/integration/cli_pipeline_test.cpp`: `spg-cli run` on
the pinned models over 36 drawn frames (watch-list line and card covered from
their first frame, face covered, decoy not masked); skips without the models.
`tests/unit/scrub_test.cpp` (`cli.*`, `[scrub]`): `scrub` with fake stages
(leak frames, times, boxes and numbers; pass; no text read is exit 1, not a
pass; same output with 1 and 4 workers; exit 1 on an unreadable frame or a
short recognizer; a throwing progress callback is an error, not a
terminate; planted-file parsing; neither planted string in any output).
`tests/integration/scrub_pipeline_test.cpp`: `scrub` on the pinned models
over nine drawn 1280x720 frames with three planted strings (20 px name line,
20 px card whose mask overlaps the name's, 56 px line): flagged when sharp,
not flagged under the CPU
compositor's blur and pixelate, an emulation of the plugin's GPU effect
(its own `mask_levels.cpp`, Smooth and Blocks, each mask sampled from the
untouched frame) or either whole-source blur.
Eval numbers are in [agent-log.md](agent-log.md) and the PR.
