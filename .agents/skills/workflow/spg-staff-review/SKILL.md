---
name: spg-staff-review
description: Review a Stream Privacy Guard branch or PR at staff level with an evidence-based weighted score and actionable findings for leak safety, real-time behavior, privacy, OBS lifecycle and architecture. Read-only unless fixes or posting are separately requested. Runs on every PR in CI; only that CI run counts for the merge gate.
---

# Staff review for Stream Privacy Guard

Judge the changed behavior against [AGENTS.md](../../../../AGENTS.md), the
relevant sections of [plan.md](../../../../docs/plan.md) and the ticket the PR
closes. Be direct and specific. Optimize for a small, trustworthy safety net, not
speculative infrastructure. Review does not authorize edits, commits, pushes,
GitHub approvals or merges. Post a comment only when the request says so.

## Scope and evidence

Inspect the branch and the complete merge-base diff. Prefer the PR's base, then a
user-specified base, then `origin/main`. Include staged, unstaged and relevant
untracked files when reviewing a local worktree; state exactly what you reviewed
(base, head SHA, file count). If there is no base, review the files as an initial
scaffold.

Read immediate consumers, contracts and tests as needed. Do not block on
pre-existing debt or tickets that come later in the plan. Use CI results and
eval output on the PR as evidence; run checks yourself only when asked or when
you can do so without side effects. A missing live-OBS observation is a
verification gap, not a confirmed defect. Use
[validation routes](../../../references/validation.md).

## Review priorities

1. **Leak safety:** any path that releases or renders a frame without a completed
   scan and without the fail-closed fallback; retroactive masks not applied to
   every buffered unsent frame; masks shrinking before hold expiry; margin
   missing; track loss, resize, scene switch or dropped frames creating a gap;
   fallback that fails open on error or exception.
2. **Real-time behavior:** render thread waiting on locks, inference or I/O;
   per-frame allocations on hot paths; unbounded queues, buffers or caches;
   GPU stage-surface stalls; worker shutdown that can hang OBS; timing that reads
   wall clock where the injected clock is required.
3. **Privacy and trust:** watch-list entries or recognized text reaching logs,
   errors, overlays, crash paths, eval artifacts or disk; unpinned or unverified
   downloads; runtime network access; real personal data or real faces committed.
4. **Correctness:** coordinate spaces and letterbox math, pixel formats and
   stride, premultiplied alpha, NMS and thresholds, Unicode normalization and
   fuzzy-match bounds, Luhn and pattern validators, off-by-one in time windows.
5. **OBS lifecycle and concurrency:** create/update/destroy, source resize,
   graphics context use, data races, lifetime of buffers shared with workers.
6. **Architecture and simplicity:** core stays OBS-free with injected clock;
   contracts change with all consumers and docs; small modules; no speculative
   abstractions, frameworks or services.
7. **Evidence quality:** tests exercise behavior, not mocks of themselves; eval
   deltas reported honestly; no weakened assertions, tolerances, timeouts,
   scoring or CI stages; claims match the evidence kind.

Trace concrete triggers to consequences. A useful finding names the file and
line, the failure scenario, the impact, and the smallest correction. Suggest a
broader redesign only with a smaller concrete replacement shape.

## Score and verdict

Score each applicable dimension from 0 to 100 and weight it:

| Dimension | Weight |
| --- | --- |
| Leak safety and correctness | 30 |
| Real-time behavior and resource bounds | 15 |
| Privacy, trust boundaries, supply chain | 15 |
| Architecture and ownership | 10 |
| Simplicity and maintainability | 10 |
| Tests and evidence quality | 15 |
| Repository discipline and docs | 5 |

Compute `round(sum(weight * score) / sum(applicable weights))`. Mark truly
irrelevant dimensions N/A and say so. Missing evidence on an affected surface
lowers that dimension's score; it is not N/A. An average never overrides a
concrete blocker.

Verdicts: **Approve**, **Approve with nits**, **Request changes**, **Block merge**.
Severities: **P0** a frame can air unblurred, a secret can leak, or the build or
main breaks; **P1** must fix before merge; **P2** material follow-up; **P3** polish.
A demonstrated fail-open path, watch-list leak, render-thread block, or weakened
check is at least P1. If no actionable defect exists, say so without inventing
findings.

## Report

Return: verdict, score with compact breakdown, scope (base, head SHA, files),
prioritized findings, and verification gaps.

## Automation output

When posting is requested (the CI review workflow, or the autopilot fallback),
publish one PR comment whose body starts with these two lines, exactly:

```
<!-- spg-staff-review head=<full head SHA> -->
SPG-REVIEW: verdict=<approve|approve-with-nits|request-changes|block> score=<0-100> p0=<n> p1=<n> p2=<n> p3=<n>
```

Then the human-readable report. Each finding starts with its severity tag, for
example `**P1** core/src/schedule/scheduler.cpp:142 — ...`. Never include
watch-list contents, recognized text from real data, or secrets in the comment.
`scripts/agent/pr_status.py` reads these two lines and trusts them only when the
review workflow's bot account posted them. A review posted from anyone else,
including the repository owner's account that local agents use, never counts.
