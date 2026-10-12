"""Synthetic sensitive strings, decoys and filler text. Nothing here is real data.

Names and streets are built from invented syllables. Emails use the reserved
example.com/.net/.org domains, phones the fictional 555-01xx range, cards a
test BIN with a valid Luhn digit, IPs the documentation ranges (RFC 5737 and
RFC 3849). Phones are NANP with 3-3-4 groups and an area code and exchange
starting 2-9, or unspaced E.164; cards are 16 digits with a valid Luhn digit,
matching the SPG-08 validators. Decoys are near misses a correct policy must
not blur: watch-list look-alikes whose every token differs in its first letter
and in at least 40% of its characters (beyond the default fuzzy limits in
docs/contracts.md), Luhn-invalid card numbers, phones with an area code
starting 0 or 1, dotted version strings with octets above 255, handles without
a domain, token prefixes with too few characters, dates and times.
"""
from __future__ import annotations

import math
import random
import string

_ONSETS = ("b", "br", "d", "dr", "f", "g", "gr", "h", "k", "l", "m", "n", "p", "qu", "r", "s", "t", "tr", "v", "z")
_VOWELS = ("a", "e", "i", "o", "u", "ae", "ei", "ou")
_CODAS = ("", "l", "n", "r", "s", "th", "ck", "ll", "nd", "rn", "sk", "x")
_STREETS = ("Street", "Avenue", "Road", "Lane", "Drive", "Court", "Way", "Terrace")
_WORDS = ("settings", "profile", "welcome", "back", "online", "stream", "starting", "soon", "loading", "level",
          "score", "match", "ready", "queue", "friends", "party", "invite", "chat", "update", "available",
          "download", "complete", "battery", "volume", "scene", "camera", "mic", "muted", "live", "viewers",
          "follow", "thanks", "nice", "play", "round", "victory", "inventory", "map", "quest", "today",
          "meeting", "notes", "draft", "saved", "search", "results", "home", "news", "weather", "sunny")
_USERS = ("pixelfox", "nightowl", "tankmain", "lurker42", "modbot", "gg_ez", "chillcat", "speedrunr")

PII_KINDS = ("pii:email", "pii:phone", "pii:card", "pii:ip", "pii:address", "pii:token")


def _word(rng: random.Random, syllables: int) -> str:
    return "".join(rng.choice(_ONSETS) + rng.choice(_VOWELS) + rng.choice(_CODAS)
                   for _ in range(syllables)).capitalize()


def person(rng: random.Random) -> tuple[str, str]:
    return _word(rng, rng.choice((1, 2))), _word(rng, 2)


def street_address(rng: random.Random) -> str:
    return f"{rng.randint(10, 9899)} {_word(rng, 2)} {rng.choice(_STREETS)}"


def watchlist(rng: random.Random) -> list[str]:
    """Four synthetic entries: two full names, a street address, a handle."""
    a, b = person(rng), person(rng)
    return [f"{a[0]} {a[1]}", f"{b[0]} {b[1]}", street_address(rng), f"{a[0]}{a[1]}{rng.randint(10, 99)}"]


def luhn_digit(digits: str) -> str:
    total = 0
    for i, ch in enumerate(reversed(digits)):
        d = int(ch) * (2 if i % 2 == 0 else 1)
        total += d - 9 if d > 9 else d
    return str((10 - total % 10) % 10)


def card(rng: random.Random, valid: bool = True) -> str:
    body = "400000" + "".join(rng.choice(string.digits) for _ in range(9))
    check = luhn_digit(body)
    if not valid:
        check = str((int(check) + rng.randint(1, 9)) % 10)
    digits = body + check
    return " ".join(digits[i:i + 4] for i in range(0, 16, 4))


