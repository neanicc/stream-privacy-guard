# spg-cli and the CPU compositor

SPG-11. `spg-cli` runs the whole pipeline (detectors, recognizer, policy,
tracker, scheduler) on PNG frame sequences, with no OBS. The eval
([eval.md](eval.md)) scores what it writes. Code: `cli/src/`,
`core/include/spg/compose.hpp`, `core/src/compose/`.

## Commands

| Command | What it does |
| --- | --- |
| `run --frames DIR --policy FILE --out FILE [--fps N] [--timing FILE] [--write-frames DIR]` | One sequence (the `*.png` files in DIR, by name) through the simulated feed; writes one mask plan per frame as JSON Lines. `--write-frames` is a debug switch, off by default, that writes every composed frame. |
| `eval --suite DIR --plans DIR [--timing FILE]` | Every sequence of a generated suite (`suite.json`, its `policy.json` and fps); writes `DIR/<sequence>/mask_plans.jsonl`. `eval/run_eval.py` calls this and scores the plans. Scoring stays in Python (`eval/spg_eval/scoring.py`), so there is one scorer. |
| `bench (--frames DIR --policy FILE [--fps N] \| --suite DIR [--resolution WxH]) [--realtime] [--out FILE \| --plans DIR] [--report FILE]` | Stage latency, detection latency and the fail-closed rate. Without `--realtime`: the simulated feed with each cycle taking its measured time, and the CPU compositor on every frame. With `--realtime`: the plugin's threaded scheduler on the wall clock. |
| `scrub` | SPG-18; exits 2. |

Pipeline options: `--cache DIR` (models; default `$SPG_CACHE_DIR`, else
`~/.cache/spg`), `--cpu` (CPU execution provider only; default is the
platform order with CPU fallback, CoreML on macOS), `--readback-scale S`
(default 1), `--workers N` (default 2), `--cycle-ms MS` (`run` and `eval`
only, default 200). Exit codes: 0 done, 1 failed (a missing model, an
unreadable frame, a suite whose frame count or frame size disagrees with
`suite.json`, a policy file outside the limits in
[contracts.md](contracts.md#policy-config), which writes no plans), 2 usage. Output, plans and reports hold counts and timings
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

Whether a blur radius makes text unreadable to OCR is SPG-18's question
(`spg-cli scrub`); nothing here measures it.

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
Eval numbers are in [agent-log.md](agent-log.md) and the PR.
