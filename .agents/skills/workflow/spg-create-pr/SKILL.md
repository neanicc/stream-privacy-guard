---
name: spg-create-pr
description: Create or update a Stream Privacy Guard GitHub PR for one ticket with a Conventional Commits title, Closes link, plan context, and truthful automated, eval, live and human evidence. Also supports drafting PR text without publishing.
---

# Create a Stream Privacy Guard pull request

Use [AGENTS.md](../../../../AGENTS.md) and the
[PR template](../../../../.github/pull_request_template.md). If asked only for
title and body text, prepare it without committing, pushing or creating a PR.

## Prepare

1. Inspect status, remote, branch, any existing PR for the branch, and the base
   (`main` unless the user or an existing PR says otherwise). Fetch the base and
   review the complete branch diff and every commit since the merge base.
2. The head is an `agent/spg-<nn>-<slug>` branch, never `main`. Keep an existing
   suitable branch. Preserve unrelated uncommitted changes.
3. Bootstrap edge: a PR needs a remote base with shared history. If the remote is
   empty, explain the bootstrap step instead of inventing a base or force-pushing.
4. Review all changes from the merge base. Check the invariants the diff touches:
   fail-closed behavior, retroactive masking, temporal hold, render-thread rules,
   coordinate spaces, watch-list secrecy, pinned artifacts.
5. Run the applicable [validation](../../../references/validation.md). Code PRs
   need build and tests; pipeline PRs also need `scripts/ci/run.sh eval` with
   metrics compared to `main`. Fix in-scope failures; disclose unavailable checks.
6. If the diff touches a human-owned path (`eval/thresholds.toml`, `.github/**`,
   `.claude/**`, `.agents/**`, `.greptile/**`, `scripts/agent/**`,
   `scripts/bootstrap/**`, `AGENTS.md`, `CLAUDE.md`, `LICENSE`), open the PR anyway,
   add the `needs-human` label and state in Notes & Risks exactly what a human must review.

## Describe and publish

- One ticket per PR. Title: whole-branch Conventional Commits summary under 72
  characters. Body: fill Summary, `Closes #<issue>`, Context (ticket, plan
  section, lane), Changes, Validation, Notes & Risks.
- Validation lists only results actually produced for this head, split into
  automated, synthetic eval (metric, value, threshold, delta vs `main`), live OBS
  and human leak test. Write "not tested" where true. No performance claims
  without a `bench` run on named hardware.
- Commit and push through [spg-commit](../spg-commit/SKILL.md). Write the body to a
  temporary UTF-8 file and run
  `gh pr create --base main --head <branch> --title <title> --body-file <file>`.
  Use `--draft` only when requested or when the change is knowingly incomplete.
  Update an existing PR for the branch instead of opening a duplicate.
- Verify the returned URL, base, head and remote head SHA.

Return the PR link, validation summary and remaining gaps. Do not merge, request
reviewers, or post review comments just because a PR was opened.
