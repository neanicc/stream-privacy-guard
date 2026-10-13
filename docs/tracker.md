# Tracker and temporal hold

`spg::Tracker` (`core/include/spg/tracker.hpp`, `core/src/track/tracker.cpp`,
SPG-09) turns detections into the Source-space rects to mask on each frame. It
implements the temporal-hold invariant from [AGENTS.md](../AGENTS.md): a blurred
region stays blurred for at least the hold after its last detection, with
margin, and tracking loss, a dropped frame or a source resize can extend a mask
but never shrink it early. Since SPG-12b (#75) the hold of a **confirmed moving
track** follows the track while the track is still being updated, instead of
keeping a box where the text was ([Following](#following)); the owner chose
this on #75.

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

## Following

A text line that scrolls keeps moving after each detection. Held where it was
found, each detection would keep the rows the line left blurred for the whole
hold, and the lines that scroll into those rows (decoys, filler) with them.
SPG-12b measured these stale rects as about 30% of the decoys blurred on the
local text suite and 27% in CI ([agent-log.md](agent-log.md)). Following
lets those held boxes go while the track's newer detections pin where the
text is.

- **Measurement.** Confirmations taken in frame order (not a late or
  same-frame result) are measured against the last measured box (the
  track's first box to begin with). One whose box center is less than
  `follow_min_move` (default 4 px, along either axis) from it is not
  measured: two workers can scan frames 17 ms apart, where a chat line moves
  under 2 px and the detector's box jitters about as much. Otherwise the move
  from that box is a measurement, and it becomes the last measured box. A
  measurement is **consistent** when (1) both edges of the box moved at least
  1 px along the axis the measured velocity is larger on (x on a tie), in its
  direction (it translated, not grew as typed or fading text does); and (2)
  unless it starts a run, it lies on the same axis and in the same direction
  as the measurement before it, at a speed along that axis within
  `follow_speed_ratio` (default 2) of that one's. A translation that fails
  (2) starts a new run from the box it was measured from; one that fails (1)
  ends the run. A confirmation whose width or height differs by more than a
  quarter from the last measured box also ends the run (another line that
  takes over the track usually has another size), and so does a confirmation
  that is not measured more than `hold` after the last measurement (the
  track stopped); the next run is then measured from that confirmation, so
  nothing held while the track stood still is followed.
- **Missed cycle.** An `update()` that does not find a track (a scan cycle
  that missed it, in or out of frame order, or matched its text to another
  track) ends the track's run, and no detection the track held at that point
  is ever followed. A later confirmation can confirm a new run, but it only
  follows detections taken after the miss, so it never follows boxes across
  a gap in which a scan did not see the text.
- **Confirmed moving.** The track's last `follow_after` (default 2, so three
  detections) measurements form one run.
- **Followed.** A held detection of a confirmed moving track is followed on
  frame time `t` when it was taken in frame order at or after the
  confirmation the run started from, no cycle has missed the track since it
  was taken, a later detection of the same run was
  seen in `(seen, t]`, and the track was confirmed at or after `t` (it is
  still being updated: a later scan found it again). A followed detection adds
  no rect on `t`; the track's coverage there is the mask of the newest
  detection of the run seen at or before `t`, which holds as usual (its box,
  its prediction hull and margin).
- **Never followed:** a late detection (its place in the run was not
  measured), a detection taken before a `rescale()` (a resize also ends the
  run) or before a missed cycle, any detection of a track that is not
  confirmed moving, and anything on a frame time after the track's last
  confirmation. So tracking loss, a missed cycle or a resize keeps every box
  held where it was, for its whole hold, even after the track is found
  again.
- `follow_after = 0` turns following off.

Why it does not uncover the text: on frame `t` the newest detection at or
before `t` found the text at its box, and the run says the text kept moving
the same way since the run began, so it does not return to the rows older
detections saw. That newest detection's mask (unmoved box, hull to its
prediction) and the retroactive rects of the later confirmation (static and
moved back, [scheduler.md](scheduler.md#masks)) still cover `t`, as before.
Following does not touch retroactive rects or the SPG-12c trail.

## Properties

Each held detection only grows until its own hold ends, and updates only add
held detections; following removes a held detection's rect only on the frames
where it is followed. These properties follow and are tested:

| Property | Statement |
| --- | --- |
| Hold | A detection with box `b` on frame time `td` is covered, grown by the margin, on every frame time in `[td, td + hold]` on which it is not followed. With following off: whatever arrives later. |
| Following | A detection may be followed on `t` only when it was taken in frame order after any resize, no `update()` since missed its track, a later in-order detection of its track was seen in `(td, t]`, the track was confirmed at or after `t`, and the track has had at least `follow_after` in-order confirmations since its last resize and miss. The newest detection of a track at or before `t` always holds. |
| Monotone in frame time (following off) | Between `t` and `t'`, if no detection active at `t` reaches its hold expiry before `t'`, every rect of `masks(t)` lies inside a rect of `masks(t')`. |
| Updates only add | `update()` and `rescale()` never remove coverage from `masks(t)` for any frame time still in the lookahead buffer, except the rects of detections an update made followed on `t`. `rescale()` never does. |
| Resize keeps scaled coverage | After `rescale()`, every rect `masks(t)` returned before is covered both as it was and scaled to the new size. |
| Bound | `masks(t)` has at most `max_rects_per_track` rects per live track. |

`tests/unit/tracker_test.cpp` checks the strict properties with following off
over 2000 seeded random scenes
(1 to 5 objects of both kinds, constant and changing velocity, irregular
detection cycles with jitter, missed cycles, late and out-of-order deliveries,
frames never queried, one resize in about 30% of scenes, random configs
including a zero hold and rect caps from 1 to 12). Monotone growth is checked
during each scene, from a frame time inside a still-retained detection's hold,
so every comparison is between non-empty masks. Unit cases cover association,
prediction, expiry, pruning, validation, resize (including a 2x upscale and two
chained resizes) and a 60 s hold with 600 held detections. The same 2000 scenes
run again with following on (random `follow_after` 1 to 3, speed ratio 1 to 3
and minimum move 1 to 8 px): every detection that may not be followed by the
rule in the table stays covered, and the scenes do follow some. That rule is
derived in the test from what was delivered (order, resize, misses and the
count of confirmations), not from the tracker's run state; whether a box
belongs to the current run needs the measurement rule itself, so unit cases
pin it. Unit cases cover the rule: one
measurement does not confirm, a reversal, an axis change, a more than twofold
speed change, a size change over a quarter, growth in place, a sub-pixel shift
and a drift under the minimum move do not either, two workers' confirmations
17 ms apart do not break a run, a restarted run follows only from where it
restarted, a missed cycle keeps the full hold of every earlier box after the
track is found again, a track that stood still for longer than the hold
starts a new run, a static line keeps its hold while a same-size line
scrolls through it (even when it is missed while they overlap), and late
detections, boxes from before a resize and frames after the last
confirmation keep their hold.

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
  When it is older than every detection the track has taken (cycles that
  finished out of order), its move to the latest box still counts as motion.
  The track takes that velocity when it has none, or when the move
  translates the box but the velocity it has (measured near rest by the
  results that landed first) does not point that way. A move of both edges
  the way the velocity points marks the track translating. The box stays as
  it is.
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

`masks(t)` covers detections seen at or before `t`. Following reads only the
track's state: the scheduler asks for a frame's masks when it releases it,
`lookahead` after the frame, so cycles on later frames have usually confirmed
the track by then. Applying a detection to
buffered frames before it (retroactive masking within `retro_ms`) is the
scheduler's job in SPG-10. `update()` can report, per detection, the motion
of the track it joined (`TrackMotion`): its velocity (zero for a new track or
one with no motion measurement yet), when the track was first seen, and
whether a measurement has translated the box (both edges moved at least a
pixel the way the velocity points, so a box that grows while typed or fading
in does not count; once set, it stays set for the track). Since SPG-12 the scheduler back-projects retroactive rects
with the velocity, and since SPG-12c it trails a translating box back to the
frame edge on frames before the first sighting
([scheduler.md](scheduler.md#masks)).

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
| `follow_after` | 2 | SPG-12b (#75): three detections, two consistent measurements |
| `follow_speed_ratio` | 2 | SPG-12b starting point: a steady scroll stays well inside it, a twofold change is not the same motion |
| `follow_min_move` | 4 px | SPG-12b: above the detector's box jitter on the text suite (1 to 2 px between frames 17 ms apart) |
| `max_rects_per_track` | 8 | SPG-09 starting point; capped at the compositor's rect capacity (`kMaskRectCapacity`, 64 per frame, [cli.md](cli.md)), so eight tracks fit a frame unmerged |

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
