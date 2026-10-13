# Plugin render path: readback, lookahead ring and blur

SPG-14 ([plan section 7](plan.md#7-obs-integration)). How the Privacy Guard
filter turns each video frame into what airs. The decisions live in
`plugin/src/pipeline/` (no OBS, unit-tested on every OS with a fake GPU and a
synchronous scheduler); `plugin/src/render/` runs them with libobs graphics;
the scheduler is SPG-10's ([scheduler.md](scheduler.md)).

## One frame

`video_render` runs on the graphics thread for every view that shows the
source. The first call for a video frame time (`obs_get_video_frame_time()`)
does the work; later calls for the same time redraw the same plan.

1. **Capture.** The filter's target is rendered at its base size into a free
   slot of the lookahead ring (one `gs_texrender` per slot, the way
   `obs_source_process_filter_begin` renders a target).
2. **Readback.** The stage surface filled on the previous tick is mapped,
   copied into a pooled buffer and handed to the scheduler with that frame's
   timestamp; `Scheduler::push` matches it to the buffered frame it shows.
   If the scheduler wants this frame scanned, the slot is drawn scaled to fit
   1920x1080 (never scaled up, aspect kept) into the other of two stage
   surfaces. The render thread never waits for a scan: it copies at most one
   readback, takes the scheduler's lock for bookkeeping, and moves on.
3. **Push** the frame (its slot stays held while it is buffered).
4. **Release** every frame that is due and draw the newest:

| Released as | Drawn |
| --- | --- |
| `FallbackAction::None` | The frame's texture, each `MaskPlan` rect replaced from a coarse level (Blur: smooth, Pixelate: blocks, SolidFill: black) |
| `BlurSource` | The frame's texture blurred whole |
| `SolidFill` | Opaque black |
| `HoldLastScanned` | The last frame released clean in this scheduler, with its own masks, exactly as it aired; else `BlurSource` |
| any, but the frame has no texture | Opaque black |

Before the first release (the first `lookahead` after a start or a policy
change) and whenever no PolicyConfig is set (watch-list storage error, every
category off, settings rejected), the frame just captured is covered whole
with the configured fallback. Nothing is ever drawn from a frame the
scheduler did not release clean, except covered whole.

The output is composed in its own texture with replace blending (so a mask
region replaces the frame under it, alpha included, rather than blending over
a sharp copy) and then drawn like any filter output.

## Blur

`plugin/data/effects/spg-mask.effect` has two techniques, `Smooth` (bilinear)
and `Blocks` (point). The frame is reduced by a chain of halvings, each texel
the average of the 2x2 below it (a box filter), so level k averages about
2^k x 2^k pixels. A mask samples the level whose cells are at least a third of
its shorter side (and at least 8 px): the text line or face inside, already
grown by the tracker's margin, spans about two cells. The whole-source
fallback uses cells of at least 1/16 of the shorter side (128 px at 1080p).
Each mask region is rounded outward to whole cells, so it covers its rect
(`mask_region`, property-tested). These sizes are starting points; SPG-12/18
should confirm with `spg-cli scrub` that nothing under them stays readable.

## Lookahead ring

Slots are textures at the source's base size, created on first use and kept
for reuse; idle textures beyond four are freed. A slot is held by the
scheduler's buffer, by the frame on screen and by the last clean frame (for
`HoldLastScanned`). The ring has `2 * max_buffered_frames + 2` slots (one
scheduler draining after a settings change, plus those two), and memory is
what is actually buffered: about `fps * lookahead` frames, roughly 8 MB each
at 1080p (500 ms at 60 fps is about 240 MB of video memory). A frame that
cannot get a slot (ring full, VRAM budget reached or texture creation
failed) is still scheduled and is drawn as opaque black when released.

## VRAM budget

SPG-14c ([plan section 7](plan.md#7-obs-integration): "expose the limit and
fall back to a shorter lookahead with a visible warning"). The setting
**Video memory for the lookahead** (`vram_budget_mb`, default 512 MiB,
[settings.md](settings.md)) bounds the ring's textures. The math is in
`plugin/src/pipeline/ring_budget.cpp` and unit-tested.

- **Cost of a frame:** width x height x the ring format's bytes per pixel
  (`gs_get_format_bpp(GS_BGRA)` returns 32 bits, so 4 bytes), at the
  source's base size now: 7.9 MiB at 1080p, 31.6 MiB at 2160p. Driver
  padding is not counted.
- **Frames a lookahead needs:** `floor(lookahead / frame interval) + 1` frames
  inside the window, plus the frame on screen, the last clean frame and one
  frame of timing slack (`kRingReserveSlots` = 3). 500 ms at 60 fps needs 34.
  The frame interval is the canvas frame rate (`obs_get_video_info`).
- **Shortening:** when the configured lookahead needs more frames than the
  budget holds, the scheduler gets the longest lookahead that fits, rounded
  down to whole milliseconds (`fit_lookahead`). With the default 512 MiB:
  1080p60 holds 64 frames, enough for the default 500 ms and up to 1016 ms;
  2160p60 holds 16 frames, so 500 ms becomes 216 ms. A budget below four
  frames leaves a lookahead of 0: every frame is released before a scan
  can finish and is covered.
- **Through a new generation:** the fit is made in `publish` (settings
  change) and again in the filter's `video_tick` when the source size or the
  frame rate changes. A different result asks for a new scheduler of the
  same settings, built on the filter's build thread (see "Building
  generations" below); the running one keeps going until it is ready, then
  it is handed over like any settings change, so the old one's buffered
  frames still air with their masks and the switch rules above apply. A
  hidden (0x0) source keeps its last fit. A refit that fails (allocation, a
  worker thread that cannot start) covers the whole source and is asked for
  again once a second until one succeeds (`RefitSchedule`); a failed
  settings change is not, since the old settings must not run again.
- **Fail closed:** a shorter lookahead only gives scans less time; frames
  not scanned within it get the fallback as always. The scheduler's
  retroactive window is clamped to the shorter lookahead.
- **Hard cap per capture:** `Pipeline` counts the bytes of every ring
  texture it has allocated (held or idle) at the size it was captured. A
  capture into an idle slot replaces that slot's texture, so only the
  difference counts and a ring at the budget reuses textures instead of
  recreating them. A capture that would pass the budget frees the other
  idle textures first, then the one it would reuse; if it still does not
  fit, the frame gets no texture, is counted
  (`PipelineCounters::over_budget`) and is drawn as opaque black when
  released (`Cover::NoTexture`). This covers the time between a resize and
  the refitted generation, and the overlap of two generations after a
  settings change (the reserve does not include a second generation). A
  lower budget never takes a texture from a frame already buffered.
- **What the streamer sees:** while the lookahead is shortened the
  properties show a warning with the configured and the actual lookahead,
  the frames the budget holds and the source size, and advise to raise the
  budget or lower the lookahead and to set Sync Offset to the shorter value
  (the video is now delayed less than the audio offset). Frames refused by
  the budget are counted in the same warning. The log gets one line when the
  lookahead is shortened, shortened to another value, or restored, for
  example `VRAM budget: lookahead shortened from 2000 ms to 1016 ms; the
  budget of 512 MiB holds 64 frames of 1920x1080 (7.9 MiB each)`.
- **Not counted:** the readback, mask-level and output textures (at most a
  few frames' worth), other filters' rings (each filter has its own budget),
  and OBS itself.

## Resize, 0x0 and settings changes

- A **resize** goes to the scheduler, which rescales held masks (they only
  grow). Buffered frames of the old size are drawn scaled to the output with
  their own masks.
- A **0x0 target** draws nothing; frames that come due meanwhile are dropped
  unseen, and the scheduler, the buffered frames and the held masks are
  kept. When frames return the scheduler is told the content may have
  changed (`mark_discontinuity`).
- A **settings change** (`update` on the graphics thread, or the watch-list
  buttons on the UI thread) asks for a new scheduler, built on the filter's
  build thread. Until it is ready every frame is covered with a solid fill
  (`SettingsChanged`): the running scheduler scans with the old settings,
  which may lack a watch-list entry or a category just added. Once the new
  one is swapped in, the old one keeps releasing its buffered frames with its
  own masks until it is empty. Frames of the new one
  within the old hold of the switch are covered whole (`SettingsChanged`),
  because their held detections were in the old tracker. A debug overlay
  toggle does not restart the scheduler.
- Once the new scheduler has aired a frame, the old one's remaining frames
  never air and it is retired: with a shorter new lookahead they are older
  than what is on screen, and the picture would jump back.
- At most two schedulers are live (`Pipeline::kMaxGenerations`). Another
  change while one is still draining retires the draining one unless it is
  the one on screen; its frames never air and the output is covered
  (`Stalled`) until the new one releases. Rapid slider changes cannot pile up
  worker threads or ring slots.

## One consistent snapshot per frame

The render side never draws from state that may have drifted from the
scheduler's or the GPU's. Each rule below covers the frame whole (blur, or
solid fill when there is nothing trustworthy to blur) and is unit-tested:

| Case | Rule |
| --- | --- |
| Settings change racing with render | The build thread hands over a complete generation under a lock held only for a pointer swap; the render thread takes it at the start of a frame (`PipelineHost`). From the request until then every frame is solid (`SettingsChanged`). A failed build (`fail()`) covers every frame with a solid fill until the next successful hand-over; the old scheduler is never left running in its place |
| A newer request while a build runs | Only the latest request is offered: requests wait in one slot with at most one task posted for it, so an older one not started is replaced (its copy of the settings freed) and never built, and one finished late is retired unseen (`GenerationBuilder`). A slow build can never put superseded settings on air |
| Adoption fails (allocation) | `Pipeline::adopt` has the strong guarantee; the frame is solid, a second render of the same frame time is solid too, and the next frame retries the same generation |
| Policy becomes empty (storage error, no categories, rejected settings) | The next frame covers the live frame whole; the old scheduler's frames are never shown again |
| A release that does not match the frame buffered under its id (time, size, unknown id, clean action with a fallback flag or reason) | Solid, `Inconsistent` |
| A slot recycled, freed, never captured or captured at another size | Every plan names the capture serial of its texture; the GPU side compares it with what the slot holds right before drawing (`checked_kind`) and draws solid on any mismatch |
| A mask rect that is invalid or not in Source space | The frame is blurred whole, never partially masked |
| Releases stop (scheduler stalled or failing) | A released frame repeats for at most 100 ms, then the live frame is covered (`Stalled`) |
| Frame times jump by more than 200 ms (filter disabled, source not rendered) or go backwards | Discontinuity: no earlier scan or re-emit stands in for the new content; the HoldLastScanned anchor is dropped |
| 0x0 target | Nothing is drawn; held masks and buffered frames are kept; a discontinuity is marked when frames return |
| A GPU call throws | Caught: the frame has no texture and is covered; no ring slot leaks |
| Copying a released frame's masks fails (allocation) | The frame is solid; the slot it held goes back to the ring |
| Composing the output fails | The output texrender and blend state are closed first, then the output is solid. Mask levels are a fixed array, so composing allocates nothing |
| First frames | Covered whole until the scheduler releases a frame (`Starting`), or while no policy is set or the detection models are not loaded (`NoDetection`) |

## Building generations (SPG-14d)

In OBS a filter's `update` and `video_tick` run on the graphics thread, the
thread that also runs `video_render`, so nothing there may build a
scheduler (start its worker threads, wrap the loaded models in stages).
`publish` (settings), `tick` (VRAM refit) and the models' hand-over (loader
thread) only decide that a generation is needed: they compare what it
would be built from (policy, fallback, budget, fitted lookahead, models
generation) with the latest request and, if different, post the build to
the filter's build thread (`TaskThread`, started when the filter is created,
so asking never starts a thread). The build copies everything it needs;
the filter's settings lock is held only to record the request and, on the
build thread, to hand the result to `PipelineHost`. `tick` only tries that
lock and never waits. The rules live in `GenerationBuilder`
(`plugin/src/pipeline/builder.hpp`, no OBS) and are unit-tested with a
manual executor and with a real build thread:

- A settings change covers every frame (solid, `SettingsChanged`) from the
  request until its generation is offered.
- A rebuild of the same settings (refit, models ready) leaves the running
  generation in place until the new one is offered.
- Only the latest request is offered; superseded ones are never built or
  are retired to the reaper. Requests wait in one slot, and at most one
  build task is queued behind the one running, so repeated settings changes
  during a slow build do not pile up copies of the settings. At most two
  generations stay live, as before (`Pipeline::kMaxGenerations`).
- A failed build covers every frame (solid, `NoDetection`). A failed
  rebuild (always of the current settings, even one that superseded a
  settings change still building) is asked for again every
  `kRefitRetryNs`; a failed settings change is not retried and covers
  until the next change.
- The filter's destructor stops the build thread: a build in progress
  finishes (the models are already loaded, so it is short), queued ones are
  dropped.

## Threads and unload

The render thread never takes the filter's settings lock; it takes a hand-off
lock only to swap in a new scheduler. Schedulers are never built (see above)
or destroyed on the graphics thread (which runs `video_render`, `video_tick`
and a deferred `update`) or the UI thread: retired ones go to a reaper
thread, whose destructor calls wait for their workers. Queueing one allocates nothing (the
queue is linked through the schedulers); if the reaper thread cannot be
started, the scheduler stays queued until a later retire starts it or unload
destroys it. The render thread does take each scheduler's own lock in `push`
and `release_due`, which a worker also holds while it applies a result
(never while a stage runs); the cost is measured in the SPG-14 agent-log
entry, and SPG-16 owns the p99 budget. The plugin builds schedulers with a
one-hour `shutdown_timeout`, so a destructor joins its workers instead of
detaching them, and `obs_module_unload` joins the reaper. When it returns, no
scheduler thread is left running code from the module. A stage stuck for good
would hold OBS shutdown; detection stages have no unbounded wait today.

## What the streamer sees

- A covered frame is obvious on screen: the whole source is blurred or black.
- The properties show how many frames aired with only their masks and how
  many were covered whole (one count per output frame drawn; frames released
  together after a pause, and skipped, are not counted), and the last
  reason. OBS's log gets one line when
  the reason changes (at most one every 10 s), with the reason name only.
- **Debug overlay** (off by default, and it airs too): green outlines on mask
  rects, and for a covered frame a square in the top-left corner coloured by
  reason. Never text.

| Colour | Reason |
| --- | --- |
| Red | Not scanned in time |
| Orange | Released late (rendering stopped and resumed) |
| Magenta | Bad frame timestamp |
| Violet | A result could not be applied |
| Grey | Starting, or detection off (no PolicyConfig, or no detection models) |
| Blue | Settings changed (old hold, or the new scheduler still being built) |
| White | The frame could not be kept (no texture) |
| Yellow | Nothing released for over 100 ms (stall) |
| Cyan | Inconsistent state (see below) |

## Audio

A video filter cannot delay audio, so the plugin does not try. The properties
tell the user to set **Sync Offset** (Edit > Advanced Audio Properties) to the
lookahead on every audio source that belongs with the filtered video: the
source's own audio, the microphone, desktop audio. The filter never changes
other sources' offsets itself (SPG-13 decision).

## Detection models (SPG-14b)

The module loads YuNet, PP-OCR det and PP-OCR rec once, for every filter, on a
loader thread of its own (`plugin/src/models/`, `plugin/src/model_loading.cpp`)
that the first filter starts; the graphics and UI threads never open or
warm up a model. It looks in the plugin's data directory (`models/`, for a
packaged build) and then in `$SPG_CACHE_DIR/models` or `~/.cache/spg/models`
(`%USERPROFILE%` on Windows), where `scripts/fetch_models.py` puts them. It
reads each file once into memory, checks the size and SHA-256 of those bytes
against `models/manifest.json`, and hands ONNX Runtime the same bytes, so a
file replaced after the check is never loaded. It never downloads anything.

Until the models are ready, and for good when one is missing, damaged or fails
to open, a filter builds no scheduler: every frame is covered whole
(`Cover::NoDetection`), the properties say why, and the log has one line
(`detection models missing, covering every frame whole: <model>: <path>`;
a loader that threw or a thread that could not start gets a fixed reason in
the same line). When loading ends the loader asks for each filter's current
policy a new scheduler whose stages are the shared detectors, the recognizer
and a `PolicyEngine` built from the filter's `PolicyConfig` (no model is
opened there: the stages share the loaded ones); the log says `'<filter
name>': scanning with the detection models`, and the filter asks OBS to
rebuild a properties view that is open on it. A failed load is not retried
until OBS restarts.

