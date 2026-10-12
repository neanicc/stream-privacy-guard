---
name: spg-signoff
description: Verify and repair Stream Privacy Guard's applicable readiness checks for the current revision, separating code and synthetic-eval readiness from live OBS and human leak-test acceptance. Use for signoff, final verification, or checking readiness after PR fixes.
---

# Sign off a Stream Privacy Guard revision

Read [AGENTS.md](../../../../AGENTS.md) and
[validation routes](../../../references/validation.md). Signoff establishes
evidence for the requested scope. It does not start a staff review, publish
statuses or merge.

## Verify and repair

1. Record status, branch, HEAD and any task or unrelated dirty files. Identify
   whether the scope is docs, core, eval, plugin, build, or whole-project
   acceptance. Read `scripts/ci/run.sh` and CI before naming the gate.
2. Docs and skills: `python3 scripts/check_agent_files.py` and `git diff --check`.
   Code: `scripts/ci/run.sh setup`, `build`, `test`. Pipeline-affecting code:
   also `scripts/ci/run.sh eval`, comparing metrics with `main`. Acceptance:
   `scripts/ci/run.sh eval --local` on the dev Mac. A stage that printed a
   "not landed" notice did not verify anything; say so.
3. Read every failure, repair in-scope causes, run focused checks for each
   repair. Never weaken tests, tolerances, scoring, thresholds or CI stages. A
   product decision or missing external prerequisite is a concrete blocker.
4. Rerun the applicable final gate after changes. Stop on a repeated identical
   infrastructure failure with no new local action available; report the command,
   evidence and missing prerequisite.
5. When commit and push are part of the request, use
   [spg-commit](../spg-commit/SKILL.md), verify the final SHA and upstream, and
   attribute only evidence valid for that SHA. Otherwise label results as
   worktree verification.

## Readiness report

State scope (commit or worktree), commands and results, fixes, and unresolved
issues. Local success does not mean hosted CI passed. A later relevant change
invalidates affected evidence.

For live or release signoff, require the [live check](../../../../docs/live-check.md)
record and a zero-leak `spg-cli scrub` result. When absent, state
**code and synthetic eval verified; live OBS and leak test not done**. Never
fabricate device observations.
