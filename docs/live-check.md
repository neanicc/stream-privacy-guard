# Live OBS check

Automated tests and synthetic eval cannot show that the plugin protects a real
stream. This is the human acceptance protocol and its record. Never fabricate
an entry; "not tested" is a valid result. Stream Privacy Guard is a safety
net, not a guarantee: a pass here means the planted strings in this session
were covered, not that nothing can ever leak.

The run is a human step (`needs-human`): it needs OBS on a real machine, a
real capture source and a person watching. Agents write and update this
protocol; they never fill in the Results.

Work through the parts in order. Each step says what to do, what to expect and
what to record. A step that fails is recorded as failed with the time and the
log line, and the run continues unless the step says to stop.

## 0. Before you start

1. **Planted strings.** Use only made-up test strings, never your real
   details. Create a folder outside the repository, for example
   `~/spg-live`, and in it a file `planted.txt` with one string per line:

   ```text
   Jordan Fakeperson
   742 Evergreen Terrace
   4111 1111 1111 1111
   jordan.fakeperson@example.com
   ```

   `spg-cli scrub` names them `#1` to `#4` in this order. Never commit this
   file, the recording or the extracted frames, and do not attach them to an
   issue or PR.
2. **Record the setup**: Mac model, macOS version, OBS version, the commit
   under test (`git rev-parse HEAD`), canvas resolution and frame rate
   (Settings > Video), and the free disk space (`df -h ~`). The scrub step
   needs about 3 to 6 MB per extracted 1080p frame: 30 s at 60 fps is about
   1800 frames, 5 to 10 GB.
3. **Build and install** the plugin and the CLI from the commit under test:
   `scripts/ci/run.sh setup`, then `scripts/ci/run.sh build`. Quit OBS, copy
   `build/plugin/obs-spg.plugin` (it carries ONNX Runtime in
   `Contents/Frameworks`) into
   `~/Library/Application Support/obs-studio/plugins/`, replacing any older
   copy. Check the models with `python3 scripts/fetch_models.py --verify`
   (they must be in `~/.cache/spg/models`; OBS started from the Dock does not
   see `SPG_CACHE_DIR`). `spg-cli` is `build/cli/spg-cli`.
4. **OBS settings**: a 1080p60 canvas. Recording to a local file
   (Settings > Output > Recording, format `mkv`, any encoder), never to a
   live platform; use a private or unlisted test stream only if you mean to
   test streaming. Open Help > Log Files > View Current Log and keep it open.

## A. Loading and settings (SPG-13, SPG-15, SPG-15b)

1. **Plugin loads.** Start OBS. Expect in the log
   `[obs-spg] loaded version <v> (spg-core <v>)` and
   `[obs-spg] watch-list memory: crash-dump exclusion: <value>`, and no crash
   or error dialog. Record the version and the crash-dump exclusion value.
2. **Filter added with defaults.** Add a Display Capture source, then Filters >
   + > **Privacy Guard** with the defaults. Expect: from the very first frame
   the preview shows the source blurred whole (window titles and 100 px text
   unreadable), never sharp; the log has one
   `detection models loaded in <n> ms (faces=..., text=..., recognizer=...)`
   line, then `'<filter name>': scanning with the detection models`, and no
   `mask effect failed to load` line. Within about one lookahead of the load
   the preview turns sharp except where something is detected, and the log
   has `frames air with detected regions masked`. Record the load time and
   the providers named in the log.
3. **Watch-list field and buttons.** Open the filter properties. Type
   `Jordan Fakeperson` into **New entry**: every character shows as a dot;
   the field's show/hide toggle reveals and hides it again. Press
   **Add entry**: "Entry added.", the field empties, the list shows a
   numbered entry (never its text) and the count rises; the log has
   `watch-list saved: 1 entries`. Add the other three planted strings the same
   way. Select an entry and press **Remove selected entry**, then
   **Remove all entries**: after each, the list and count refresh at once
   and the log has a new `watch-list saved: <n> entries` line. Add the four
   strings again before continuing. Record any button that did not refresh
   the list.
