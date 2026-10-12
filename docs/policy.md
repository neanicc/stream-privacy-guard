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
| `PII_IP` | IPv4: four consecutive dot-separated octets (0-255). IPv6: eight hex groups, or at least two groups around a single `::`, optionally ending in an IPv4 tail. A label glued to the address (`ipv6:`, whose `6` is a hex digit) is tried without its first group. |
| `PII_ADDRESS` | A house number (1-6 digits, optional letter), one to four words with a letter, then a street suffix from the list in `pii.cpp` (`street`, `st`, `avenue`, `rd`, `ln`, ...); or `po box N` / `p o box N`. No postcode rules. |
| `PII_TOKEN` | A known prefix at a word start followed by enough `[a-z0-9_-]` characters: Twitch stream keys (`live_`) and `oauth:` tokens, GitHub (`ghp_`, `github_pat_`, ...), GitLab, Slack (`xox?-`), Stripe, `sk-` keys, AWS key ids (`akia`, `asia`), Google API keys (`aiza`), npm, Hugging Face, JWTs (`eyj`). A JWT whose parts are each shorter also counts: a header of at least 18 characters starting `eyj`, a dot, and a payload of at least 3. Also a PEM `-----BEGIN ... PRIVATE KEY` header. |

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

`evaluate()` costs O(entries × entry length × text length) per call, with up to
three passes per entry, and has no length cap of its own. It is meant for a
worker thread; the OCR stage that calls it should bound the text it passes in.
Its temporaries for a line of up to 4096 code points reuse blocks of the
shared heap, so after warm-up a call makes no system call and holds the heap's
lock only to take and return blocks; a longer line maps a region of its own
per temporary (outside the lock).

**Requirement on callers (SPG-10 scheduler, SPG-07 OCR stage):** the
`PolicyEngine` constructor and `evaluate()` throw `std::bad_alloc` when
`SecureHeap` cannot map a region (`mmap` or `VirtualAlloc` failed, for
example at `vm.max_map_count`). `mlock` and dump exclusion are best effort;
mapping is not. The caller must catch it and treat the frame as not scanned,
so the configured fallback applies (default: blur the whole source). An
exception from the policy stage must never let an unscanned frame through.
