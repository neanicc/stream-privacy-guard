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
- Type a planted entry into the masked field slowly, wait two seconds, move a
  slider, press Add, then undo and redo several times (Edit menu): the field
  keeps showing dots while typing, Add stores the entry, and no undo step
  restores text into the field. Close and reopen the properties with text
  typed but not added: the field is empty and Add adds nothing. Check the OBS
  log for `watch-list memory: crash-dump exclusion:` and record its value.

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
   roughly 8 MB per buffered frame at 1080p, up to the VRAM budget (about
   500 MB at 1000 ms 60 fps; longer lookaheads are shortened, step 11).
7. Resize the capture (crop/pad or switch to a smaller display) and hide and
   show the source and the filter: no sharp frame at any point; while hidden
   nothing is drawn.
8. Uncheck every category: the error line appears and the source stays
   covered. Recheck them.
9. Quit OBS with the filter active: it exits within a few seconds; the log
   ends with `[obs-spg] unloaded` and no crash report is created.
10. Record 30 s while doing steps 4 to 7 and scrub it frame by frame (or with
    `spg-cli scrub` once SPG-18 lands): no frame shows the source sharp.
11. VRAM budget (SPG-14c). With the 1080p60 canvas, set the lookahead to
    2000 ms and leave **Video memory for the lookahead** at 512 MiB. Expect
    one log line `VRAM budget: lookahead shortened from 2000 ms to 1016 ms;
    the budget of 512 MiB holds 64 frames of 1920x1080 (7.9 MiB each)`, and
    on reopening the properties a warning naming 2000 ms, 1016 ms and the
    Sync Offset advice. Video memory grows by about 500 MB, not 1 GB. Set
    the budget to 64 MiB: the lookahead drops to 83 ms, the
    source stays covered (never sharp) through the switch, and the log has
    one new line. Then give the source a 4K base size (a 3840x2160 image or
    a scaled capture) at the default budget and 500 ms: one line about
    216 ms appears after the resize, and frames refused while the old
    lookahead drained are counted in the warning, drawn black, never sharp.
    Return to 1080p: one `fits again` line. Record the video memory seen at
    each step.

Record the Stats dock "average time to render frame" with and without the
filter, the number of missed or skipped frames, and the GPU memory seen.
The filter maps a stage surface on the render thread every frame
(`gs_stagesurface_map` of the copy staged one frame earlier), and this map
can wait on the GPU. If the render time with the filter is more than about
1 ms above the time without it, or rendering-lag frames appear, profile one
render (Instruments, or OBS's profiler in the log at exit) and record the
time spent in the map separately. SPG-16 decides whether to skip a map that
is not ready.

## Record

Append results below: date, commit, hardware, OBS version, scenario list, leak
frames found, fail-closed behavior observed, and remaining issues.

## Results

_No live runs recorded yet._
