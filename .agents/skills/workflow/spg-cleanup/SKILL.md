---
name: spg-cleanup
description: Simplify an implemented Stream Privacy Guard change or fix concrete review findings while preserving fail-closed behavior, timing semantics and privacy rules. Use for cleanup, simplify, polish, or make the diff smaller.
---

# Clean up a Stream Privacy Guard change

Read [AGENTS.md](../../../../AGENTS.md), the complete task diff, and immediate
consumers and tests where needed. Scope the pass to this change and preserve
unrelated work. A cleanup request authorizes edits, not commits or publication by
itself (autopilot and babysit runs carry their own authority).

1. Identify intended behavior and invariants before editing. Refer to plan
   sections 4–8 for timing, contracts, detection and OBS rules.
2. Remove dead code, debug dumps, stale comments, unused dependencies, duplicated
   conversions and pass-through layers. Prefer clear module boundaries over a new
   framework or general utility.
3. Preserve fail-closed fallbacks, retroactive masking, hold and margin, bounded
   queues, deadline checks, coordinate-space tags, injected clocks and the
   secrecy of watch-list text. Do not simplify away error paths that fail closed.
4. Keep `core/` free of OBS and wall-clock reads. Reuse buffers on hot paths only
   where allocation cost is shown; do not turn cleanup into speculative tuning.
   Do not reformat another lane's files or update pinned dependencies incidentally.
5. Fix supported, in-scope review findings. Do not add product behavior only
   because a reviewer suggested it; open a follow-up issue instead. When a
   contract must change, update consumers, fixtures and `docs/contracts.md` together.
6. Run focused [checks](../../../references/validation.md) for the changed
   behavior, including `scripts/ci/run.sh eval` when pipeline behavior could move.
   Reuse passing evidence for unchanged inputs.

Report the simplifications and fixes, checks with results, and remaining limits.
