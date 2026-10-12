# Evaluation

The eval answers one question: how often would a planted secret air unblurred?
It runs the real pipeline on generated frames with exact ground truth, then
scores the mask plans. Targets live in [`eval/thresholds.toml`](../eval/thresholds.toml),
which only Ali edits.

## Suites

| Suite | Content | Where it runs |
| --- | --- | --- |
| `text` | Generated UI-like frames: chat panels, notifications, browser address bars, desktop windows, HUD-style panels. Watch-list strings, PII (emails, phones, Luhn-valid cards, IPs, street addresses, token-shaped strings), and decoys (near-miss strings that must not match). Motion: static, scroll, slide-in, fade, partial reveal. 1080p and 720p. Includes clean frames with no sensitive content. Seeded and deterministic. | CI (reduced size) and locally |
| `faces` | Face crops from a public benchmark composited onto generated backgrounds at varied scale and position. The dataset is fetched to `$SPG_CACHE_DIR` with its license recorded and is never committed or uploaded as a CI artifact. | Locally |
| `realtime` | `spg-cli bench --realtime` replays the text suite's sequences at the `[suite.realtime]` resolution as a feed at its fps against the wall clock with the default config, so slow detection shows up as fail-closed frames. Scored on that suite's ground truth with the cycles' latencies. | Locally on the dev Mac |

## Metrics

A ground-truth instance on a frame is **covered** when at least `coverage_tau`
of its box area lies inside the union of that frame's mask rects (or the frame is
a fail-closed frame).

| Metric | Definition |
| --- | --- |
| `*_pair_recall` | Covered (frame, instance) pairs ÷ all visible (frame, instance) pairs of that kind |
| `watchlist_first_frame_recall` | Watch-list instances covered on the first frame they become visible ÷ all watch-list instances. Tests retroactive masking |
| `leak_frame_rate` | Frames with any uncovered sensitive instance ÷ frames with any sensitive instance |
| `clean_frame_blur_area` | Mean masked area fraction on frames with no sensitive instance. Stops "blur everything" from passing |
| `decoy_blur_rate` | Decoy instances covered ÷ decoy instances. Informational ceiling only; recall matters more |
| `fail_closed_frame_rate` | Fail-closed frames ÷ all frames (realtime suite) |
| `p95_detection_latency_ms` | 95th percentile time from a frame's timestamp to the completion of the detection cycle that scanned it (realtime suite) |

The scorer prints one line per metric with its threshold, then exactly one of
`THRESHOLDS: PASS` or `THRESHOLDS: FAIL`. `PASS` requires every suite marked
required for the current mode to have run. A missing suite is a failure, not a
skip, in `--local` mode.

## Scoring rules

These settle what the metric definitions leave open. The scorer is
`eval/spg_eval/scoring.py`.

- Mask plans pair with ground truth by `timestamp_ns`. A frame without a plan,
  a plan without a frame, or two plans for one timestamp make the input
  unscorable, which prints `THRESHOLDS: FAIL`. A missing frame is never
  scored as blurred or as clean.
- Each sequence's ground truth must hold exactly frames 0..n-1 for the frame
  count `suite.json` declares; otherwise the suite is unscorable. Pairing alone
  would accept a ground truth and a plan file shortened alike.
- An instance is visible on a frame when its visibility is above 0 and its
  box, clipped to the frame, has area. Coverage is measured on that clipped box
  against the union of the plan's rects clipped to the frame.
- Pair recalls count visible (frame, instance) pairs: `watchlist`, every
  `pii:*` kind together, and `face`. Per-kind PII recalls are reported in
  `report.json` without limits. `decoy_blur_rate` also counts visible pairs.
- A frame is clean when its ground truth lists no sensitive instance at all,
  visible or not. The generators list every planted item on every frame of its
  sequence (visibility 0 while it cannot show), so clean frames come only from
  clean sequences and a mask held over an item that just faded or scrolled away
  is not charged to the clean-frame budget.
- A limit whose metric has no data (no clean frames, no watch-list items) fails.
- `p95_detection_latency_ms` is the nearest-rank 95th percentile of per-cycle
  latencies that a realtime run supplies; `fail_closed_frame_rate` counts plans
  with `fallback: true`.
- A `fallback: true` plan covers every instance on its frame and counts as a
  fully blurred frame against the clean-frame budget. Only the realtime suite
  limits `fail_closed_frame_rate`, so in the text and faces suites the
  clean-frame budget is the only brake on a pipeline that falls back often.
  The realtime suite runs in local mode, where thresholds.toml requires it.
