# Text policy

How recognized text becomes a verdict (SPG-08, [plan section 6](plan.md#6-detection-and-decision)).
The API is `core/include/spg/policy.hpp`; the code is `core/src/policy/`. A
`PolicyEngine` is built from a `PolicyConfig` and returns a `Verdict` of reason
codes (`WATCHLIST`, `PII_EMAIL`, `PII_PHONE`, `PII_CARD`, `PII_IP`,
`PII_ADDRESS`, `PII_TOKEN`) limited to the enabled categories. It never returns
the matched text, and `Face` is never a text reason.

The rules favour recall: when a rule had to choose, it chose to blur. The
numeric defaults are placeholders until SPG-12 tunes them against the eval.

## Folding

Every check first folds the text:

- Compatibility forms, case and diacritics: each character in the blocks listed
  in `core/src/policy/make_fold_table.py` maps to `casefold(NFKC(c))` with
  combining marks removed, so fullwidth, circled and mathematical-styled
  letters, ligatures and accented letters fold to plain lower case (`ＪＯＳÉ`,
  `𝐉𝐨𝐬𝐞` and `jose` agree). `æ`, `œ`, `ø`, `ð`, `þ`, `ł`, `đ`, `ħ`, `ı`, `ŧ`
  are spelled with base letters. The table is generated (`fold_table.inc`); it
  is not full NFKC, and characters outside those blocks pass through.
- Zero-width characters (U+200B..U+200D, U+2060, U+FEFF) and soft hyphens are
  deleted; control characters and Unicode whitespace become one space.
- A malformed UTF-8 byte becomes U+FFFD, which separates words. No text makes
  evaluation throw; only running out of protected memory does (see
  [Secrecy](#secrecy)).

## Watch-list matching

Entries and text are compared in **matching form**: the folded text with OCR
confusions merged (`0`→`o`; `1`, `i`, `|`→`l`; `5`→`s`; `8`→`b`) and every run
of punctuation, symbols or whitespace turned into one space. Outside ASCII,
punctuation and symbols are the Unicode general categories P, S and Z of any
script (`separator_table.inc`, generated with the fold table), so an Arabic
comma or a CJK bracket ends a word just as `,` does. `normalize_for_matching()`
returns this form. An entry whose matching form is empty (punctuation only) is
rejected when the engine is built, since it could never match.

For an entry of `n` characters in matching form, `k(n) = min(max_edits,
floor(max_edit_ratio * n))` edits (insertions, deletions, substitutions) are
allowed; merged confusions cost nothing. With `p = min_partial_chars`:

| Entry | Matches when |
| --- | --- |
| `n < p` | The entry equals a run of whole words in the text within `k(n)` edits ("vex" matches "the vex" but not "convex" or "vexing"). |
| `n >= p` | The whole entry is within `k(n)` edits of any substring, including one inside a longer word ("xxzyxaquorbelxx"). |
| `n > p`, partial reveal | A prefix of `m >= p` characters is within `k(m)` edits of text that starts at a word start ("4821 qx mapl" for "4821 Qx Maple Synthway"), or a suffix of `m >= p` characters is within `k(m)` edits of text that ends at a word end. |

Each check is the approximate substring recurrence (Sellers) over the entry's
prefixes, O(entry length × text length) per entry, stopping early once every
prefix is over budget. With the defaults (2, 0.2, 6) a six-character partial
allows one edit; that is the main source of false positives and the first
thing SPG-12 should measure.

## PII validators

PII checks read the folded text with punctuation kept. Where a digit is
expected, `o` reads as `0` and `l`, `i`, `|` as `1` next to a real digit.

| Reason | Rule |
| --- | --- |
| `PII_PHONE` | Digit groups joined by at most three of ` -.()+`. Every window of whole consecutive groups is tried. NANP: 10 digits (or 11 after a leading `1`) with area code and exchange starting 2-9, and group breaks only after the `1`, the area code or the exchange (so a date and time is not a phone). E.164: `+` (up to three spaces or `(` may sit between it and the first digit, as in `+ (44)`) then 8 to 15 digits, not starting with 0. |
| `PII_CARD` | A window of the same digit groups holding 13 to 19 digits that passes the Luhn check. No issuer prefix check. |
| `PII_EMAIL` | `local@domain.tld`, allowing one OCR space on each side of `@`; the domain needs a dot, no empty label and a final label of at least two non-digits. |
| `PII_IP` | IPv4: four consecutive dot-separated octets (0-255), in the digit view or in the text as read (a `\|` next to an address reads as `1` in the digit view). An IPv6 address whose `::` OCR read as one `:` counts too: 4 to 7 hex groups, no `::`, at least two of them 3 or more characters long (not a time or a MAC address). IPv6: eight hex groups, or at least two groups around a single `::`, optionally ending in an IPv4 tail. A label glued to the address (`ipv6:`, whose `6` is a hex digit) is tried without its first group. |
| `PII_ADDRESS` | A house number (1-6 digits, optional letter), one to four words with a letter, then a street suffix from the list in `pii.cpp` (`street`, `st`, `avenue`, `rd`, `ln`, ...); or `po box N` / `p o box N`. No postcode rules. |
| `PII_TOKEN` | A known prefix at a word start followed by enough `[a-z0-9_-]` characters: Twitch stream keys (`live_`) and `oauth:` tokens, GitHub (`ghp_`, `github_pat_`, ...), GitLab, Slack (`xox?-`), Stripe, `sk-` keys, AWS key ids (`akia`, `asia`), Google API keys (`aiza`), npm, Hugging Face, JWTs (`eyj`). A JWT whose parts are each shorter also counts: a header of at least 18 characters starting `eyj`, a dot, and a payload of at least 3. Also a PEM `-----BEGIN ... PRIVATE KEY` header. OCR misreads (SPG-12): prefixes and bodies are read with every `\|` as `l` (an OCR reading of `l` or `I`), while whether a prefix starts a word is decided on the text as read, where `\|` separates (`key\|ghp_...`), so a match on the text as read is never lost; the body may continue past up to two single spaces between runs of at least 6 characters when the joined body holds a digit, so a stray OCR space does not cut a key in two while words after a prefix-like word (`live_streaming starting tomorrow`) do not count. A JWT never joins across a space. |

## Secrecy

Watch-list entries, their matching forms and recognized text are `Secret`s or
private engine state. The engine's `LogSink` receives counts, indices and
reason codes only; `ContractError` messages name the entry index.
`tests/unit/policy_sentinel_test.cpp` runs a synthetic sentinel entry through
construction, matching, PII scanning, `describe()` and three config errors
while capturing the sink and `std::cout`/`std::cerr`/`std::clog`, and asserts
that no captured text contains any fragment of the entry, raw or normalized.

`Secret` is a type-level guard (no stream operator, no implicit conversion)
and, since SPG-15b, protected memory: its text lives in `SecureHeap`, which
keeps it out of crash dumps where the OS allows, locks it in RAM where it can
and zeroes it on release. The compiled matching forms, the folded and matching
copies `evaluate()` makes of recognized text and the PII digit views use the
same heap through `SecureAllocator`. What each OS covers, and the copies that
remain outside it (short-string buffers on the stack, OBS and Qt copies of the
typed entry), are in [settings.md](settings.md#watch-list-text-in-memory).
Recognized text is covered from decoding to the verdict; see
[Recognized text in memory](#recognized-text-in-memory).

`evaluate()` costs O(entries × entry length × text length) per call, with up to
three passes per entry, and has no length cap of its own. It is meant for a
worker thread; the OCR stage that calls it should bound the text it passes in.
Its temporaries for a line of up to 4096 code points reuse blocks of the
shared heap, so after warm-up a call makes no system call and holds the heap's
lock only to take and return blocks; a longer line maps a region of its own
per temporary (outside the lock). Measured on the dev Mac (M-series, 10
synthetic entries, `evaluate()` in a tight loop): no change single-threaded
(about 26 µs for a 170-character line, 440 µs for 3000 characters); with four
threads evaluating at once, a 170-character line went from about 28 to 33 µs
because the threads share the heap's lock. The scheduler ticket should
measure this against the frame budget; a per-class lock is the next step if
it matters.

**Requirement on callers (SPG-10 scheduler, SPG-07 OCR stage):** the
`PolicyEngine` constructor and `evaluate()` throw `std::bad_alloc` when
`SecureHeap` cannot map a region (`mmap` or `VirtualAlloc` failed, for
example at `vm.max_map_count`). `mlock` and dump exclusion are best effort;
mapping is not. The caller must catch it and treat the frame as not scanned,
so the configured fallback applies (default: blur the whole source). An
exception from the policy stage must never let an unscanned frame through.

## Recognized text in memory

OCR output can hold the very secrets the filter hides, so since SPG-15d it
gets the same protection as watch-list entries (`SecureHeap`: kept out of
crash dumps where the OS allows, locked in RAM where it can, zeroed on
release) from the moment it is decoded until the verdict:

| Copy | Where |
| --- | --- |
| Text of one read job, as CTC decoding appends it | `ocr::SecureText`, a `std::vector<char, SecureAllocator<char>>`: every byte is in the heap block (a vector has no short-string buffer). A decode error zeroes what it appended |
| A region's views joined by spaces and newlines | `ocr::SecureText` |
| `Recognition::text`, `CtcResult::text`, the recognition cache's entries, `TextHit::text`, what `spg::TextRecognizer::recognize()` returns to `run_scan()` | `Secret` |
| The model's output probabilities (`runtime::Tensor::data`; the most probable class per step spells the text) | Ordinary memory, zeroed once the batch is decoded or fails |
| Folded, matching and reversed forms in `PolicyEngine::evaluate()` | `SecureU32String` |
| Recognition cache fingerprints (crop pixels) | Ordinary memory, zeroed before release: `Fingerprint` zeroes its pixels when destroyed or assigned over, and the cache zeroes each one when the entry is replaced, erased, expires, is evicted or refused, and when the cache is destroyed. The same holds for the content-keyed cache in front of the scan stage (`ocr::CachedRecognizer`, SPG-16e), which is a `RecognitionCache` filed by a hash of each crop |

The scan path adds one copy, also a `Secret`: `ocr::CachedRecognizer` (the
recognizer stage in `spg-cli` and the plugin) keeps each text it read in its
cache and reuses it for at most `max_age` (1 s) of frame time. Expired
entries (text and zeroed crop pixels) are dropped on the next recognition
call or when the cache is destroyed (the filter's stages are rebuilt or the
filter goes away); a cycle with no text region makes no call, so they can
stay in memory longer than `max_age` while nothing on screen is read. `run_scan()`
(`core/src/schedule/scan.cpp`) gets a `std::vector<Secret>` from the
recognizer stage, compares each one's size
with `max_text_bytes` and passes it to the policy by reference;
`recognize_tracks()` moves each `Secret` into its `TextHit`. Neither logs nor
keeps text. The tests prove it with the heap's wipe observer: a decode buffer
is a block of the process heap and is all zero when released, a `recognize()`
call leaves exactly one live block per region (its `Secret`) after zeroing at
least two per region (job buffer and joined buffer), and every way a
fingerprint leaves the cache passes through a wipe the cache's own observer
sees as all zero. `tests/integration/text_recognizer_test.cpp` still reads a
synthetic sentinel through the cache and the policy while capturing the
standard streams, and checks no output or error holds any of it.

What is not covered:

- **ONNX Runtime's buffers.** The model writes its output into ONNX
  Runtime's own memory (its CPU arena) before `Session::run()` copies it
  out. That memory is not ours to zero or exclude.
- **Pixels.** The readback frame (`OwnedFrame`, and OBS's own copies), the
  crop images and float input tensor the recognizer builds, the detector's
  tensors and OpenCV temporaries are ordinary memory, freed without zeroing.
  They are pictures of the text, not text; the frame is the largest copy and
  lives in OBS and the lookahead buffer either way. Only cache fingerprints,
  which outlive a cycle by up to `max_age`, are zeroed.
- **Fingerprints are not in `SecureHeap`.** The cache holds up to 64 MiB of
  them, and most crops are over the heap's 16 KiB size classes, so each would
  be a region of its own: that would use up `RLIMIT_MEMLOCK` (often 8 MiB on
  Linux), so later watch-list regions could not be locked, and on Windows the
  512 Windows Error Reporting exclusions per process, so later watch-list
  regions could not be kept out of dumps.
  Residual risk: a fingerprint is a picture of on-screen text, possibly a
  watch-listed string, and while it is cached (up to `max_age`, default
  1 s) it can appear in a crash dump or swap. The transient crops
  `fingerprint()` and the recognizer's read jobs render (including buffers
  a growing vector frees) are not zeroed at all. "Wiped, dump-excluded
  memory" in SPG-15d holds for recognized text, not for the pixels that
  show it. A bounded pixel arena in `SecureHeap` or a shorter default
  `max_age` would narrow this; neither is part of SPG-15d.
- **Very short text in the policy.** `evaluate()`'s `SecureU32String`
  temporaries keep up to 3 or 4 code points (by standard library) inside the
  string object, which is on the stack (see
  [settings.md](settings.md#what-is-not-covered), "Short strings"). A
  recognized line that short sits there while it is matched.
- The platform limits of `SecureHeap` itself (macOS has no dump exclusion,
  hibernation files, debuggers) are in
  [settings.md](settings.md#platform-limits).
