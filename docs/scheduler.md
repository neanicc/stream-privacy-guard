# Scheduler, lookahead and fail-closed release

SPG-10 ([plan section 4](plan.md#4-timing-model)). This part decides, for every
frame, whether it may air with its masks or must get the fallback. It holds the
fail-closed, retroactive-masking and temporal-hold invariants from
[AGENTS.md](../AGENTS.md). Code: `core/include/spg/{scan,lookahead,scheduler}.hpp`,
`core/src/schedule/`, `core/src/mask/mask_rects.*`.

| Piece | What it is |
| --- | --- |
| `run_scan()` | One detection cycle over one readback frame: faces, text regions, recognition, policy. Never throws; any exception, invalid output or missing detector for an enabled category makes the cycle incomplete, and an incomplete result still carries the valid boxes found before the failure ([partial cycles](#partial-cycles)). |
| `Lookahead` | Single-threaded state machine: the frame ring, the bounded scan queue, cadence, coverage, retroactive masks, release and fallback. Tests and offline tools drive it directly with a `ManualClock`. |
| `Scheduler` | `Lookahead` behind a mutex plus a pool of worker threads that call `run_scan()`. This is what the plugin (SPG-14) uses. |

## Release rule

A frame with timestamp `t` is released once the clock reaches `t + lookahead`
(or earlier when more than `max_buffered_frames` are buffered). It is released
**clean** (`FallbackAction::None`, masks from its `MaskPlan`) only when all of
these hold; otherwise it gets the fallback:

1. A **complete** cycle on a frame with time `ts` in `[t, t + retro]` finished
   before the release. "Complete" means every enabled category had its
   detector and every stage that ran returned valid output: a null detector
   for an enabled category (`ScanFailure::MissingStage`, for example a model
   that failed to load), an exception of any type (`runtime::RuntimeError`
   such as `InferenceFailed`, `DecodeError`, `ContractError`, `bad_alloc`, or
   a non-`std` throw), a detection that fails `validate()` or has the wrong
   kind, or a recognizer returning the wrong number of texts makes it
   incomplete. A frame whose cycle result could not be fully applied (an
   allocation or tracker failure in `complete()`, or a complete result from
   before more resizes than the size history holds) fails closed
   (`FallbackReason::ApplyFailed`), as do the other frames that result could
   have masked: buffered frames from `ts - retro` on, frames pushed up to
   `ts + hold`, and the frame `hold_last_scanned` would show again.
2. That cycle scanned the **same content**: no resize and no
   `mark_discontinuity()` between the two frames.
3. The release is no later than `t + lookahead + release_grace`. A later
   release (the caller stopped asking) may have lost held detections to the
   tracker's pruning, so it fails closed.
4. The frame's timestamp increases and is within `clock_tolerance` of
   `clock.now()` when pushed. Otherwise the clock and the frames do not share
   one `FrameClock` time base (the tracker's pruning compares them), so the
   frame is never scanned and fails closed.

### One gate for every path that airs a frame

`Lookahead::gate(frame, at)` is the only code that decides a frame may air
clean; it checks rules 4, 1 (`apply_failed`, `covered`) and 3 in that order and
returns the `FallbackReason`. Two paths air a frame, and both call it:

| Path | What airs | Gate call |
| --- | --- | --- |
| `release_due()`, frame due at `t + lookahead` | the frame with its plan | `gate(frame, now)` |
| `release_due()`, buffer over `max_buffered_frames` (early) | the oldest frame with its plan | `gate(frame, now)`; same rule, only earlier |
| `hold_last_scanned` re-emit | the last clean frame again, as it was released | `gate(last_clean, when it was released)` |

A re-emit is a release: it shows the old frame's pixels with the masks it had
then. A complete cycle that finishes later and could have masked it (its
boxes reach the frame retroactively, `ts - retro <= t`, or by hold,
`t <= ts + hold`), or whose result cannot be applied, marks it
`apply_failed`, exactly as it marks a buffered frame in that reach. It then
fails the gate and the fallback blurs the source instead (SPG-10b). Before
this, the re-emit checked only the segment, so a frame could be shown again
without boxes held over it or found just after it.

Every other path only feeds the gate: `push()` sets `bad_timestamp` (rule 4)
and `apply_failed` for frames up to `ts + hold` of a failed result;
`complete()` sets `covered` (rules 1 and 2, complete results only) or
`apply_failed`; an incomplete result (whose boxes are still held, see
[partial cycles](#partial-cycles)), a job dropped from a full queue or by
`take_job()`, a cadence-skipped frame and a readback that matches no buffered
frame never set `covered`, so their frames stay `NotScanned` unless another
cycle covers them; a resize or
`mark_discontinuity()` starts a segment so earlier cycles do not cover later
frames. A result that lands after its frames were released (late
`complete()`) cannot rescue them; it still reaches the tracker and, through
the rule above, the re-emit candidate. `Scheduler`'s destructor drops a
result that returns after stop and never releases another frame; nothing
else drops, flushes or clears buffered frames.

Detectors run their tiles serially with no deadline and the policy has no
time bound, so nothing interrupts a stage. Deadlines are enforced at release:
a cycle that stalls, returns late or overruns in the policy simply has not
finished when its frames come due, and they fail closed. Its result is still
applied when it returns, so it helps frames that are not yet due.

Rule 1 is the coverage rule from the plan and the SPG-08b note: a cycle counts
for frames up to `retro` before it, because its boxes are applied to them
retroactively. A cycle *before* a frame does not count: content that appears
after the last scan would otherwise air unscanned.

This rule assumes that content visible at `t` is still visible at `ts`. A
secret that appears and disappears entirely between two cycles on one segment
is never seen, yet the frames around it are released clean. The property test
encodes the same assumption in its oracle, so it does not establish leak
safety. This is a tracked risk for SPG-12 (scroll and slide-in sequences) and
SPG-16 (cadence against `bench`): a shorter `retro` or a faster cadence
narrows the window.

The readback a cycle scans is matched to a buffered frame by timestamp, not
by the push it arrived with. A mapped stage surface usually holds the
previous frame (plan section 7), so `push(t, size, readback)` with
`readback.timestamp == t_prev` queues a cycle on `t_prev`. A readback whose
timestamp matches no buffered frame with a valid timestamp is not scanned.

### Partial cycles

A cycle can fail after some of its stages finished: faces found, then the text
detector throws; text regions found, then the recognizer or the policy throws.
The boxes found before the failure are real detections. Until SPG-10c
`run_scan()` dropped them, so the failed frame blurred but the boxes never
reached the tracker: if the face detector missed on the next cycle, frames
that cycle covered aired the face inside its hold.

`run_scan()` stops at the first failure and returns an incomplete result whose
`detections` keep what was found before it:

| Failure | Boxes kept |
| --- | --- |
| face detector throws | none (text never runs) |
| face detector returns malformed output | its valid faces (text never runs) |
| text detector throws | the faces |
| text detector returns malformed output | the faces and its valid regions |
| recognizer throws or returns the wrong count | the faces and every region |
| policy throws on some region | the faces and every region, including any it judged safe before failing |
| missing detector for an enabled category | none (nothing runs) |

A box is kept only when it passes `validate()` and has the stage's kind; a
non-finite, negative, wrong-space or wrong-kind box, or a bad score, is never
held. A region that no finished text stage cleared is masked (recall over
precision), as it is when no recognizer or policy is configured.

`Lookahead::complete()` treats an incomplete result with boxes like a complete
one for masking: the boxes enter the tracker (held for `hold`), become
retroactive rects on buffered frames in `[ts - retro, ts)`, and mark the
re-emit candidate in their reach. If they cannot be applied (tracker
rejection, allocation failure, or a job from before more resizes than the size
history holds) they fail closed exactly as a complete result's do
(`ApplyFailed` from `ts - retro` to `ts + hold`). The one difference is
coverage: an incomplete cycle never sets `covered`, so it never lets a frame
air clean (rule 1). It counts in `scans_failed`, never in `scans_completed`.
An incomplete result with no boxes changes nothing but `scans_failed`.

If an allocation fails while `run_scan()` collects the boxes a stage returned
(appending faces or regions, or the per-region policy flags), `detections`
holds only part of what the cycle saw, and nothing can hold the rest. The
result then sets `detections_lost`, and `complete()` fails closed exactly as
for a result it cannot apply (`ApplyFailed` from `ts - retro` to `ts + hold`)
instead of holding the partial list.

## Masks

`MaskPlan.rects` for a frame at `t` is the union of:

- `Tracker::masks(t)`: every detection seen in `[t - hold, t]`, with margin and
  constant-velocity prediction ([tracker.md](tracker.md));
- **retroactive rects**: each box of a cycle (complete, or partial) at `ts` in `(t, t + retro]`
  that finished while the frame was buffered, grown by the same margin and
  rounded outward to whole pixels. The tracker only covers frames at or after
  a detection, so this is what blurs a secret from its first frame.

A fallback frame's plan still lists these rects (`plan.fallback` is set).
Each retroactive box is masked where it was found, plus the margin. When the
box joined a track that has a velocity (`Tracker::update` reports it) and no
resize came between the cycle's frame and `t`, it is also masked moved back
along that velocity by `ts - t` (capped at the tracker's `max_prediction`),
with the same margin: where a sliding or scrolling secret was on the earlier
frame. The static rect stays, so back-projection only adds blur. SPG-12
measured why: a toast token sliding in at about 480 px/s on the text suite
was first scanned 100 ms after it appeared, and the static rect covered only
57-89% of it on its first five frames.

A constant velocity under-projects an ease-out slide: it is first seen near
rest, much slower than it came in. So on a frame before its track was first
seen (`TrackMotion::first_seen`), a box whose track is translating (a
measurement has moved both edges at least a pixel the way the velocity points,
so it slid or scrolled rather than grew while typed or faded in) is also
**trailed** back to the frame edge it moves away from, along the axis it moves
most on (`mask::trail_to_edge`), with the margin. Only frames after the last
frame a complete cycle scanned before that first sighting get the trail: that
cycle would most likely have found the box if it was already there. SPG-12c
measured why: on the full text suite two toasts were first scanned near rest,
at about -710 and -160 px/s, and the static and moved rects missed them on
5 and 7 frames (on its first frame one was 418 px right of where the cycles
found it, about 300 px beyond its back-projected rect).
Per frame, a rect contained in another is dropped, and past `max_retro_rects`
they merge into one hull (more blur, never less).

## Resize and content switches

A push with a new source size rescales the tracker (`Tracker::rescale`, masks
only grow) and starts a new segment. A cycle that was in flight across the
resize returns boxes in the old size. Each is held as the hull of the box and
of its grown mask scaled to every size since, so frames of each size keep the
scaled margin, as `Tracker::rescale` does for boxes held before the resize.
A retroactive rect on a frame of another size is the hull of the box and its
scaling grown in that size, and of the grown mask scaled.
The cycle does not count as a scan of new-size frames (rule 2). Up to 64
resizes are remembered. A complete result from before that cannot be scaled,
so it fails closed like any result that cannot be applied
(`FallbackReason::ApplyFailed` for frames from `ts - retro` on and pushed up to
`ts + hold`). Until SPG-10b it was dropped like an incomplete one, so frames of
later sizes that another cycle covered aired without its held boxes.

## Fallback modes

| `PolicyConfig::fallback` | `ReleasedFrame::action` |
| --- | --- |
| `blur_source` (default) | `BlurSource` |
| `solid_fill` | `SolidFill` |
| `hold_last_scanned` | `HoldLastScanned` with `reemit` = the last frame released clean in the same segment, if it still passes the gate; `BlurSource` when there is none (start, after a resize or a discontinuity) or when a cycle that finished after its release could have masked it. The plugin shows that frame again exactly as it released it. |

## Degenerate configs

Validation does not yet impose floors (#28), so the scheduler stays safe on
its own: `lookahead = 0` releases every frame as it is pushed, so nothing is
scanned in time and every frame fails closed (unless the caller scans
synchronously before releasing); `retro = 0` counts only a cycle on the frame
itself; `hold = 0` keeps a detection only on its own frame, though coverage
and retroactive rects still apply. Negative durations and zero capacities are
rejected.

## Threads

`Scheduler` starts `workers` threads. The render-facing calls (`wants_scan`,
`push`, `release_due`, `mark_discontinuity`, `stats`) take the mutex only for
bookkeeping; workers hold it only to take a job and to apply a result, never
while a stage runs, so the render thread never waits on inference. `push`
copies the readback into an `OwnedFrame` outside the lock, and only when the
cadence wants a scan. That copy allocates and copies one readback on the
render thread per queued scan (every frame at the default `scan_interval` of
0), and `release_due` waits for the mutex while a worker applies a result,
whose cost grows with detections times buffered frames. Neither waits on
inference, but SPG-16 must measure the p99 of the render-facing calls. The
plugin (SPG-14) avoids that allocation: it copies the mapped stage surface
into a pooled buffer, wraps it with `OwnedFrame::adopt` (no copy; the
buffer returns to the pool when the worker drops the frame) and calls the
`push` overload that takes a `shared_ptr<const OwnedFrame>`
([plugin-render.md](plugin-render.md)). The queue holds at most `queue_capacity` jobs; a new job
pushes out the oldest, and workers take the newest. The clock is read from
worker threads (the tracker prunes on update), so it must be thread-safe
(`SteadyClock` is; `ManualClock` is not).

Shutdown waits `shutdown_timeout` for workers, then detaches any still inside
a stage call. A detached worker owns the stages and its job through
`shared_ptr`s and exits without touching the scheduler or the clock once the
call returns. The plugin must not unload the module that holds the stage code
while one is still running; SPG-14 decides how (for example, waiting on unload
rather than on filter destroy).

## Using it

```cpp
spg::Scheduler scheduler(clock, spg::SchedulerConfig::from_policy(policy),
                         spg::ScanStages{faces, text, recognizer,
                                         std::make_shared<spg::EnginePolicy>(engine)});
// render callback
if (scheduler.wants_scan(t)) { /* read back a downscaled copy */ }
scheduler.push(t, source_size, readback_or_null);
scheduler.release_due(released);  // compose each: rects, or the fallback action
```

`DetectorAdapter<spg::detect::FaceDetector>` (and `TextDetector`) adapts the
SPG-05/06 detectors without making `spg-core` depend on them; it and
`EnginePolicy` reject a null pointer with `ContractError`. A detector may be
left null only when `scan.categories` disables its categories; otherwise
every cycle fails and every frame gets the fallback. Without a recognizer or
policy every text region is masked. Recognized text longer than
`scan.max_text_bytes` (512) is not passed to the policy; its region is masked.
Offline, `spg-cli` (SPG-11) drives `Lookahead` directly: push, take a job,
`run_scan()`, `complete()`, `release_due()`, in simulated time
([cli.md](cli.md)); `spg-cli bench --realtime` drives `Scheduler`.

## Defaults

| Setting | Default | Source |
| --- | --- | --- |
| `lookahead`, `retro`, `hold`, margin, fallback, mask style, `scan.categories` | from `PolicyConfig` | plan section 4 |
| `workers` | 2 | SPG-10 starting point; SPG-16 measured 1 against 2 (one worker misses far fewer frames, but the tracker then loses the velocities it gets from the two workers' paired cycles; agent-log SPG-16) |
| `queue_capacity` | 1 (the newest frame) | plan section 4 ("workers process the newest readback frame") |
| `scan_interval` | 0 (offer every frame) | SPG-10 starting point; SPG-16 tried 120 and 150 ms with two workers: staggering them cut fail-closed frames a little (single runs) but far less than one worker did, and it gives up the paired cycles the tracker's velocities come from |
| `max_buffered_frames` | 600 (5 s at 120 fps) | `kMaxLookahead` |
| `max_retro_rects` | 64 per frame | tracker's `max_rects_per_track` limit |
| `scan.max_text_bytes` | 512 | SPG-10 starting point |
| `clock_tolerance` | 1 s | SPG-10 starting point |
| `release_grace` | 500 ms | SPG-10 starting point; tracker `retain` is raised to `lookahead + release_grace` |
| `shutdown_timeout` | 2 s | SPG-10 starting point |

## Evidence

`tests/unit/lookahead_test.cpp` (deterministic cases),
`tests/unit/lookahead_property_test.cpp` (1500 seeded scenes; its oracle also
checks every re-emitted frame against every box that could reach it, and its
scenes include results the tracker rejects, a source that changes size
faster than the size history, and failed cycles that return partial boxes,
which the oracle checks for retro and hold like a complete cycle's but never
counts as coverage) and `tests/unit/scheduler_test.cpp` (real threads). See
the SPG-10, SPG-10b and SPG-10c entries in [agent-log.md](agent-log.md). None of this is synthetic-eval or live evidence;
cadence, worker count and the fail-closed rate on real detectors are for
`spg-cli bench` (SPG-11/16) to measure.