- Every key in a `[suite.*]` table other than `required_in`, `resolution` and
  `fps` must be `<metric>_min` or `<metric>_max` for a metric the scorer
  computes; anything else is an error, so a typo cannot silently drop a limit.
  The check runs when the thresholds load, for every suite, including suites
  the current mode skips.
- Output lines and `report.*` hold metric values and counts only, never the
  text of a planted item.

## Generated suites

`eval/generate.py` writes a suite as:

```
<suite>/suite.json                 index: suite, seed, profile, fps (60), sequences
<suite>/policy.json                schema-1 policy config with the suite's synthetic watch-list
<suite>/<seq>/frames/000000.png    RGB frames in Source pixels
<suite>/<seq>/ground_truth.jsonl   one ground-truth document per frame (docs/contracts.md)
<plans>/<seq>/mask_plans.jsonl     one mask plan per frame, written by the pipeline
```

- **Text.** Five templates, each with its own motion: a chat panel that
  scrolls, a notification toast that slides in and fades out, a browser address
  bar where a string is typed (partial reveal), an account window that fades in
  or stays static, and a game HUD over a panning scene that fades in and out.
  Sequences alternate 1080p and 720p and every fourth is clean (filler text and
  decoys only). Half of the planted secrets are watch-list entries. Profiles:
  `ci` is 10 sequences of 40 frames, `full` 30 of 90.
- **Faces.** Crops of the pinned dataset pasted as webcam-style overlays (face
  height 40 to 320 px at 1080p, one to three per frame, static or drifting, some
  fading in) on generated desktop and game backgrounds. Every fourth sequence has
  no faces. Profiles: `ci` 6 x 30 frames, `full` 24 x 60.
- **Boxes.** An item's box is its full box clipped to the panel it is drawn in
  and to the frame, since nothing outside those can show. Partial reveal does
  not shrink the box. Visibility is opacity x revealed fraction x the share of
  the box left after clipping.
- **Synthetic content.** Watch-list entries are invented names, a street
  address and a handle. PII uses reserved or fictional ranges (example.com
  emails, 555-01xx phones in NANP 3-3-4 or E.164 form, Luhn-valid cards on a
  test BIN, documentation IP ranges, token-shaped strings). Decoys are near
  misses a correct policy must not blur: watch-list look-alikes beyond the
  default fuzzy limits, Luhn-invalid cards, phones with an invalid area code,
  version strings with octets above 255, handles, short token prefixes, dates
  and times. `eval/spg_eval/synth.py` states the rules.
- **Text rendering** uses Pillow's bundled Aileron Regular font (CC0). Pillow
  is pinned per interpreter and platform in `eval/pydeps.json` and fetched into
  `$SPG_CACHE_DIR` with its SHA-256 checked; nothing is installed into Python.
- **Faces dataset.** LFW, pinned with its license under `datasets` in
  `models/manifest.json` and fetched on first use of the faces suite into
  `$SPG_CACHE_DIR/deps/dataset-lfw/`. LFW has no per-image face box; its images
  are face detections enlarged 2.2 times and centered, so the pinned
  `face_box` is the central 114 px square of the 250 px image. The generated
  frames show real people: they stay in the local output directory.

## Sanity checks the scorer must keep passing

- A pipeline that blurs nothing fails every recall threshold.
- A pipeline that blurs every frame entirely fails `clean_frame_blur_area_max`.
- A pipeline that masks exactly the visible sensitive boxes passes, which shows
  the limits can be met on the generated suites.
- Re-running the generator with the same seed produces byte-identical ground truth.

`tests/scripts/test_scorer.py` and `tests/scripts/test_generators.py` (ctest
`scoring.*`) check these, and `run_eval.py` repeats the first three on every
suite it generates before it scores a pipeline.

## Commands

```sh
scripts/ci/run.sh eval            # CI mode: text suite only
scripts/ci/run.sh eval --local    # all suites, required for acceptance

python3 eval/generate.py text --out eval-out/text --profile ci   # one suite
python3 eval/score.py --mode ci --suite text=eval-out/text       # plans in the suite directory
python3 eval/score.py --mode ci --suite text=eval-out/text --reference noop
```

`run.sh eval` builds the tree, then calls `eval/run_eval.py`, which
generates the suites for the mode, runs the sanity checks, runs
`build/cli/spg-cli eval` on each suite (simulated time, fixed 200 ms cycles,
2 workers; [cli.md](cli.md)) and scores the plans. In local mode it also runs
`spg-cli bench --realtime` on the text suite's 1920x1080 sequences for the
realtime suite. A missing or failing `spg-cli` prints `THRESHOLDS: FAIL` in
every mode; there is no "not run" outcome. The report lines include each
suite's frame and fail-closed counts and stage medians. SPG-16 tunes the
realtime numbers.
