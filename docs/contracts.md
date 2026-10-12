# Core contracts

The types every lane shares, defined in `core/include/spg/` (SPG-02) and
summarized in [plan section 5](plan.md#5-contracts). The headers are the source
of truth; this page explains the rules behind them and specifies the JSON forms
that the CLI, the plugin and the Python eval tooling exchange.

Changing a contract follows [team-plan.md](team-plan.md): one dedicated PR that
updates the headers, every consumer, the fixtures in `tests/fixtures/contracts/`
and this page together. A breaking JSON change bumps `schema`.

## Types

| Header | Type | Meaning |
| --- | --- | --- |
| `time.hpp` | `Timestamp`, `Duration` | Monotonic nanoseconds since an arbitrary epoch (the unit OBS uses). |
| `time.hpp` | `Clock`, `SteadyClock`, `ManualClock` | Injected time source. Core code takes a `Clock&` and never reads a global clock; tests use `ManualClock`. |
| `frame.hpp` | `FrameView` | Non-owning frame in Source space: up to three plane pointers and strides, width, height, `PixelFormat` (`BGRA`, `NV12`, `I420`), timestamp. `validate()` rejects zero sizes, missing planes and short strides. |
| `geometry.hpp` | `Rect`, `Box`, `Space` | Float rectangle (`x`, `y`, `width`, `height`) and a box that carries its `Space`: `Source`, `Readback` or `ModelInput`. |
| `category.hpp` | `Category`, `CategorySet` | `Face`, `Watchlist`, `PiiEmail`, `PiiPhone`, `PiiCard`, `PiiIp`, `PiiAddress`, `PiiToken`. One enum names enabled categories, verdict reasons and ground-truth kinds. |
| `detection.hpp` | `Detection` | Box in Source space, kind (`Face`, `TextRegion`), score in [0, 1], detector id. |
| `detection.hpp` | `TextHit`, `Verdict` | A text detection, its recognized text (a `Secret`, memory only) and the verdict. A verdict holds reason codes only. |
| `detection.hpp` | `Track` | Id, Source-space box, velocity (Source px/s), last confirmed time, hold expiry, kind. |
| `mask.hpp` | `MaskPlan`, `MaskRect` | For one frame timestamp: Source-space rects with a style (`blur`, `pixelate`, `solid_fill`) and a `fallback` flag. |
| `policy_config.hpp` | `PolicyConfig` | Enabled categories, watch-list, fuzzy limits, lookahead/retro/hold, margin, fallback mode, mask style. |
| `policy.hpp` | `PolicyEngine`, `LogSink` | Turns recognized text into a `Verdict` under a `PolicyConfig`; rules in [policy.md](policy.md). `LogSink` receives diagnostic lines that never contain text. |
| `ground_truth.hpp` | `GroundTruthFrame` | Eval ground truth for one frame. |
| `secret.hpp` | `Secret` | Watch-list entries and OCR text. |
| `error.hpp` | `ContractError` | Thrown for any contract violation. |

Reason codes are `FACE`, `WATCHLIST`, `PII_EMAIL`, `PII_PHONE`, `PII_CARD`,
`PII_IP`, `PII_ADDRESS` and `PII_TOKEN`. JSON uses the lower-case category
names `face`, `watchlist`, `pii:email`, `pii:phone`, `pii:card`, `pii:ip`,
`pii:address` and `pii:token`.

## Coordinate spaces

Every space is pixels with the origin at the top-left corner, x to the right
and y down. A `Box` always says which space it is in.

| Space | Pixels of |
| --- | --- |
| `Source` | The full-resolution OBS source the filter is on. Detections, tracks, mask plans and ground truth are always in this space. |
| `Readback` | The downscaled copy of the whole source that workers scan. `ReadbackMapping(source, readback)` scales each axis independently. |
| `ModelInput` | One model's input tensor. `ModelInputMapping(crop, input, fit)` resizes `crop` (a tile or the whole frame, in Readback pixels) into `input` with `Fit::Stretch`, `Fit::LetterboxCenter` or `Fit::LetterboxTopLeft`: `model = (readback - crop.origin) * scale + pad`. |

`convert(box, target, SpaceMap{readback, model_input})` in
`core/src/frame/geometry.cpp` is the only conversion between spaces. It
computes in double precision and rounds outward when narrowing the result to
`float`: the origin rounds down and the far edge (origin + length, summed in
double) rounds up, so the result contains the double-precision image of the box and a
converted mask never loses an edge pixel to rounding. It does not clip (a box in
letterbox padding maps outside the source), and throws `ContractError` for an
invalid box, a result outside the `float` range, or a `ModelInput` conversion
without a model mapping. Property tests in `tests/unit/geometry_test.cpp` check
that no converted edge moves inward, that round trips between every pair of
spaces never shrink the box and grow it by at most the rounding bound, that a
direct conversion matches one through `Readback`, and that containment is
preserved, over 5000 seeded random mappings each.

`enclosing_pixels(rect, bounds)` turns a float rect into the smallest pixel
rect that contains it, clipped to the frame. It rounds outward so that turning a
mask into pixels never uncovers a pixel the float rect touched.

## Secrecy rules

- Watch-list entries and recognized text are `Secret`s. `Secret` has no stream
  operator, formatter or implicit conversion; `reveal()` is the only accessor,
  and a static assertion in the tests keeps it unprintable.
- A `Verdict` and a reason code never contain the matched text.
- `ContractError` messages name fields, array indices and byte offsets only.
  The JSON layer never forwards the JSON library's own messages, which can quote
  input. `tests/unit/json_test.cpp` feeds a sentinel string through malformed
  documents and asserts no error contains any part of it.
- `serialize_policy_config` writes the watch-list. Its output belongs only in
  the user's own config file. Eval configs use synthetic entries only.

## JSON

Common rules for every document:

- UTF-8, one JSON object per document, `"schema": 1` required.
- Unknown fields and duplicate field names are errors. A misspelled
  `"watchlist"` must fail loudly rather than silently disable matching.
- Integers must be JSON integers. Numbers must be finite. A field stored as
  `float` is range-checked on the parsed value before it is narrowed, so a value
  just outside its range (`1.00000001` for a visibility, `-1e-50` for a width)
  is an error rather than rounding into range, and a value beyond the `float`
  range is an error.
- Box objects are `{"x": .., "y": .., "w": .., "h": ..}` in Source pixels, and
  documents that contain boxes declare `"space": "source"`.
- Serializers write one line with no trailing newline, so a sequence of frames
  can be stored as JSON Lines (one document per line). Floats are written with
  enough digits to restore the exact `float`.
- C++ API: `parse_policy_config`, `serialize_policy_config`,
  `parse_ground_truth_frame`, `serialize_ground_truth_frame`, `parse_mask_plan`,
  `serialize_mask_plan` in `spg/json.hpp`. The library is nlohmann/json 3.12.0
  (MIT), fetched by CMake and pinned by SHA-256, private to `spg-core`.

### Policy config

Every field except `schema` is optional and defaults to the value shown.

```json
{
  "schema": 1,
  "categories": ["face", "watchlist", "pii:email", "pii:phone", "pii:card",
                 "pii:ip", "pii:address", "pii:token"],
  "watchlist": [],
  "fuzzy": {"max_edits": 2, "max_edit_ratio": 0.2, "min_partial_chars": 6},
  "lookahead_ms": 500,
  "retro_ms": 500,
  "hold_ms": 750,
  "margin_px": 4,
  "margin_ratio": 0.25,
  "fallback": "blur_source",
  "mask_style": "blur"
}
```

| Field | Rule |
| --- | --- |
| `categories` | Enabled categories. Absent means all; `[]` means none. |
| `watchlist` | Strings; an entry that is empty or blank is an error (reported by index). Blank means only Unicode White_Space characters (including U+00A0 and U+3000) and the zero-width characters U+200B..U+200D, U+2060 and U+FEFF. |
| `fuzzy` | For an entry of n normalized characters the allowed edit distance is `min(max_edits, floor(max_edit_ratio * n))`; a partial reveal counts only with at least `min_partial_chars` characters. `max_edit_ratio` in [0, 1], `min_partial_chars` ≥ 1. |
| `lookahead_ms` | [0, 5000]. |
| `retro_ms` | [0, `lookahead_ms`]; defaults to `lookahead_ms` (plan section 4). |
| `hold_ms` | [0, 60000]. |
| `margin_px`, `margin_ratio` | Each mask edge grows by `margin_px + margin_ratio * min(width, height)` Source pixels. Not negative. |
| `fallback` | `blur_source` (default), `hold_last_scanned` or `solid_fill`. There is no fail-open mode. `hold_last_scanned` re-emits the last frame released after a complete scan, exactly as released with its masks applied; it never emits the unscanned frame or an unmasked copy of an earlier one. When there is no such frame for the current source size and content (the first frames after the filter starts, or after the source is resized or switched), it applies `blur_source` instead; SPG-10 implements and tests this. |
| `mask_style` | `blur`, `pixelate` or `solid_fill`. |

The fuzzy and margin numbers are starting points. SPG-08 implemented the matcher
that reads `fuzzy` ([policy.md](policy.md)) and kept these values; SPG-12 and
SPG-09 tune them against the eval and update this page. `0` is a valid value for `lookahead_ms`,
`hold_ms`, `margin_px` and `margin_ratio` and turns that protection off
(`lookahead_ms: 0` leaves no retroactive window). Whether to impose a floor is
open for SPG-09 and SPG-10 to settle against the eval.

### Ground truth (one frame)

Written by the eval generator (SPG-03), read by the scorer. Every field is
required. Example: [`tests/fixtures/contracts/ground_truth_frame.json`](../tests/fixtures/contracts/ground_truth_frame.json).

```json
{
  "schema": 1,
  "space": "source",
  "frame": 12,
  "timestamp_ns": 200000000,
  "width": 1920,
  "height": 1080,
  "instances": [
    {"id": "wl-0", "kind": "watchlist", "box": {"x": 100, "y": 40.5, "w": 220, "h": 18}, "visibility": 1.0},
    {"id": "decoy-1", "kind": "decoy", "box": {"x": 0, "y": 0, "w": 50, "h": 12}, "visibility": 0}
  ]
}
```

| Field | Rule |
| --- | --- |
| `frame` | Frame index, integer ≥ 0. |
| `timestamp_ns` | Integer nanoseconds; matches the mask plan for the same frame. |
| `width`, `height` | Source size, ≥ 1. |
| `instances[].id` | Non-empty, unique within the frame; the same id names the same planted item on every frame, which is how first-frame recall finds an item's first visible frame. |
| `instances[].kind` | A category name or `decoy` (a near-miss that should not be blurred). |
| `instances[].box` | The item's full box in Source pixels, even when partly revealed. |
| `instances[].visibility` | [0, 1]: how much of the item shows on this frame (fade alpha, reveal fraction). 0 means present but not visible. Recall over precision: the reference rule is that an instance with visibility > 0 is visible. |

Ground truth never contains an item's text. The pipeline gets the planted
watch-list strings from the suite's policy config.

### Mask plan (one frame)

Written by `spg-cli run` (SPG-11), read by the scorer and compositor tests.
Every field is required. Example: [`tests/fixtures/contracts/mask_plan.json`](../tests/fixtures/contracts/mask_plan.json).

```json
{
  "schema": 1,
  "space": "source",
  "timestamp_ns": 200000000,
  "fallback": false,
  "rects": [{"box": {"x": 92, "y": 32.5, "w": 236, "h": 34}, "style": "blur"}]
}
```

`fallback: true` means the frame failed closed and the configured fallback
covers the whole source; the scorer counts every instance on it as covered.
`rects` still lists what was found. The scorer pairs a mask plan with ground
truth by `timestamp_ns`.