4. **Undo and redo never restore typed text.** Select the entry for
   `742 Evergreen Terrace` (the second) and press **Remove selected entry**,
   so adding it again is not a duplicate. Type `742 Evergreen Terrace` into
   the field slowly, wait two seconds, move a slider, press **Add entry**, then
   undo and redo several times (Edit menu). Expect: the field keeps showing
   dots while typing, Add stores the entry (four entries again), and no undo
   or redo step puts text back into the field. Then type text, do not press Add, close and
   reopen the properties: the field is empty and **Add entry** adds nothing
   ("Nothing was added: the entry is empty.").
5. **Not in the scene collection.** Scene Collection > Export to
   `~/spg-live/scenes.json` (or use the file under
   `~/Library/Application Support/obs-studio/basic/scenes/`), then
   `grep -c -e Fakeperson -e Evergreen -e 4111 ~/spg-live/scenes.json`.
   Expect `0`. Quit and restart OBS: the filter shows the same entry count
   and the log has `watch-list loaded: 4 entries`.
6. **File permissions.**
   `ls -la ~/Library/Application\ Support/obs-studio/plugin_config/obs-spg/watchlists/`.
   Expect the directory `drwx------` and each `<32 hex>.txt` file
   `-rw-------`. Record the listing (file names are random references, not
   secrets).
7. **Duplicated filter gets its own file.** Right-click the filter >
   Duplicate (or Copy Filters, Paste Filters onto another source). Expect the
   log line `copied filter: watch-list forked to its own file` and one more
   file in the `watchlists` folder. Remove an entry in the copy: the
   original's count does not change. Delete the copy afterwards.

## B. Detection and render path (SPG-14, SPG-14b)

The readback, lookahead ring, blur and the detection models need a real
graphics context and source, which CI does not have. These steps check that
detected faces and planted text are masked (not the whole source), that
every other frame is covered whole, and the timing. Background:
[plugin-render.md](plugin-render.md).

1. **Detection.** Type `Jordan Fakeperson` into a text editor in large and in
   14 px text, and paste `4111 1111 1111 1111` and
   `jordan.fakeperson@example.com` on other lines. Expect each blurred from
   the first frame it appears on (check in the recording, part E), an
   ordinary line such as `Weather today is sunny` sharp, and the masks
   staying on the text while you scroll slowly. Play a video with faces in a
   browser window: faces blurred, the rest of the frame not. Record the
   smallest text size and face size masked, and any line or face that aired
   sharp even for one frame.
2. **Debug overlay.** Turn on **Debug overlay**: green outlines sit on the
   masked text and faces only; a covered frame gets a square in the top-left
   corner (grey while the models load or with none, red when not scanned in
   time). No recognized text is drawn. Search the log for `Fakeperson`,
   `Evergreen` and `4111`: no match is the pass condition. Turn it off.
3. **Status lines.** In the properties, the models line says they are loaded,
   the status line counts frames aired with only their masks and frames
   covered whole and names the last reason, and the Audio line explains Sync
   Offset.
4. **Missing and damaged models.** Quit OBS, rename `~/.cache/spg/models` to
   `~/.cache/spg/models-off`, start OBS. Expect the source covered whole from
   the first frame and for good, the properties line "Detection models not
   found, so every frame is covered whole", and exactly one log line
   `detection models missing, covering every frame whole: yunet: ...`. Then
   restore the folder, replace one model file with a copy truncated by one
   byte (keep the name; save the original) and restart: the properties say a
   file is damaged, the log says `detection models corrupt`, and the source
   stays covered. Restore the original file and restart OBS.
5. **No categories.** Uncheck every category under **Detect**: the error line
   "Every category is off..." appears and the source stays covered. Recheck
   them.
6. **Lookahead changes.** Change **Lookahead** between 100 and 2000 ms
   several times while watching the preview and the Stats dock (View >
   Stats): no sharp frame at any switch, no frame-time spikes, no missed
   frames attributed to rendering lag, and video memory (Activity Monitor >
   GPU, or the OBS log) growing by about 8 MB per buffered frame at 1080p up
   to the budget (about 500 MB at 1000 ms and 60 fps; longer lookaheads are
   shortened, C3). Set it back to 500 ms.
7. **Resize, hide, disable.** With the planted text on screen, resize the
   capture (crop/pad, or switch to a smaller display), hide and show the
   source, and disable and enable the filter: the text is never sharp at any
   point; while hidden nothing is drawn.
