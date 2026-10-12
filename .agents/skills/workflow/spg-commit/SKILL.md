---
name: spg-commit
description: Stage focused Stream Privacy Guard changes and create Conventional Commits; push the task branch when requested or when part of PR delivery or autopilot. Use for commit, commit and push, or committing a completed delivery task.
---

# Commit Stream Privacy Guard changes

Follow [AGENTS.md](../../../../AGENTS.md). A commit-only request stays local; an
explicit commit-and-push, PR delivery or autopilot run includes pushing the task
branch. Do not expand this task into review, cleanup, a PR or a merge.

1. Inspect status, unstaged and staged diffs, and relevant untracked files.
   Preserve unrelated work and already-staged changes; isolate only this task.
2. Resolve the branch. New agent work uses `agent/spg-<nn>-<slug>` from `main`.
   Never commit on `main`. If HEAD is unborn, inspect remote refs before the
   first commit instead of creating an unrelated root.
3. Reuse validation for unchanged inputs; run missing focused checks from
   [validation routes](../../../references/validation.md). Do not claim a check
   ran if the stage only printed a "not landed" notice.
4. Group related changes into coherent commits. Stage explicit paths and inspect
   `git diff --cached`. The commit must not contain unrelated content, build
   output (`build/`, `eval-out/`), model weights, datasets, recordings, frames of
   real people, watch-list examples that look like real personal data, or secrets.
   A feature, its tests and its docs belong together.
5. Message: `type(scope): imperative summary`, under 72 characters, no final
   period. Scopes follow lanes and modules, for example
   `feat(policy): fuzzy-match watch-list token windows`,
   `fix(schedule): fail closed when a cycle misses its deadline`,
   `chore(agents): add spg delivery skills`. Explain non-obvious reasons in the
   body; for multiline messages write a temporary file and use `git commit -F`.
6. Honor hooks and configured authorship. Fix an in-scope hook failure and retry;
   never bypass a hook with `--no-verify`. Do not amend a pushed commit.
7. When pushing is authorized, push the task branch with upstream set. Resolve a
   non-fast-forward from evidence (fetch, inspect, merge); never force-push.
   Verify the remote branch SHA matches the local commit after the push.

Report the commit hash, pushed or local state, checks, and remaining worktree
changes. A leftover unrelated change is not a reason to commit or discard it.