def pii(rng: random.Random, kind: str) -> str:
    if kind == "pii:email":
        first, last = person(rng)
        return f"{first.lower()}.{last.lower()}{rng.randint(1, 99)}@example.{rng.choice(('com', 'net', 'org'))}"
    if kind == "pii:phone":
        # NANP 3-3-4 groups with area code and exchange starting 2-9, or E.164.
        area, line = rng.randint(201, 989), f"01{rng.randint(0, 99):02d}"
        return rng.choice((f"({area}) 555-{line}", f"+1{area}555{line}", f"{area}-555-{line}"))
    if kind == "pii:card":
        return card(rng)
    if kind == "pii:ip":
        if rng.random() < 0.25:
            return f"2001:db8:{rng.randint(1, 0xffff):x}::{rng.randint(1, 0xffff):x}"
        return f"{rng.choice(('192.0.2', '198.51.100', '203.0.113'))}.{rng.randint(1, 254)}"
    if kind == "pii:address":
        return street_address(rng)
    if kind == "pii:token":
        alnum = string.ascii_letters + string.digits
        return rng.choice((
            "ghp_" + "".join(rng.choice(alnum) for _ in range(36)),
            "AKIA" + "".join(rng.choice(string.ascii_uppercase + string.digits) for _ in range(16)),
            "sk_live_" + "".join(rng.choice(alnum) for _ in range(24))))
    raise ValueError(f"unknown PII kind {kind!r}")


def _perturb(rng: random.Random, token: str) -> str:
    letters = [i for i, c in enumerate(token) if c.isalnum()]
    if not letters:
        return token
    count = max(2, math.ceil(0.4 * len(letters)))
    picks = {letters[0], *rng.sample(letters, min(count, len(letters)))}
    out = list(token)
    for i in picks:
        pool = string.digits if out[i].isdigit() else string.ascii_lowercase
        new = rng.choice([c for c in pool if c != out[i].lower()])
        out[i] = new.upper() if out[i].isupper() else new
    return "".join(out)


def edit_distance(a: str, b: str) -> int:
    row = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        prev, row[0] = row[0], i
        for j, cb in enumerate(b, 1):
            prev, row[j] = row[j], min(row[j] + 1, row[j - 1] + 1, prev + (ca != cb))
    return row[-1]


def far_from(text: str, entries: list[str]) -> bool:
    """Beyond the default fuzzy limit of every entry, with no 5-character token prefix in common."""
    a = text.lower()
    prefixes = {t[:5] for e in entries for t in e.lower().split() if len(t) >= 5}
    return (all(edit_distance(a, e.lower()) > min(2, int(0.2 * len(e))) for e in entries)
            and not any(t[:5] in prefixes for t in a.split() if len(t) >= 5))


def lookalike(rng: random.Random, entries: list[str]) -> str:
    """A string shaped like a watch-list entry that a correct policy must not match."""
    while True:
        text = " ".join(_perturb(rng, t) for t in rng.choice(entries).split())
        if far_from(text, entries):
            return text


def decoy(rng: random.Random, entries: list[str]) -> str:
    choice = rng.randrange(8)
    if choice == 6:
        return f"{rng.randint(2020, 2029)}-{rng.randint(1, 12):02d}-{rng.randint(1, 28):02d}"
    if choice == 7:
        return f"{rng.randint(0, 23):02d}:{rng.randint(0, 59):02d}:{rng.randint(0, 59):02d}"
    if choice == 0:
        return lookalike(rng, entries)
    if choice == 1:
        return card(rng, valid=False)
    if choice == 2:
        return f"({rng.choice('01')}{rng.randint(10, 99)}) 555-01{rng.randint(0, 99):02d}"
    if choice == 3:
        return f"v{rng.randint(2, 9)}.{rng.randint(256, 999)}.{rng.randint(300, 999)}.{rng.randint(1, 9)}"
    if choice == 4:
        return "@" + _word(rng, 2).lower() + _word(rng, 1).lower()
    return "ghp_" + "".join(rng.choice(string.ascii_letters) for _ in range(rng.randint(4, 9)))


def filler(rng: random.Random, low: int = 2, high: int = 6) -> str:
    return " ".join(rng.choice(_WORDS) for _ in range(rng.randint(low, high))).capitalize()


def username(rng: random.Random) -> str:
    return rng.choice(_USERS)
