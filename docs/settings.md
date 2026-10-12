# Filter settings and watch-list storage

What the Privacy Guard filter's properties hold and where they are saved
(SPG-15, [plan section 7](plan.md#7-obs-integration) and
[open question 4](plan.md#12-open-questions)). The OBS glue is
`plugin/src/spg-filter.cpp`; clamping, the PolicyConfig mapping and storage are
OBS-free in `plugin/src/settings/` and unit-tested on every OS.

## Properties

| Setting | Key | Default | Notes |
| --- | --- | --- | --- |
| Watch-list | see below | empty | Masked entry field (OBS password field), Add, a numbered entry list (text is never shown again), Remove selected, Remove all |
| Detect | `category_face`, `category_watchlist`, `category_pii_email`, `category_pii_phone`, `category_pii_card`, `category_pii_ip`, `category_pii_address`, `category_pii_token` | all on | Every category off is a configuration error: the filter covers the whole source |
| Watch-list sensitivity | `sensitivity` | `standard` | `standard` = spg-core fuzzy defaults (2 edits, ratio 0.2, partials from 6 characters); `high` = 3, 0.3, 5. No preset below the defaults. Placeholders until SPG-12 measures them |
| Lookahead | `lookahead_ms` | 500 | Clamped to [100, 2000] ms; the retroactive window equals it |
| Video memory for the lookahead | `vram_budget_mb` | 512 | MiB, clamped to [64, 16384]. A lookahead that needs more at the source's size and frame rate is shortened to fit, with a warning ([plugin-render.md](plugin-render.md#vram-budget)); a low value only shortens it |
| Mask hold | `hold_ms` | 750 | Clamped to [400, 10000] ms |
| Fallback | `fallback` | `blur_source` | `blur_source`, `hold_last_scanned`, `solid_fill`; there is no fail-open choice |
| Debug overlay | `debug_overlay` | off | Boxes and reason codes only, never text (drawn by SPG-14) |

Values loaded from a scene file are clamped and written back, so an old or
hand-edited file cannot switch a protection off: a lookahead or hold below the
floor is raised to it, and an unknown sensitivity or fallback name becomes the
default. The ceilings are safe values that bound VRAM (lookahead) and keep the
sliders usable. The floors are spg-core's own (`kMinLookahead`, `kMinHold` in
`spg/policy_config.hpp`, SPG-08b), checked at compile time, so no slider value
or clamped scene-file value builds a config spg-core rejects; a file saved with
a hold of 250 to 399 ms (the earlier floor) is raised to 400. Any config
spg-core still rejects (for example a watch-list entry that can never match)
turns detection off and the filter covers the whole source.

## Watch-list storage

OBS saves filter settings in plain text inside the scene collection JSON,
which people share for support. So, following the plan's recommended default
for open question 4, entries never go into filter settings:

- The settings hold only `watchlist_ref` (32 random hex digits) and
  `watchlist_count`. The entries live in
  `<OBS config>/plugin_config/obs-spg/watchlists/<ref>.txt`.
- On macOS and Linux the file is created `0600` in a `0700` directory, and a
  file or directory found readable by others is tightened on load. On Windows
  the file inherits the per-user `%APPDATA%` profile ACL (the user, SYSTEM and
  Administrators); the plugin sets no ACL of its own.
- Writes go to a new user-only temporary file that is renamed over the old one
  (synced first on macOS and Linux), so a failed write leaves the old list.
- Format: the line `spg-watchlist 1 # secret: do not share or attach this file`,
  then one entry per line with `\`, newline and carriage return escaped as
  `\\`, `\n` and `\r`. Anything else is corrupt. At most 256 entries of at most
  256 bytes each.
- The masked field `watchlist_new_entry` never stays in the settings: the
  field's modified callback moves each keystroke's text into the filter's
  memory (see [the undo stack](#the-obs-undo-stack)), and Add moves it into
  the file. The filter's `save` callback, which OBS calls just before writing
  the scene collection, still erases the key, for a script that set it
  directly, so an entry typed but never added is not saved there either.
- A filter copied in OBS (Duplicate, Copy and Paste Filters) arrives with its
  original's reference. A reference held by another live filter is forked: the
  copy gets a new reference and its own copy of the file.
- The file is read when a filter is created (on the thread that creates it)
  and when `update` sees a different reference in the settings. libobs runs a
  video filter's `update` deferred on the video thread, so a reference changed
  from outside (a script, obs-websocket) is read there: one file of at most
  about 128 KiB, plus a synced copy when that reference is held by another
  filter. OBS's own image source reads its file the same way. The render
  callback never reads files or takes the filter's lock.

## Failure handling

A watch-list that cannot be used turns detection off and the filter covers the
whole source (fail closed), with an error in the properties:

| Case | Result |
| --- | --- |
| No file, `watchlist_count` 0 | Empty list (a new filter) |
| No file, `watchlist_count` > 0 (scene collection moved without it) | Missing: fail closed |
| Malformed `watchlist_ref` (never turned into a path) | Fail closed |
| Unreadable, not a regular file, corrupt or over the limits | Fail closed |
| No absolute OBS config directory | Fail closed |
| A copy of a filter whose watch-list failed to load | Fail closed; the copy has no file, so it records one as expected (`watchlist_count` at least 1) and reports it missing after a reload |

While the store is broken, Add and Remove are disabled; **Remove all entries**
writes a new empty list over the old file, or under a new reference when the
old one is malformed or its file cannot be replaced (for example a directory in
its place). The old reference is released either way.

## Secrecy

Entries are `spg::Secret`s. The plugin logs counts and fixed error names only;
notices in the properties are fixed texts. `plugin/tests/settings_test.cpp`
captures `std::cout`, `std::cerr` and `std::clog` while a sentinel entry goes
through every store and settings path, and `plugin.loads` (macOS and Windows)
captures the whole libobs log while it adds, duplicates, removes and saves a
sentinel through the filter's real buttons, then checks the log, the saved
scene collection data and every file outside the watch-list directory.

Not covered: watch-list files of deleted filters stay on disk until removed by
hand, and the copies listed under [what is not covered](#what-is-not-covered).

## The OBS undo stack

OBS's undo stack is in memory only, but it keeps whole settings snapshots as
JSON text. In OBS 32.1.2 (the pinned version) the properties view handles an
edit in `WidgetInfo::ControlChanged`
(`shared/properties-view/properties-view.cpp`): when a burst of edits starts
it copies the settings, then for each edit it writes the widget's whole value
into the settings, calls the filters dialog's visual update
(`obs_source_update`) and then the property's modified callback. A single-shot
500 ms timer, restarted by every edit, finally calls `FilterChangeUndoRedo`
(`frontend/dialogs/OBSBasicFilters.cpp`), which stores the copy and the live
settings as JSON in the undo stack.

The masked field's modified callback runs synchronously on every keystroke, so
it copies the text into a `Secret` held by the filter (`pending_entry`),
overwrites the stored string in place and erases the key before the timer can
fire. The burst's first copy was taken before the first keystroke reached the
settings. Both undo snapshots therefore hold only the filter's own keys:
`watchlist_ref`, `watchlist_count`, the selected entry number and the plain
settings. The callback returns false, so the view is not rebuilt while the
user types and the field keeps showing the dots. libobs also runs the callback
whenever the properties are built (`obs_source_properties` applies the
settings), which resets `pending_entry` to the empty field a rebuilt view
shows: Add never adds text the field no longer shows. Add takes the text from
the settings if a script left it there, otherwise from `pending_entry`; a
field a script set to the empty string counts as empty, so Add rejects it
rather than adding text typed earlier.

Other undo paths: deleting a filter or pasting filters stores
`obs_save_source` output, which runs the `save` callback first; resetting a
filter to defaults stores the current settings, which hold no entry. Undo and
redo restore `watchlist_ref` and `watchlist_count`, never entries; the file
is not changed by them.

Evidence: `plugin.loads` (macOS and Windows) builds the properties once, types
a sentinel one character at a time through the same sequence of libobs calls
as `ControlChanged`, records the two snapshots `FilterChangeUndoRedo` would
store (and another for a slider move), and checks that no prefix of three or
more characters is in the settings after its keystroke, that every snapshot
holds only the filter's own keys and no fragment of the sentinel, that Add
then stores the typed entry, and that text typed before a rebuild is not
added. Removing the callback fails 8 of those checks. This emulates the view;
the real Qt view is a [live check](live-check.md) for a human.

## Watch-list text in memory

AGENTS.md: watch-list content is never written to crash dumps. Every copy of
an entry that spg-core or the plugin makes lives in `SecureHeap`
(`core/include/spg/secure_memory.hpp`):

- Regions are page-aligned anonymous mappings (`mmap`, `VirtualAlloc`). Blocks
  up to 16 KiB (a folded line of 4096 code points) come from size classes in
  shared 64 KiB regions, kept for reuse; larger ones get a region of their
  own. Mapping, exclusion and locking (and their undo) run outside the heap's
  lock, which guards only the free lists and the region list.
- Each region is excluded from crash dumps where the OS has a call for it,
  before any block in it is used: `madvise(MADV_DONTDUMP)` on Linux,
  `WerRegisterExcludedMemoryBlock` on Windows. macOS has none. The Windows
  call exists from Windows 10 1703 on and is looked up in `kernel32.dll` at
  run time, not imported: on an older Windows the plugin still loads and
  reports `none`, rather than failing to load (which would leave a scene's
  filter unapplied).
- Each region is locked in RAM (`mlock`, `VirtualLock`) so it is not paged to
  swap. Best effort: `RLIMIT_MEMLOCK` or the Windows working-set quota can
  refuse it, and the heap then carries on unlocked.
- Every block is zeroed when released (`SecureZeroMemory` on Windows, `memset`
  behind a compiler barrier elsewhere) before it is reused or unmapped.
- The plugin logs the mechanism once at load:
  `watch-list memory: crash-dump exclusion: madvise(MADV_DONTDUMP)`,
  `WerRegisterExcludedMemoryBlock` or `none`.

What uses it:

| Copy | Where |
| --- | --- |
| `spg::Secret` (entries, `PolicyConfig::watchlist`, OCR text) | Its bytes, always; a `std::vector<Secret>` holds only pointers and sizes |
| OCR text while it is decoded and joined | `ocr::SecureText` buffers; the whole OCR path is in [policy.md](policy.md#recognized-text-in-memory) |
| `Secret(std::string)` | Copies the string, then zeroes the string's whole buffer |
| Policy engine's compiled entries | `SecureU32String`s in a vector on `SecureAllocator` |
| Policy folding, matching forms, reversed forms, PII digit views | `SecureU32String` / `SecureString` temporaries |
| Watch-list file body | Read with `read()` / `_read()` straight into a `SecureString`, written with `write()` / `_write()` from one; no stream or stdio buffer |
| Typed, not yet added entry | `FilterState::pending_entry` (a `Secret`) |

### Platform limits

- **Linux:** the kernel's core dump, and so `systemd-coredump`, leaves the
  regions out (`/proc/<pid>/smaps` shows `dd` in their `VmFlags`). A debugger
  or another process that can read `/proc/<pid>/mem` still can.
- **Windows:** the registration applies to dumps that Windows Error Reporting
  collects, at most `WER_MAX_REGISTERED_ENTRIES` (512) blocks per process; the
  heap uses one per region. OBS 32.1.2 installs its own unhandled-exception
  filter (`libobs/obs-win-crash-handler.c`) that writes a text crash log
  (stack traces and the module list, no memory) and exits, so most crashes
  never reach WER. A dump written by a tool that calls `MiniDumpWriteDump`
  itself (a debugger or a dump utility) does not consult the registration
  and includes the regions.
- **macOS:** no per-region exclusion exists. Crash reports (`.ips`) hold
  backtraces and register state, not heap memory; full core dumps are off by
  default (`ulimit -c 0`) and, when a user enables them, include the
  regions. Locking and zeroing still apply.
- Hibernation writes RAM, locked pages included, to disk (`hiberfil.sys`,
  `sleepimage`).

### What is not covered

- **The masked field on its way in.** Each keystroke passes through Qt (the
  `QLineEdit` and a temporary UTF-8 `QByteArray`) and into a libobs
  `obs_data` item before the callback moves it. The callback overwrites the
  item with `*`s before erasing it (libobs 32.1.2 reuses an item's storage
  for a value of the same length), but Qt's and libobs's freed buffers are
  not zeroed, and the `QLineEdit` keeps the text, and its own edit history,
  until the view is rebuilt (Add does that). That is Qt memory the plugin
  cannot reach.
- **Short strings.** A `std::basic_string` keeps a few characters (3 or 4
  `char32_t`, 15 or 22 `char`, by standard library) inside the string
  object. For temporaries that object is on the stack, which a full dump
  includes. A `Secret` never does this.
- **Strings that are not `Secret`s.** Only watch-list entries are tracked.
  OBS's and the frontend's copies of other settings and the watch-list file
  path are ordinary memory; none of them hold entries.
- **The file.** The watch-list file and the OS file cache hold the entries by
  design (user-only permissions, above).
