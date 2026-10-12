# Scheduler, lookahead and fail-closed release

SPG-10 ([plan section 4](plan.md#4-timing-model)). This part decides, for every
frame, whether it may air with its masks or must get the fallback. It holds the
fail-closed, retroactive-masking and temporal-hold invariants from
[AGENTS.md](../AGENTS.md). Code: `core/include/spg/{scan,lookahead,scheduler}.hpp`,
`core/src/schedule/`, `core/src/mask/mask_rects.*`.

| Piece | What it is |
| --- | --- |
| `run_scan()` | One detection cycle over one readback frame: faces, text regions, recognition, policy. Never throws; any exception, invalid output or missing detector for an enabled category makes the cycle incomplete. |
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
   allocation or tracker failure in `complete()`) fails closed
   (`FallbackReason::ApplyFailed`), as do the other frames that result could
   have masked: buffered frames from `ts - retro` on, and frames pushed up to
   `ts + hold`.
2. That cycle scanned the **same content**: no resize and no
   `mark_discontinuity()` between the two frames.
3. The release is no later than `t + lookahead + release_grace`. A later
   release (the caller stopped asking) may have lost held detections to the
   tracker's pruning, so it fails closed.
4. The frame's timestamp increases and is within `clock_tolerance` of
   `clock.now()` when pushed. Otherwise the clock and the frames do not share
   one `FrameClock` time base (the tracker's pruning compares them), so the
   frame is never scanned and fails closed.

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

## Masks

`MaskPlan.rects` for a frame at `t` is the union of:

- `Tracker::masks(t)`: every detection seen in `[t - hold, t]`, with margin and
  constant-velocity prediction ([tracker.md](tracker.md));
- **retroactive rects**: each box of a complete cycle at `ts` in `(t, t + retro]`
  that finished while the frame was buffered, grown by the same margin and
  rounded outward to whole pixels. The tracker only covers frames at or after
  a detection, so this is what blurs a secret from its first frame.

A fallback frame's plan still lists these rects (`plan.fallback` is set).
Retroactive rects are static boxes: a secret that moved between `t` and `ts`
is covered where it was found, plus the margin. Back-projecting with the track
velocity is left for SPG-12 to measure on the scroll and slide-in sequences.
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
resizes are remembered; a result from before that is treated as incomplete.

## Fallback modes

| `PolicyConfig::fallback` | `ReleasedFrame::action` |
| --- | --- |
| `blur_source` (default) | `BlurSource` |
| `solid_fill` | `SolidFill` |
| `hold_last_scanned` | `HoldLastScanned` with `reemit` = the last frame released clean in the same segment; `BlurSource` when there is none (start, after a resize or a discontinuity). The plugin shows that frame again exactly as it released it. |

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
inference, but SPG-16 must measure the p99 of the render-facing calls; a
pooled readback buffer would remove the allocation. The queue holds at most `queue_capacity` jobs; a new job
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
Offline, `spg-cli` (SPG-11) can drive `Lookahead` directly: push, take a job,
`run_scan()`, `complete()`, `release_due()`.

## Defaults

| Setting | Default | Source |
| --- | --- | --- |
| `lookahead`, `retro`, `hold`, margin, fallback, mask style, `scan.categories` | from `PolicyConfig` | plan section 4 |
| `workers` | 2 | SPG-10 starting point |
| `queue_capacity` | 1 (the newest frame) | plan section 4 ("workers process the newest readback frame") |
| `scan_interval` | 0 (offer every frame) | SPG-10 starting point; SPG-16 tunes cadence against `bench` |
| `max_buffered_frames` | 600 (5 s at 120 fps) | `kMaxLookahead` |
| `max_retro_rects` | 64 per frame | tracker's `max_rects_per_track` limit |
| `scan.max_text_bytes` | 512 | SPG-10 starting point |
| `clock_tolerance` | 1 s | SPG-10 starting point |
| `release_grace` | 500 ms | SPG-10 starting point; tracker `retain` is raised to `lookahead + release_grace` |
| `shutdown_timeout` | 2 s | SPG-10 starting point |

## Evidence

`tests/unit/lookahead_test.cpp` (deterministic cases),
`tests/unit/lookahead_property_test.cpp` (1500 seeded scenes) and
`tests/unit/scheduler_test.cpp` (real threads). See the SPG-10 entry in
[agent-log.md](agent-log.md). None of this is synthetic-eval or live evidence;
cadence, worker count and the fail-closed rate on real detectors are for
`spg-cli bench` (SPG-11/16) to measure.
