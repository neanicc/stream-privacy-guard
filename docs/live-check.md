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

## Render path (SPG-14) and detection (SPG-14b)

The readback, lookahead ring, blur and the detection models in the module
need a real graphics context and a real source, which CI does not have. These
steps check that detected faces and watch-list text are masked (not the whole
source), that every other frame is covered whole, and the timing. See
[plugin-render.md](plugin-render.md).

1. Build and install the plugin from the commit under test (`scripts/ci/run.sh
   setup` and `build`, then copy `build/plugin/obs-spg.plugin`, which carries
   ONNX Runtime in `Contents/Frameworks`, into
   `~/Library/Application Support/obs-studio/plugins/`). Check the models are
   in `~/.cache/spg/models` (`python3 scripts/fetch_models.py --verify`; OBS
   started from the Dock does not see `SPG_CACHE_DIR`). Start OBS with a
   1080p60 canvas and open Help > Log Files > View Current Log.
2. Add a display capture, add the Privacy Guard filter with defaults. Expect:
   from the first frame the preview shows the source blurred whole (strong
   enough that window titles and 100 px text are unreadable), never sharp;
   the log has one `detection models loaded in <n> ms (faces=..., text=...,
   recognizer=...)` line, then `'<filter name>': scanning with the detection
   models`, and no `mask effect failed to load` line. Record the load time and
   the providers. Within about one lookahead of the load the preview turns
   sharp except where something is detected, and the log has `frames air
   with detected regions masked`.
3. Detection: add the planted entry `Jordan Fakeperson` to the watch-list.
   Type it into a text editor in large and in 14 px text, and paste
   `4111 1111 1111 1111` and `jordan.fakeperson@example.com` on other lines:
   each is blurred from the first frame it appears on (step through a
   recording), an ordinary line such as `Weather today is sunny` stays sharp,
   and the masks stay put while you scroll slowly. Play a video with faces in
   a browser window: faces are blurred, the rest of the frame is not. Record
   the smallest text size and face size masked, and any line or face that
   aired sharp even for one frame (with the time in the recording).
4. With the debug overlay on, check that the boxes sit on the text and faces
   in step 3, and that no recognized text is drawn or logged: search the log
   for `Fakeperson` and `4111` (no match is the pass condition).
5. Missing models: quit OBS, rename `~/.cache/spg/models` to
   `~/.cache/spg/models-off`, start OBS. Expect the source covered whole from
   the first frame and for good, the properties line "Detection models not
   found, so every frame is covered whole", and exactly one log line
   `detection models missing, covering every frame whole: yunet: ...`. Then
   replace one model file with a copy truncated by one byte (keep the name)
   and restart: the properties say the file is damaged, the log says
   `detection models corrupt`, and the source stays covered. Restore the
   folder and restart OBS.
6. Open the filter properties: the models line says they are loaded, the
   status line counts frames aired with only their masks and frames covered
   whole and names the last reason; the Audio line explains Sync Offset.
7. Turn on the debug overlay: green outlines on the masked text and faces
   only; a covered frame gets a square in the top-left corner (grey while the
   models load or with none, red when not scanned in time). Turn it off.
8. Run a CPU stress load (several `yes > /dev/null`) until frames cannot be
   scanned in time: the source is covered whole, never sharp. Set the fallback
   to "Cover with a solid fill": the covered frames turn black. "Repeat the
   last scanned frame": the last clean frame repeats. Set it back and stop
   the stress load.
9. Change the lookahead between 100 and 2000 ms several times while watching
   the preview and the Stats dock (View > Stats): no sharp frame at any
   switch, no frame-time spikes, no missed frames attributed to rendering
   lag, and video memory (Activity Monitor > GPU, or the OBS log) grows by
   roughly 8 MB per buffered frame at 1080p (about 950 MB at 2000 ms 60 fps).
10. Resize the capture (crop/pad or switch to a smaller display) and hide and
    show the source and the filter with the planted text on screen: the text
    is never sharp at any point; while hidden nothing is drawn.
11. Uncheck every category: the error line appears and the source stays
    covered. Recheck them.
12. Quit OBS with the filter active: it exits within a few seconds; the log
    ends with `[obs-spg] unloaded` and no crash report is created.
13. Record 30 s while doing steps 3 and 7 to 10 and scrub it frame by frame
    (or with `spg-cli scrub` once SPG-18 lands): no frame shows a planted
    string or a face sharp.

Record the Stats dock "average time to render frame" with and without the
filter, the number of missed or skipped frames, the GPU memory seen, and the
CPU use of OBS with the filter scanning (Activity Monitor): the models run on
the scheduler's worker threads with ONNX Runtime's default thread count.
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
