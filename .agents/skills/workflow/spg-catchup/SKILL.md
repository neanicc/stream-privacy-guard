---
name: spg-catchup
description: Bring a Stream Privacy Guard task branch up to date with main or its PR base, resolving merge conflicts while preserving contracts, timing semantics and both sides' tests. Use for catch up, sync branch, or resolve conflicts.
---

# Catch up a Stream Privacy Guard branch

Follow [AGENTS.md](../../../../AGENTS.md). Inspect status, the current branch, its
base, and any merge or rebase already in progress. Use the PR's base or the
user's explicit ref; otherwise `main`. Fetch it.

- Preserve unrelated dirty or staged files. Prefer an isolated worktree when
  needed; do not blindly stash, reset or overwrite work.
- If the base is already an ancestor of HEAD, there is nothing to do.
- Merge by default for pushed branches. Rebase only when requested; rewriting a
  published branch needs explicit authorization. Never force-push or discard a
  whole side without reading it.
- For each conflict, read both intents and nearby consumers and tests. Resolve
  routine conflicts directly. Ask only for a real product or contract choice.
- Watch especially for: contract header changes in `core/include/spg/`, time
  units and clock injection, fallback semantics, hold and margin defaults,
  coordinate-space tags, CMake target lists, pinned versions and hashes in
  `deps/` and `models/` manifests. Keep both sides' regression tests.
- `docs/agent-log.md` conflicts: keep both entries, in merge order.
- Do not hand-merge two different pinned versions of one dependency; pick the one
  `main` has unless the branch's ticket is the upgrade, and re-verify the hash.
- Inspect the complete resulting diff, including automatic merges. Run the
  relevant [checks](../../../references/validation.md), stage the resolved files
  explicitly and finish the merge. Honor hooks.

Push only when requested or part of babysit, PR delivery or autopilot. Report the
base, resulting commit, validation and whether it was pushed. If blocked
mid-operation, state the exact state and the decision needed; never leave an
unfinished merge silently or abort someone else's operation.
