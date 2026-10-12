# Live OBS check

Automated tests and synthetic eval cannot show that the plugin protects a real
stream. This is the human acceptance record. Never fabricate an entry; "not
tested" is a valid result.

## Setup

- Dev Mac model and macOS version, OBS version, plugin build (commit SHA).
- A display capture or window capture source with the Privacy Guard filter.
- Watch-list entries that are planted test strings, not your real details, for
  example `Jordan Fakeperson` and `742 Evergreen Terrace`.
- Record locally (Settings → Output → Recording), not to a live platform, unless
  you deliberately use a private/unlisted test stream.

## Scenarios

- Planted string appears instantly (paste into a text editor) and fades in
  (notification).
- Scrolling chat containing the string at varied speeds; partial reveal while
  typing it.
- Small text (address bar), large text, light and dark themes.
- Alt-tab between windows, scene switch, source resize, toggling the filter.
- A video with faces played in a browser window.
- CPU/GPU stress (run a heavy game or `yes > /dev/null` on several cores) to force
  the fail-closed fallback; confirm the whole source blurs rather than leaking.
- Change settings live; restart OBS and confirm the watch-list persisted without
  appearing in the scene collection JSON in plain text (per the storage decision).

## Scrub the recording

Convert the recording to frames and run the scrubber added in SPG-18:

```sh
ffmpeg -i recording.mkv -vf fps=60 /tmp/spg-scrub/%06d.png
spg-cli scrub --frames /tmp/spg-scrub --planted planted.txt
```

The scrubber reports every frame where a planted string is still readable. Zero
leak frames is the pass condition. Watch the frames it flags yourself.

## Record

Append results below: date, commit, hardware, OBS version, scenario list, leak
frames found, fail-closed behavior observed, and remaining issues.

## Results

_No live runs recorded yet._
