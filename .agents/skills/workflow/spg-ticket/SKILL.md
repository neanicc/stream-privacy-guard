---
name: spg-ticket
description: Implement one Stream Privacy Guard ticket end to end in an isolated worktree - read the issue and plan, build the smallest complete change with tests, validate, commit, push, and open a PR that closes the issue. Use for implement SPG-xx, take this issue, or when the autopilot dispatches a ticket.
---

# Deliver one ticket

Inputs: the ticket ID (`SPG-xx`), its issue number, and anything the dispatcher
passed (lane, dependencies, done-when list, notes from an earlier attempt). This
skill authorizes branch creation, commits, pushing the task branch and opening
one PR. It does not authorize merging, editing human-owned paths, or changing
other lanes' files beyond a minimal integration point.

## 1. Orient

- Confirm you are in your own worktree, not the main checkout. Fetch and branch
  from the current remote main: `git fetch origin` then
  `git switch -c agent/spg-<nn>-<slug> origin/main`. If that branch already exists
  on the remote from an earlier attempt, check it out and continue from it.
- Read the issue (`gh issue view <n>`), the ticket row and related sections of
  [plan.md](../../../../docs/plan.md), the invariants in
  [AGENTS.md](../../../../AGENTS.md), your lane in
  [team-plan.md](../../../../docs/team-plan.md),
  [validation routes](../../../references/validation.md), the last few
  `docs/agent-log.md` entries, and the code this ticket builds on.
- Write the done-when items as your checklist.

## 2. Design small

- Build the smallest change that meets every done-when item, in the planned
  layout. No new frameworks, services or runtime network access.
- For an open question in plan section 12, take the recommended default and
  record the decision in the agent log. Do not ask; nobody may be watching.
- Check APIs against official sources (OBS docs and `obs-plugintemplate`, ONNX
  Runtime docs, model cards) with WebFetch rather than guessing signatures.
- If the ticket is too large for one reviewable PR (roughly 800 changed lines of
  non-generated code, or independent parts), deliver the first coherent slice and
  propose the split in your report.
- If the work requires a human-owned path (`.github/**`, `.claude/**`, `.agents/**`,
  `eval/thresholds.toml`, `scripts/agent/**`, `AGENTS.md` and the others listed in
  AGENTS.md), stop and report it. Do not edit it. Code adapted from
  `obs-plugintemplate` keeps its build and packaging scripts under `build-aux/` or
  `scripts/`, never `.github/`; CI already calls `scripts/ci/run.sh` stages,
  including `package` (which runs `scripts/package.sh`).

## 3. Build with tests

- Write tests that exercise behavior through public interfaces with synthetic
  inputs and an injected clock. Fixtures stay small; commit the generator or seed
  rather than large binaries.
- Every downloaded artifact goes in a manifest with URL, version, SHA-256 and
  license, fetched to `$SPG_CACHE_DIR` by a script that verifies the hash.
- Keep the invariants: fail closed, retroactive masking, hold and margin, render
  thread never waits on inference, explicit coordinate spaces, watch-list text
  never logged.

## 4. Validate

- Focused checks first, then `scripts/ci/run.sh build` and `test`.
- If pipeline behavior can change, run `scripts/ci/run.sh eval` and compare with
  `main` (run it on `origin/main` in a temporary worktree, or download the latest
  `eval-report` artifact from main's Check run). Record metric values and deltas.
- Plugin work: build on this Mac; CI covers the other operating systems. Loading
  in OBS is live evidence and needs a human.

## 5. Deliver

- Append an entry to `docs/agent-log.md` for decisions or measurements.
- Commit and push through [spg-commit](../spg-commit/SKILL.md); open the PR through
  [spg-create-pr](../spg-create-pr/SKILL.md) with `Closes #<issue>`.
- If you are blocked after a real attempt, push the branch anyway, open a
  **draft** PR that states exactly what is missing, and report it under BLOCKERS.
- If you are continuing an earlier draft PR and the work is now complete, mark it
  ready with `gh pr ready <pr>` so reviews start.
- Release the branch: `git switch --detach`.

## 6. Report

End with this block so the conductor can parse it:

```
TICKET: SPG-xx   ISSUE: #n   PR: <url or none>   HEAD: <sha>
DONE-WHEN: <item> - done/not done - evidence (one line each)
CHECKS: <command> -> <result> (one line each; "not run" where true)
EVAL: <metric> <value> (delta vs main) ... | not affected
DECISIONS: <decision and reason> | none
FOLLOW-UPS: <proposed issue title> [lane] - why | none
BLOCKERS: none | <exact description and what would unblock it>
```

Never report a check that did not run, or a done-when item without evidence.