ONNX Runtime 1.30.0 ships next to the module (`obs-spg.plugin/Contents/Frameworks`
with the module's only rpath on macOS, `onnxruntime.dll` beside `obs-spg.dll`,
`libonnxruntime.so.1` beside `obs-spg.so` with `$ORIGIN`); OpenCV is linked
statically, and on Linux the static libraries' symbols are kept out of the
module's dynamic symbol table. Each model session uses ONNX Runtime's default
execution providers (CoreML on macOS, with CPU fallback) and default thread
count.

## Evidence and limits

`plugin/tests/pipeline_test.cpp` (all OSes): fail-closed plans on every tick
of every case, warm-up, retroactive and held masks, readback timing, fallback
modes, resize, 0x0, settings change, ring exhaustion, 40 seeded runs with
failures, resizes, 0x0 ticks and restarts, ring, pool, mask geometry (2000
seeded rects) and the reaper; `plugin/tests/ring_budget_test.cpp` and the
`[budget]` pipeline cases: the budget math (5000 seeded fits), the warning
and its log line, refusal past the budget, a fitting budget that never
refuses (with HoldLastScanned re-emits), a lower budget that keeps buffered
frames, and a resize followed by a refitted generation; the `[build]` cases
(SPG-14d): a settings change covered until its generation is built, a
rebuild that keeps the running generation, superseded requests never
offered, waiting requests replaced rather than queued, failed builds covered
and retried only as a rebuild, a build that cannot be posted or prepared,
the build thread's order and stop, a build that runs on the build thread and
never on the caller (frames solid while it is held), a `ThreadedScheduler`
and its stage constructed on the build thread and swapped in by the render
side, and builds racing
with frames that never air an unscanned frame or leave more than two
generations live. The GPU code and the effect need a graphics
context, which headless libobs in CI does not have: they are checked live
([live-check.md](live-check.md)). HDR sources are captured as 8-bit BGRA
(untested). The render-thread cost measured on the dev Mac is in the SPG-14
entry of [agent-log.md](agent-log.md).
