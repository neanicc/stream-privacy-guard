# Stream Privacy Guard validation routes

Use [the plan](../../docs/plan.md#11-verification-strategy-and-acceptance-checklist),
[eval definitions](../../docs/eval.md) and [AGENTS.md](../../AGENTS.md). The
product runtime is the OBS plugin; the core is validated headless first.

## Select checks from the current repository

Inspect `scripts/ci/run.sh`, `CMakeLists.txt` and `eval/` before naming a
command. Before SPG-01 most stages print a notice and do nothing.

| Changed surface | Applicable checks |
| --- | --- |
| Docs/skills/agents | `python3 scripts/check_agent_files.py`, `git diff --check`; inspect new files |
| Core C++ (`core/`) | Focused tests via `ctest --test-dir build -R <name>`, then `scripts/ci/run.sh build` and `test` |
| Policy, tracker, scheduler, detectors, OCR, models | Above plus `scripts/ci/run.sh eval`; report metric deltas against `main` |
| Eval generator or scorer | Scorer self-tests (no-op fails recall, blur-all fails clean-frame budget, seed determinism) plus `scripts/ci/run.sh eval` |
| Plugin (`plugin/`) | Plugin build on the local OS; CI builds all three. Loading in OBS is live evidence, not automated |
| Build system, deps, models manifest | Clean configure in a fresh build directory, `setup`/`build`/`test`; hashes verified by the fetch scripts |
| GitHub workflow (human-owned) | actionlint and the stages the workflow invokes |
| Whole foundation, PR signoff, autopilot acceptance | `setup`, `build`, `test`, `eval --local` on the dev Mac |

Reuse passing results when inputs are unchanged. Record the tested commit or
label an uncommitted worktree. Failed, unavailable or empty suites are not passes;
`ctest` with no tests is an error. Never weaken assertions, tolerances, eval
scoring or thresholds to get green results.

## Evidence kinds

- **Automated:** build, unit and integration tests, workflow and doc checks.
- **Synthetic eval:** generated frames with exact ground truth scored against
  `eval/thresholds.toml`. Shows pipeline accuracy on that distribution only.
- **Live OBS:** the plugin running on a real source on named hardware. Records
  OBS version, OS, commit, scenario, observations.
- **Human leak test:** a recorded session checked by `spg-cli scrub` and by eye,
  per [the live check](../../docs/live-check.md).

Realtime numbers (`bench --realtime`) are valid only for the machine that produced
them. CI runners are not the dev Mac. No synthetic or CI result establishes that
a live stream is protected.
