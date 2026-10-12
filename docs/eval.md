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
| `realtime` | `spg-cli bench --realtime` replays a suite as a 60 fps feed against the wall clock with the default config, so slow detection shows up as fail-closed frames. | Locally on the dev Mac |

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

## Sanity checks the scorer must keep passing

- A pipeline that blurs nothing fails every recall threshold.
- A pipeline that blurs every frame entirely fails `clean_frame_blur_area_max`.
- Re-running the generator with the same seed produces byte-identical ground truth.

## Commands (available after SPG-03 and SPG-11)

```sh
scripts/ci/run.sh eval            # CI mode: text suite only
scripts/ci/run.sh eval --local    # all suites, required for acceptance
```