8. **Render cost.** Record the Stats dock "average time to render frame"
   with the filter enabled and disabled, the missed or skipped frames, the
   GPU memory seen, and the CPU use of OBS with the filter scanning (Activity
   Monitor; the models run on the scheduler's worker threads with ONNX
   Runtime's default thread count). The filter maps a stage surface on the
   render thread every frame (`gs_stagesurface_map` of the copy staged one
   frame earlier), which can wait on the GPU. If the render time with the
   filter is more than about 1 ms above the time without it, or
   rendering-lag frames appear, profile one render (Instruments, or OBS's
   profiler in the log at exit) and record the time spent in the map
   separately (SPG-16 decides whether to skip a map that is not ready).

## C. Settings changes, rebuilds and the VRAM budget (SPG-14c, SPG-14d)

1. **A settings change covers, never leaks.** While recording, with the
   planted text on screen, change **Mask hold after last detection** (or the
   sensitivity, or a category). Expect the log line
   `covering the whole source: settings changed`, the source shown as a solid
   fill (never sharp, never an unscanned frame) until the new settings are
   built, then `frames air with detected regions masked`. Record how long the
   fill lasted (the two log timestamps, or frames in the recording). Repeat
   three times.
2. **Removing the filter during a rebuild.** Change a setting and remove the
   filter right away, before the fill ends. Expect OBS to stay up and
   responsive, the source to show unfiltered only after the filter is gone
   (that is the user's choice, not a leak), and no crash report. Undo the
   removal (Edit > Undo) or add the filter again with the planted entries.
3. **VRAM budget.** With the 1080p60 canvas, set the lookahead to 2000 ms and
   leave **Video memory for the lookahead** at 512 MiB. Expect one log line
   `VRAM budget: lookahead shortened from 2000 ms to 1016 ms; the budget of
   512 MiB holds 64 frames of 1920x1080 (7.9 MiB each)`, and on reopening the
   properties a warning naming 2000 ms, 1016 ms and the Sync Offset advice.
   Video memory grows by about 500 MB, not 1 GB. Set the budget to 64 MiB:
   the lookahead drops to 83 ms, the source stays covered (never sharp)
   through the switch, and the log has one new line. Then give the source a
   4K base size (a 3840x2160 image source, or a capture scaled to 4K) at the
   default budget and 500 ms: one line about 216 ms appears after the
   resize, and frames refused while the old lookahead drained are counted in
   the warning, drawn black, never sharp. Return to 1080p: one `fits again`
   line. Record the video memory seen at each step. Set the lookahead back
   to 500 ms and the budget to 512 MiB.
4. **Retry after a failed rebuild.** A rebuild that fails (for example while
   the GPU is short of memory) logs
   `could not start detection for these settings: covering the whole source`
   once; the plugin retries about every second. There is no reliable way to
   cause this by hand. If the line appears at any point in the run, record
   when, confirm the source stayed covered (never sharp), and how long until
   the next `frames air with detected regions masked` (expected: about a
   second once the cause is gone). Otherwise record "not observed".

## D. Fail closed under load

1. Run a CPU stress load (several `yes > /dev/null &` in a terminal, or a
   heavy game) until frames cannot be scanned in time: the source is covered
   whole, never sharp, and the log has
   `covering the whole source: not scanned in time`. Set **When a frame
   cannot be scanned in time** to "Cover with a solid fill": the covered
   frames turn black. "Repeat the last scanned frame": the last clean frame
   repeats. Set it back to "Blur the whole source", stop the stress load
   (`killall yes`) and check the masks return.
2. **Clean exit.** Quit OBS with the filter active: it exits within a few
   seconds, the log ends with `[obs-spg] unloaded`, and no crash report is
   created (Console > Crash Reports).

## E. Record the scenario session

Start OBS again with the filter, the four planted entries and the defaults.
Start recording, then go through these in about 30 to 60 s, noting the
recording time of each:

1. A planted string appears instantly (paste it into a text editor) and fades
   in (a notification that contains it).
2. Chat containing a planted string scrolling at slow, medium and fast
   speeds; typing a planted string one character at a time (partial reveal).
3. Small text (the browser address bar), large text, a light and a dark
   theme.
4. Alt-tab between windows, a scene switch, a source resize and toggling the
   filter's visibility.
5. The video with faces in a browser window.
6. One settings change (C1) and one lookahead change (B6).

Stop recording. Keep the file only on this machine.

## F. Scrub the recording

`spg-cli scrub` runs the text detector and recognizer on every frame (with
more thorough offline settings than the live pipeline: tiles overlapping by
half and fainter candidate boxes kept) and matches each region's text against
the planted strings with the policy engine's fuzzy matcher, at the High
sensitivity by default. It prints only frame numbers, times, boxes and the
planted strings' numbers, never their text, and writes no frames.

1. **Extract frames** at the canvas frame rate and full size (scaling down
   would make text harder to read and hide leaks), numbered from 0 so that a
   frame's number divided by the frame rate is its time in the recording:

   ```sh
   mkdir -p ~/spg-live/frames
   ffmpeg -i ~/Movies/<recording>.mkv -vf fps=60 -start_number 0 ~/spg-live/frames/%06d.png
   ```

2. **Scrub:**

   ```sh
   build/cli/spg-cli scrub --frames ~/spg-live/frames --planted ~/spg-live/planted.txt \
     --fps 60 --report ~/spg-live/scrub.json --workers 4
   ```

   Progress goes to stderr every 100 frames. Scrub is slow on purpose (it
   reads more of each frame than the live pipeline): on the dev Mac's CPU a
   720p frame took about 0.85 s of text detection per worker, so 1800 frames
   of 1080p can take 30 to 60 minutes; set `--workers` to the number of
   performance cores. The exit code is 0 when no
   planted string was found readable in any frame (last line `PASS: ...`), 3
   when at least one was (`LEAK: ...`, with one `leak: frame <n> (<file>,
   <s> s): planted #<i> at x <x> y <y>, <w>x<h>` line per region), and 1 when
   the scrub could not finish (a frame it cannot read, missing models) or
   read no text at all (`NOTHING CHECKED: ...`, a wrong folder or wrong-scale
   frames): a frame that was not checked never counts as clean.
3. **Pass condition: zero leak frames.** Open every flagged frame
   (`~/spg-live/frames/<file>`) and look at the box: record the frame, the
   time, which planted string, whether it is really readable, and what was
   happening (scenario from part E). A flagged region inside a mask means the
   blur cells are too fine for that text (SPG-14 sizes: cells at least a
   third of the mask's shorter side and 8 px; the whole-source fallback at
   least 1/16 of the shorter side and 16 px): record its size.
4. **Positive control.** Record 5 s with the filter disabled and the planted
   strings on screen. Extract it into its own empty folder and give it its
   own report, so it never mixes with the session's frames or overwrites its
   report:

   ```sh
   mkdir -p ~/spg-live/control-frames
   ffmpeg -i ~/Movies/<control>.mkv -vf fps=60 -start_number 0 ~/spg-live/control-frames/%06d.png
   build/cli/spg-cli scrub --frames ~/spg-live/control-frames --planted ~/spg-live/planted.txt \
     --fps 60 --report ~/spg-live/control.json --workers 4
   ```

   Expect exit code 3 and every planted string flagged; if one is not, scrub cannot see it in
   this setup (too small, unusual font) and part F proves nothing about it:
   record which, and check those frames by eye.
5. **What scrub does not check.** Faces, and text the OCR cannot read at all.
   Step through the face scenario (E5) and every scene switch and resize (E4)
   frame by frame by eye (for example `ffplay` or QuickTime, arrow keys) and
   record any face or planted string that is sharp in any frame.
6. Delete the frames (`rm -rf ~/spg-live/frames ~/spg-live/control-frames`)
   and keep or delete the recordings; never upload any of them.

## Record

Append one entry per run under Results:

```text
### <date> - <commit> - <tester>
Setup: <Mac model>, macOS <v>, OBS <v>, canvas <WxH>@<fps>, recording <codec>
A1-A7: pass/fail/not tested each, crash-dump exclusion value, notes
B1-B8: pass/fail/not tested each; smallest text and face masked; model load
  time and providers; render time with/without the filter; GPU memory; CPU
C1-C4: pass/fail/not observed each; settings-change fill duration (3 runs)
D1-D2: pass/fail each
E: scenario list with recording times
F: frames scrubbed, spg-cli scrub exit code, leak frames (frame, time,
  planted #, real or false alarm, scenario), positive control result,
  by-eye check of faces and transitions
Remaining issues: <links to issues filed>
```

## Results

_No live runs recorded yet._
