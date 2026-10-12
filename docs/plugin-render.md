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
cannot get a slot (ring full or texture creation failed) is still scheduled
and is drawn as opaque black when released.

## Resize, 0x0 and settings changes

- A **resize** goes to the scheduler, which rescales held masks (they only
  grow). Buffered frames of the old size are drawn scaled to the output with
  their own masks.
- A **0x0 target** draws nothing; frames that come due meanwhile are dropped
  unseen, and the scheduler, the buffered frames and the held masks are
  kept. When frames return the scheduler is told the content may have
  changed (`mark_discontinuity`).
- A **settings change** builds a new scheduler off the render thread (in
  `update` or the watch-list button handler). The old one keeps releasing its
  buffered frames with its own masks until it is empty. Frames of the new one
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
| Settings change racing with render | The settings side hands over a complete generation under a lock held only for a pointer swap; the render thread takes it at the start of a frame (`PipelineHost`). A failed build (`fail()`) covers every frame with a solid fill until the next successful hand-over; the old scheduler is never left running in its place |
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
| First frames | Covered whole until the scheduler releases a frame (`Starting`), or for good while no policy is set (`NoDetection`) |

## Threads and unload

The render thread never takes the filter's settings lock; it takes a hand-off
lock only to swap in a new scheduler. Schedulers are never destroyed on the
render, video or UI thread: retired ones go to a reaper thread, whose
destructor calls wait for their workers. Queueing one allocates nothing (the
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
| Grey | Starting, or detection off (no PolicyConfig) |
| Blue | Settings changed (old hold) |
| White | The frame could not be kept (no texture) |
| Yellow | Nothing released for over 100 ms (stall) |
| Cyan | Inconsistent state (see below) |

## Audio

A video filter cannot delay audio, so the plugin does not try. The properties
tell the user to set **Sync Offset** (Edit > Advanced Audio Properties) to the
lookahead on every audio source that belongs with the filtered video: the
source's own audio, the microphone, desktop audio. The filter never changes
other sources' offsets itself (SPG-13 decision).

## This build

Detection models are not loaded by the plugin yet: `make_stages()` returns no
detectors, so every scan fails (`ScanFailure::MissingStage`) and every frame
airs covered whole after the lookahead. The readback, ring, release and blur
paths all run; loading the models and ONNX Runtime in the module is a
follow-up.

## Evidence and limits

`plugin/tests/pipeline_test.cpp` (all OSes): fail-closed plans on every tick
of every case, warm-up, retroactive and held masks, readback timing, fallback
modes, resize, 0x0, settings change, ring exhaustion, 40 seeded runs with
failures, resizes, 0x0 ticks and restarts, ring, pool, mask geometry (2000
seeded rects) and the reaper. The GPU code and the effect need a graphics
context, which headless libobs in CI does not have: they are checked live
([live-check.md](live-check.md)). HDR sources are captured as 8-bit BGRA
(untested). The render-thread cost measured on the dev Mac is in the SPG-14
entry of [agent-log.md](agent-log.md).
