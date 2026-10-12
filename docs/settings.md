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
| Mask hold | `hold_ms` | 750 | Clamped to [250, 10000] ms |
| Fallback | `fallback` | `blur_source` | `blur_source`, `hold_last_scanned`, `solid_fill`; there is no fail-open choice |
| Debug overlay | `debug_overlay` | off | Boxes and reason codes only, never text (drawn by SPG-14) |

Values loaded from a scene file are clamped and written back, so an old or
hand-edited file cannot switch a protection off: a lookahead or hold below the
floor is raised to it, and an unknown sensitivity or fallback name becomes the
default. The ceilings are safe values that bound VRAM (lookahead) and keep the
sliders usable. If SPG-08b sets spg-core floors above these, the plugin floors
must rise to match; until then a config spg-core rejects fails closed.

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
- The masked field `watchlist_new_entry` is moved into the file by Add and is
  erased from the settings in the filter's `save` callback, which OBS calls
  just before writing the scene collection, so an entry typed but never added
  is not saved there either. OBS hands that callback the live settings, so a
  scene collection save (including an autosave) while the user is still
  typing clears the field: the entry has to be typed again. That is the
  intended trade: an unsent entry is dropped rather than written to disk.
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

Not covered: the entries are in process memory (crash dumps are a process-level
measure the plugin does not take yet), in OBS's in-memory undo stack if a user
typed into the masked field, and in watch-list files of deleted filters, which
stay on disk until removed by hand.
