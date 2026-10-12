# Tracker and temporal hold

`spg::Tracker` (`core/include/spg/tracker.hpp`, `core/src/track/tracker.cpp`,
SPG-09) turns detections into the Source-space rects to mask on each frame. It
implements the temporal-hold invariant from [AGENTS.md](../AGENTS.md): a blurred
region stays blurred for at least the hold after its last detection, with
margin, and tracking loss, a dropped frame or a source resize can extend a mask
but never shrink it early.

## Model

Every detection a track receives is **held**: its box, the frame time it was
seen on and the velocity to predict from. The mask for frame time `t` is, per
held detection with `seen <= t <= seen + hold`:

- the detection box moved at its velocity for `min(t - seen, max_prediction)`;
- joined with the unmoved box (the hull of both), so the mask covers where the
  object was and where it is predicted to be;
- grown on every edge by `margin_px + margin_ratio * min(width, height)` of
  the detection box (fixed when the detection is held; only a resize grows it);
- rounded outward to whole Source pixels.

Per track, these rects are merged into fixed time buckets by the frame time
each detection was seen on: bucket `floor(seen / w)` with
`w = ceil(hold / (max_rects_per_track - 1))`, and each bucket yields the hull of
its rects. Detections active at `t` were seen in `[t - hold, t]`, which spans at
most `max_rects_per_track` buckets, so `masks(t)` returns at most that many
rects per live track (default 8) in `O(held + max_rects_per_track^2)` work, even
for a 60 s hold. Bucket membership never changes, so a bucket's hull only grows
as detections arrive or `t` advances; merging adds coverage (part of a moving
object's trail between detections in one bucket) and never removes it. Rects
that another rect of the same track contains are left out; the union is
unchanged. `masks(t, out)` fills a caller's buffer instead of allocating. Both
are for the scheduler or a worker, not the OBS render path.

Because each held detection only grows until its own hold ends, and updates
only add held detections, these properties follow and are tested:

| Property | Statement |
| --- | --- |
| Hold | A detection with box `b` on frame time `td` is covered, grown by the margin, on every frame time in `[td, td + hold]`, whatever arrives later. |
| Monotone in frame time | Between `t` and `t'`, if no detection active at `t` reaches its hold expiry before `t'`, every rect of `masks(t)` lies inside a rect of `masks(t')`. |
| Updates only add | `update()` and `rescale()` never remove coverage from `masks(t)` for any frame time still in the lookahead buffer. |
| Resize keeps scaled coverage | After `rescale()`, every rect `masks(t)` returned before is covered both as it was and scaled to the new size. |
| Bound | `masks(t)` has at most `max_rects_per_track` rects per live track. |

`tests/unit/tracker_test.cpp` checks all of them over 2000 seeded random scenes
(1 to 5 objects of both kinds, constant and changing velocity, irregular
detection cycles with jitter, missed cycles, late and out-of-order deliveries,
frames never queried, one resize in about 30% of scenes, random configs
including a zero hold and rect caps from 1 to 12). Monotone growth is checked
during each scene, from a frame time inside a still-retained detection's hold,
so every comparison is between non-empty masks. Unit cases cover association,
prediction, expiry, pruning, validation, resize (including a 2x upscale and two
chained resizes) and a 60 s hold with 600 held detections.

## Association and motion

- A detection joins a live track (one whose hold has not ended at the frame
  time) of the same kind when its IoU with the track's predicted box, or with
  its last confirmed box, is at least `iou_threshold`. Pairs are assigned
  greedily, best IoU first; an unmatched detection starts a new track.
- Velocity is the motion of the box center between confirmations: the first
  measurement is taken as is, later ones are smoothed with weight
  `velocity_smoothing`. Box size follows the latest detection.
- A detection for a frame at or before the track's last confirmation (a late
  or second result for the same frame) is held but does not move the track.
- An object that moves more than roughly half its size between detection
  cycles fails the IoU gate and starts a new track. Its detections are still
  held; only prediction is lost until the new track has a velocity.

## Time and the clock

Hold and prediction run on frame time (the `Timestamp` passed to `update()` and
`masks()`). The injected `Clock` is read only by `prune()` (which `update()`
calls): a held detection is dropped once `seen + hold + retain < clock.now()`.
`retain` defaults to the lookahead, because a frame can wait that long in the
buffer before its mask is asked for. `masks()` is pure and never reads the
clock. The tracker is not thread-safe; its single owner (the scheduler, SPG-10)
serializes calls.

Because `prune()` compares `clock.now()` with frame timestamps, the injected
clock must report the same `FrameClock` time base (epoch and units) as the
frames. A clock running ahead of frame time would drop held detections before
their buffered frames are masked, which fails open; one running behind would
never drop them. SPG-10 must pass frame timestamps from the clock it injects,
or assert that the two agree.

`masks(t)` covers detections seen at or before `t`. Applying a detection to
buffered frames before it (retroactive masking within `retro_ms`) is the
scheduler's job in SPG-10.

## Source resize

`rescale(from, to)` replaces every held box with the hull of its old box and its
scaled box (rounded outward), widens its velocity range to cover both the old
and the scaled velocity (scaled values rounded outward), and grows its margin
per axis to the largest of: the old margin, the scaled margin plus one scaled
pixel, and the margin of the merged box. The scaled old mask is the scaled box
grown by the scaled margin plus less than one scaled pixel of outward rounding,
so the mask after a resize contains the mask before it, the scaled mask before
it, and the scaled box with its own margin. A fresh margin alone would not do:
`margin_px` does not scale, so on a 2x upscale a 40 px box's mask
`[-14, 54]` (scaled `[-28, 108]`) would shrink to `[-24, 104]`. Tracks continue
in the new coordinates. Detections handed to `update()` after `rescale()` must
be in the new coordinates.

## Defaults

| Setting | Default | Source |
| --- | --- | --- |
| `hold` | 750 ms | `PolicyConfig::hold` (plan section 4) |
| `margin_px`, `margin_ratio` | 4 px, 0.25 | `PolicyConfig` placeholder from SPG-02 |
| `retain` | 500 ms | `PolicyConfig::lookahead` |
| `iou_threshold` | 0.3 | SPG-09 starting point |
| `velocity_smoothing` | 0.5 | SPG-09 starting point |
| `max_prediction` | 500 ms | SPG-09 starting point |
| `max_rects_per_track` | 8 | SPG-09 starting point; SPG-11 sets it from the blur effect's rect capacity |

`TrackerConfig::from_policy()` takes hold, margin and lookahead from the policy.
The margin and the tracker numbers have no eval evidence yet; SPG-12 tunes
them against the synthetic suites. Velocity is not bounded: two confirmations
very close in frame time with a few pixels of jitter give a large velocity and
a long predicted hull. That only over-blurs (it never removes coverage), so it
is left for SPG-12 (#13) to bound against the clean-frame blur budget rather
than trading prediction recall away without eval evidence. A floor for zero
hold or margin is #28 (SPG-08b); `validate()` accepts zero today.
`max_rects_per_track` is validated to [1, 64], which keeps the per-track
containment pass at most 4096 comparisons.
