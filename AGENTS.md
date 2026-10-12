# Stream Privacy Guard agent guide

Stream Privacy Guard (SPG) is an OBS Studio filter that detects sensitive content
on a live video source (faces, watch-listed strings such as a real name or home
address, and structured PII such as phone or card numbers) and blurs it before
the frame reaches the encoder. It is a **safety net, not a guarantee**: a missed
frame has already aired. Never describe it as preventing doxxing.

Read [the plan](docs/plan.md) before changing behavior. Its tickets, thresholds
and open questions are a planning snapshot; the code, tests and eval output are
the current evidence. [docs/autopilot.md](docs/autopilot.md) explains how
unattended runs work.

## Repository status and scope

Inspect the tree and manifests before naming a command or claiming a capability
exists. Until SPG-01 lands there is no build: the repository holds the plan,
agent workflows, CI structure and eval thresholds. Implementation follows the
dependency-ordered tickets in [plan section 9](docs/plan.md#9-tickets), mirrored
as GitHub issues labeled `ticket`.

Implement the requested work and its necessary validation. Preserve unrelated
work. A build request does not by itself authorize publication; a review request
is read-only unless fixes are also requested. For an end-to-end delivery request
or an autopilot run, complete the authorized steps without asking at each
workflow boundary.

## Skills

Canonical skills live in `.agents/skills/workflow/`. `.claude/skills/` and
`.cursor/skills/` hold relative discovery links; edit the canonical files and
run `scripts/link_skills.sh` after adding one. `CLAUDE.md` points here so
guidance has one owner. These workflows are adapted from the Trail/teachAR
delivery skills for a real-time C++/OBS project.

| Request | Skill |
| --- | --- |
| Run the backlog unattended, autopilot, keep shipping | [spg-autopilot](.agents/skills/workflow/spg-autopilot/SKILL.md) |
| Implement ticket SPG-xx, take an issue end to end | [spg-ticket](.agents/skills/workflow/spg-ticket/SKILL.md) |
| Commit, commit and push | [spg-commit](.agents/skills/workflow/spg-commit/SKILL.md) |
| Create/open/draft PR, write PR description | [spg-create-pr](.agents/skills/workflow/spg-create-pr/SKILL.md) |
| Staff review, review branch, grade PR | [spg-staff-review](.agents/skills/workflow/spg-staff-review/SKILL.md) |
| Cleanup, simplify, fix review findings | [spg-cleanup](.agents/skills/workflow/spg-cleanup/SKILL.md) |
| Signoff, verify readiness | [spg-signoff](.agents/skills/workflow/spg-signoff/SKILL.md) |
| Catch up with main, resolve merge conflicts | [spg-catchup](.agents/skills/workflow/spg-catchup/SKILL.md) |
| Babysit PR, fix CI, address PR feedback | [spg-babysit](.agents/skills/workflow/spg-babysit/SKILL.md) |
| Manual test plan, live OBS QA, leak test | [spg-manual-flows](.agents/skills/workflow/spg-manual-flows/SKILL.md) |
| Grill me, challenge the plan | [spg-grill-me](.agents/skills/workflow/spg-grill-me/SKILL.md) |

Use relevant skills to support the requested task; their presence does not
authorize extra external actions. Staff review, cleanup and signoff are useful
separately, not mandatory ceremonies before every commit, except where the
autopilot merge gate requires them.

Subagent roles live in `.claude/agents/`: `spg-implementer` and `spg-babysitter`
work in isolated worktrees, `spg-reviewer` is a read-only self-check (its reports
never count for the merge gate), `spg-researcher` answers bounded technical
questions. Delegate only when the user or the autopilot skill
calls for it.

## Autopilot authority

An autopilot run (started with the `spg-autopilot` skill or a `/goal` that names
it) is authorized to create branches, commit, push task branches, open PRs, create
and label ticket issues, reply to and resolve review threads it addressed, comment
on the "Autopilot status" issue, rerun CI and review workflows, and squash-merge
its own PRs **only** from the main session when the merge gate in that skill
passes (`.claude/hooks/guard_merge.py` enforces it). It is never authorized to:

- push to `main`, force-push, rewrite published history, or use `gh pr merge --admin`;
- edit or merge a change to a human-owned path: `eval/thresholds.toml`,
  `.github/**`, `.claude/**`, `.agents/**`, `.greptile/**`, `scripts/agent/**`,
  `scripts/bootstrap/**`, `AGENTS.md`, `CLAUDE.md`, `LICENSE`. Open the PR, label it
  and its issue `needs-human`, explain why, and move on;
- add or remove the `human-approved` label, change repository settings, rulesets,
  secrets, credentials or app installations;
- merge from a subagent, merge any other way than the gated command in
  `spg-autopilot`, or post a staff review that claims to come from CI;
- weaken a test, tolerance, threshold, timeout or CI step to get a green result;
- commit model weights, datasets, recordings, screenshots of real people or real
  personal data.

## Stack and ownership

C++20 and CMake for the product; Python 3.12 only for eval tooling and scripts.
Keep the core free of OBS so it can be tested headless on any CI runner.

| Planned location | Responsibility |
| --- | --- |
| `core/` (`spg-core`) | Pure pipeline: frame views, detectors, OCR, policy, tracking, scheduling, mask planning. No OBS, no globals, injected clock |
| `cli/` (`spg-cli`) | Runs the core on PNG sequences: `run`, `bench`, `eval` subcommands |
| `plugin/` (`obs-spg`) | Thin OBS filter adapter: frame capture, lookahead buffer, GPU blur effect, settings UI |
| `eval/` | Synthetic data generator, scorer, human-owned `thresholds.toml` |
| `models/manifest.json`, `deps/manifest.json` | Pinned download URLs, SHA-256 and licenses. Weights and binaries live in `$SPG_CACHE_DIR` (default `~/.cache/spg`), never in Git |
| `scripts/ci/run.sh` | The only entry point CI calls: `setup`, `build`, `test`, `eval` stages |
| `tests/` | Unit and integration tests plus small synthetic fixtures |

Prefer the approach proven by `obs-backgroundremoval` and `obs-detect` for
bundling ONNX Runtime and OpenCV into an OBS plugin. Pin every fetched artifact
by version and SHA-256. Do not add a framework, service, network call or
telemetry without a plan change.

## Product and safety invariants

- **Fail closed.** If the pipeline cannot finish scanning a frame before its
  lookahead deadline, the configured fallback applies (default: blur the whole
  source) rather than emitting an unscanned frame. Never default to fail-open.
- **Retroactive masking.** A region detected at time t is applied to every
  buffered frame from `t - lookahead` onward that is still unsent, so the first
  frame a secret appears is covered even though detection took time.
- **Temporal hold.** A blurred region stays blurred for at least the configured
  hold after its last detection, with margin. Tracking loss, a dropped frame or a
  source resize can extend a mask, never shrink it early.
- **Recall over precision.** When uncertain, blur. Over-blur is bounded by the
  clean-frame blur budget in `eval/thresholds.toml`; recall targets are not
  traded away for it.
- **The render thread never waits on inference.** Detection runs on worker
  threads with bounded queues; the OBS render path only composes masks.
- **Coordinates are explicit.** Every box records its space (source pixels,
  model input, letterboxed). Conversions live in one tested module.
- **Watch-list content is secret.** It is never logged, printed, sent anywhere,
  written to crash dumps, or included in eval artifacts other than synthetic ones.
- Frames are never written to disk unless the user enables an explicit debug
  capture, which defaults off and is labeled as such.

## Boundaries and privacy

No network access at runtime. Models are fetched once by `scripts/fetch_models.py`
from pinned URLs and verified by hash. Real recordings, screenshots, faces and
personal data stay out of Git and out of CI artifacts; synthetic fixtures are the
default. A dataset used only for local evaluation is fetched to the cache with
its license recorded in `models/manifest.json` and never redistributed.

## Checks and evidence

Use [.agents/references/validation.md](.agents/references/validation.md). Once
SPG-01 lands, the full software gate is `scripts/ci/run.sh build`, `test` and
`eval`, plus actionlint for workflow changes. Focused edits use focused checks;
reuse passing evidence while inputs are unchanged. Report unavailable or failing
checks; never weaken checks to get green results.

Separate four kinds of evidence: **automated** (build, unit tests), **synthetic
eval** (generated frames scored against thresholds), **live OBS** (the plugin on a
real machine and source) and **human leak test** (a recorded session scrubbed
frame by frame). No synthetic result establishes live behavior. Follow
[the live check](docs/live-check.md).

Keep [docs/agent-log.md](docs/agent-log.md) current: each PR that makes a
substantive decision or change adds one entry in the same PR.

## Git and delivery

- Work on `agent/spg-<nn>-<slug>` branches from `main`. Never commit on `main`.
- Conventional Commits (`type(scope): summary`, under 72 characters), focused
  staging, inspect staged and untracked files, honor hooks.
- One ticket per PR. The PR body includes `Closes #<issue>` for the ticket issue.
- Do not amend shared history or force-push. Commit-only requests stay local.
- Merging is allowed only under the autopilot merge gate or an explicit request.
  Never infer deployment, release publishing or contacting anyone from PR
  authorization.
