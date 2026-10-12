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

## Render path (SPG-14)

The readback, lookahead ring and blur need a real graphics context, which CI
does not have. Until detection models are loaded by the plugin, every frame
is expected to air covered whole; these steps check that path and its timing.
See [plugin-render.md](plugin-render.md).

1. Build and install the plugin from the commit under test (`scripts/ci/run.sh
   build`, then copy `build/plugin/obs-spg.plugin` into
   `~/Library/Application Support/obs-studio/plugins/`). Start OBS with a
   1080p60 canvas and open Help > Log Files > View Current Log.
2. Add a display capture, add the Privacy Guard filter with defaults. Expect:
   the preview shows the source blurred whole (strong enough that window
   titles and 100 px text are unreadable), never sharp, from the first frame;
   the log has `covering the whole source: starting`, then about 10 s later
   `covering the whole source: not scanned in time`, and no `mask effect
   failed to load` line.
3. Open the filter properties: the status line counts frames covered whole
   and names the last reason; the Audio line explains Sync Offset.
4. Turn on the debug overlay: a red square appears in the top-left corner of
   the source (a grey one for the first half second). Turn it off.
5. Set the fallback to "Cover with a solid fill": the source turns black.
   "Repeat the last scanned frame": the source is blurred whole (nothing was
   ever scanned clean). Set it back.
6. Change the lookahead between 100 and 2000 ms several times while watching
   the preview and the Stats dock (View > Stats): no sharp frame at any
   switch, no frame-time spikes, no missed frames attributed to rendering
   lag, and video memory (Activity Monitor > GPU, or the OBS log) grows by
   roughly 8 MB per buffered frame at 1080p (about 950 MB at 2000 ms 60 fps).
7. Resize the capture (crop/pad or switch to a smaller display) and hide and
   show the source and the filter: no sharp frame at any point; while hidden
   nothing is drawn.
8. Uncheck every category: the error line appears and the source stays
   covered. Recheck them.
9. Quit OBS with the filter active: it exits within a few seconds; the log
   ends with `[obs-spg] unloaded` and no crash report is created.
10. Record 30 s while doing steps 4 to 7 and scrub it frame by frame (or with
    `spg-cli scrub` once SPG-18 lands): no frame shows the source sharp.

Record the Stats dock "average time to render frame" with and without the
filter, the number of missed or skipped frames, and the GPU memory seen.

## Record

Append results below: date, commit, hardware, OBS version, scenario list, leak
frames found, fail-closed behavior observed, and remaining issues.

## Results

_No live runs recorded yet._
