---
name: spg-grill-me
description: Challenge the Stream Privacy Guard plan one consequential decision at a time, grounding questions in the repository, OBS behavior and eval evidence. Use for grill me, stress-test this plan, or challenge assumptions.
---

# Challenge the Stream Privacy Guard plan

Read [plan.md](../../../../docs/plan.md) (especially section 12, open questions),
[AGENTS.md](../../../../AGENTS.md) and any actual evidence for the decision at
hand: code, eval output, bench numbers, agent-log entries. Inspect the repository
before asking anything the files can answer. Do not treat the plan snapshot as
confirmed behavior.

Ask one consequential question at a time, give a recommended choice with its
tradeoff, and wait for the answer. Start with the earliest unresolved dependency:
frame access and lookahead in OBS, fail-closed default, watch-list storage, model
and dependency choices, then thresholds, UX and Tier 2 scope.

Use concrete scenarios: a notification with the user's address is on screen for
three frames and detection takes 220 ms; the GPU is saturated by a game and
cycles take 900 ms; the user resizes the source mid-stream; OCR reads
"742 Evergreen Terr4ce"; a chat message contains a card number split across two
lines; the watch-list file is missing after an OBS update. Ask what behavior and
what observable evidence would count as success. Distinguish synthetic eval,
realtime bench, live OBS observation and a human leak test.

Prefer a smaller demonstrable tier over a longer unproven feature list. Keep
proposed thresholds, hardware assumptions and OBS API capabilities unresolved
until supported by evidence. Recheck version-sensitive facts (OBS APIs, ONNX
Runtime execution providers, model licenses) from their official source.

Capture agreed decisions in `docs/plan.md` or `docs/agent-log.md` when the user
asks. Do not turn answers into implementation, publication, or edits to
`eval/thresholds.toml` (human-owned). Finish with the decisions reached and the
evidence still required.

In an unattended autopilot run there is no one to answer. Do not run this skill
there; take the plan's recommended default, record it in the agent log, and move on.
