# Tracker and temporal hold

`spg::Tracker` (`core/include/spg/tracker.hpp`, `core/src/track/tracker.cpp`,
SPG-09) turns detections into the Source-space rects to mask on each frame. It
implements the temporal-hold invariant from [AGENTS.md](../AGENTS.md): a blurred
region stays blurred for at least the hold after its last detection, with
margin, and tracking loss, a dropped frame or a source resize can extend a mask
but never shrink it early. The one exception (SPG-12b, [Follow](#follow-spg-12b)):
while a track confirmed as moving is still being updated, its hold follows the
track, and after its last detection every box is held where it was seen.

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
only add held detections, these properties follow and are tested. With
`follow_moving_tracks` off (before SPG-12b) they hold on every frame time; with
it on (the default), the first four hold except where Follow moves a box, on
frame times up to a followed track's last detection:

| Property | Statement |
| --- | --- |
| Hold | A detection with box `b` on frame time `td` is covered, grown by the margin, on every frame time in `[td, td + hold]`, whatever arrives later. |
| Monotone in frame time | Between `t` and `t'`, if no detection active at `t` reaches its hold expiry before `t'`, every rect of `masks(t)` lies inside a rect of `masks(t')`. |
| Updates only add | `update()` and `rescale()` never remove coverage from `masks(t)` for any frame time still in the lookahead buffer. |
| Resize keeps scaled coverage | After `rescale()`, every rect `masks(t)` returned before is covered both as it was and scaled to the new size. |
| Follow | A box Follow releases from where it was seen on frame time `t` is a detection that moved its track forward, seen before the track's latest such sighting `a` at or before `t`, with another `b` at or after `t`. Then the hull of `a` and `b` grown by their margins, and the box grown by its margin and moved by the measured move to `a` and to `b`, are covered. |
| Bound | `masks(t)` has at most `max_rects_per_track` rects per live track. |

`tests/unit/tracker_test.cpp` checks all of them over 2000 seeded random scenes
(1 to 5 objects of both kinds, constant and changing velocity, irregular
detection cycles with jitter, missed cycles, late and out-of-order deliveries,
frames never queried, one resize in about 30% of scenes, random configs
including a zero hold and rect caps from 1 to 12), once with follow off and
once with it on. Monotone growth is checked during each scene, from a frame
time inside a still-retained detection's hold, so every comparison is between
non-empty masks (with follow on, only after every detection fed: 39,123
comparisons). The follow run checks 57,594 released boxes against the Follow
row, and every other box against Hold. Unit cases cover association,
prediction, expiry, pruning, validation, resize (including a 2x upscale and two
chained resizes), a 60 s hold with 600 held detections (followed and not), and
for Follow: a scrolling line covered where it is on every frame, released at
the release time the scheduler uses, a stopped line holding every box where it
was seen, and a velocity estimate pointing the wrong way or lagging an
ease-out never uncovering the line.

## Follow (SPG-12b)

On a scrolling chat the held boxes of a secret line stay where it was for the
whole hold, while the line moves on and the next lines scroll into those
positions: SPG-12d attributed most covered decoys on the text suite to such
stale and swept positions (CI 226 of 376 at the defaults). Follow masks the
held boxes of a confirmed moving track where the track went instead.

- **Confirmed moving.** A track's run is measured from a checkpoint box, over
  at least 100 ms of frame time (two workers' paired cycles land a frame
  apart, where a pixel of jitter is as large as the move). A move of both
  edges at least a pixel one way is a translation; it is steady when it is
  within half, or 3 px, of what the run's last speed predicts. Two steady
  translations the same way confirm the track. A move against the run, a
  turn, a change of speed, no translation for 250 ms (a stop), a motion-gate
  pairing or a resize ends the run. The speed check is what keeps another
  line out: after missed cycles the line below can take a scrolling line's
  track by IoU with its last box, and it measures far less move than the run
  predicts.
- **Moved boxes.** On a frame time `t` up to the track's last detection, let
  `a` and `b` be its latest sighting at or before `t` and its earliest at or
  after it, counting only detections that moved the track forward (their
  frame times only increase). When `a` is in the current run and `t` is
  within `a`'s hold, every detection of the run seen before `a` is masked as
  its own box moved by the measured move of its center to `a` and to `b`
  (the hull of both), grown by its own margin, and the run's bridges older
  than `a` are left out. The hull of `a` and `b`, grown by the larger of
  their margins, is added. Text that moved one way between two sightings is
  inside that hull on every frame time between them, whatever its speed
  profile, so no frame depends on the velocity estimate.
- **Held where seen.** Late detections, boxes from before the run, boxes held
  before a gate pairing or resize (they may be another object's), and every
  box on frame times after the track's last detection.

What Follow releases is where the text has been: the hull of `a` and `b`
covers where it is, and each moved box keeps its own size and margin there.
A gate pairing at the start of a scroll (a first
move that IoU misses, as on 720p chat lines 16 to 19 px tall) keeps the boxes
before it in place for their hold, like a new track, and the SPG-16b trail
for its frames; that is the cost of not following a pairing that may be wrong.
`TrackMotion::bridged_from` tells the scheduler from which frame time a
confirmed moving track's own masks cover its path, so it can leave out
retroactive rects there ([scheduler.md](scheduler.md#masks)).

At the 60 s hold bound (600 detections), on an Apple M4 that was also running
another build, `masks()` took about 11 us per call for a followed moving box
(19 us held where seen) and 9 to 11 us at rest either way. The SPG-16b
figures under [Association and motion](#association-and-motion) were measured
unloaded, so compare only within one run.

## Association and motion

- A detection joins a live track (one whose hold has not ended at the frame
  time) of the same kind when its IoU with the track's predicted box, or with
  its last confirmed box, is at least `iou_threshold`. Pairs are assigned
  greedily, best IoU first.
- The tracks and detections IoU left unpaired then go through the motion gate
  (SPG-16b). With one detection cycle every ~200 ms, a small fast box moves
  more than its own size between cycles (a 43 px face at 480 px/s moves
  96 px), so IoU alone started a new track each cycle and the track never
  had a velocity. A detection may join such a track when its center is
  within `motion_gate_per_s * min(gap, max_prediction)` box sizes of the
  track's last center or of the center its velocity predicts at the frame
  time (forward, or back for an older result), where `gap` is the frame
  time between them and a box size is, per axis, the larger extent of the
  two boxes. Its width and height must each be within `motion_size_ratio`
  of the last box. Pairs are assigned greedily, closest first; ties go by
  index. Because this pass runs after IoU, every pair IoU makes is kept. An
  unmatched detection starts a new track.
- A gate pairing can be wrong: a new object can come within reach of an old
  track, for example a face sliding in from the edge near where another face
  was. Every detection is still held from its own frame for the whole hold,
  so no hold ends early, but the motion matters too. Inherited from the old
  track, the new object's `first_seen` would be too early and its velocity
  would point the old object's way, so the scheduler would skip the trail to
  the frame edge that the new object's own track would get, and the frames it
  slid in on would air unblurred (Greptile on PR #88). So a gate pairing also
  starts a second motion estimate on the track, `since_gate`, measured from
  the gated detection on as the new track it would otherwise have started:
  - `TrackMotion` reports that estimate once it has a velocity, and the whole
    track's until then. The whole track's motion only adds to what a new
    track reports before its first measurement (the static box).
  - Each held detection predicts over both velocities (the hull of the box
    moved at either).
  - A later detection also bridges from that estimate's last box when it is
    not the track's (after a late gate pairing).

  This keeps the coverage of the new object's own track while later
  detections pair with the track as they would have with that one, which is
  the case when its box is where the new object is. It does not rebuild
  every pairing the tracker would have made without the gate: two gate
  pairings in a row with results out of order can still differ.
- When a detection moves its track forward in time, the hull of the track's
  last box and the new box, with the larger of their margins, is also held
  from the last confirmation. An object that moves in one direction between
  two sightings stays inside that hull, whatever its speed profile. Velocity
  alone only covers the path constant motion would take: an ease-out slide
  is further along than the measurement says, and a track seen once has no
  velocity yet. The bridge is left out when the new box lies inside the last
  one: the last box's own hold already covers that hull with a margin at
  least as large, so a box at rest holds one entry per detection. A moving
  box holds two. At the bound above (60 s hold, a detection every 100 ms),
  `masks()` took about 11 us per call for a moving box (1199 entries) and
  5 us for one at rest (600; 11 us before the bridge was left out), and the
  slowest `update()` under 7 us (Apple M4, Release build).
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
- An object that moves further than the motion gate reaches between
  detection cycles (20 box sizes a second by default), or changes size by
  more than `motion_size_ratio`, starts a new track. Its detections are
  still held; only prediction is lost until the new track has a velocity.

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
scheduler's job in SPG-10. `update()` can report, per detection, the motion
of the track it joined (`TrackMotion`): its velocity (zero for a new track or
one with no motion measurement yet), when the track was first seen, and
whether a measurement has translated the box (both edges moved at least a
pixel the way the velocity points, so a box that grows while typed or fading
in does not count; once set, it stays set for the track), the track's id, and
`bridged_from` (see [Follow](#follow-spg-12b)). Since SPG-12 the scheduler
back-projects retroactive rects with the velocity, since SPG-12c it trails a
translating box back to the frame edge on frames before the first sighting,
since SPG-16d it spreads a first sighting (no velocity yet) by the motion
gate's reach on earlier frames that air before a second sighting can land,
and since SPG-12b it leaves them out from `bridged_from` on
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
| `margin_px`, `margin_ratio` | 4 px, 0.25 | `PolicyConfig` placeholder from SPG-02; kept by the SPG-12d sweep (#81, [agent log](agent-log.md)) |
| `retain` | 500 ms | `PolicyConfig::lookahead` |
| `iou_threshold` | 0.3 | SPG-09 starting point |
| `motion_gate_per_s` | 20 box sizes/s | SPG-16b; 0 turns the gate off. The faces suite's fast face (35 x 48 px at about 510 px/s) moves 14.3 widths a second; 16 to 30 gave the same recall, and decoy blur rose with the gate |
| `motion_size_ratio` | 2 | SPG-16b; a toast first seen clipped at the frame edge is often about half as wide as at rest |
| `velocity_smoothing` | 0.5 | SPG-09 starting point |
| `max_prediction` | 500 ms | SPG-09 starting point |
| `max_rects_per_track` | 8 | SPG-09 starting point; capped at the compositor's rect capacity (`kMaskRectCapacity`, 64 per frame, [cli.md](cli.md)), so eight tracks fit a frame unmerged |
| `follow_moving_tracks` | true | SPG-12b; false holds every box where it was seen and reports no bridged frames. The run's 100 ms baseline, 3 px steadiness tolerance and 250 ms stop window are constants in `tracker.cpp` |

`TrackerConfig::from_policy()` takes hold, margin and lookahead from the policy.
SPG-12d (#81) swept the margin between these defaults and the SPG-08b floors
(2 px, 0.125) on the synthetic suites: no setting meets the text decoy limit,
because most decoy blur comes from stale and swept positions of scrolling
lines, not from the margin, and the smaller margins raise the faces leak rate.
The defaults stay. The tracker numbers have no
eval evidence yet; SPG-12 tunes them against the synthetic suites. Velocity is not bounded: two confirmations
very close in frame time with a few pixels of jitter give a large velocity and
a long predicted hull. That only over-blurs (it never removes coverage), so it
is left for SPG-12 (#13) to bound against the clean-frame blur budget rather
than trading prediction recall away without eval evidence. A floor for zero
hold or margin is #28 (SPG-08b); `validate()` accepts zero today.
`max_rects_per_track` is validated to [1, 64], which keeps the per-track
containment pass at most 4096 comparisons.
