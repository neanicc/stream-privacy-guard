# Pull request checks

[Check](../.github/workflows/check.yml) runs on every pull request (including
label changes), pushes to `main`, merge queues and manual dispatch. New commits
cancel older runs for the same PR. The repository ruleset requires the
aggregate `check` job.

| Job | What it verifies |
| --- | --- |
| Agent files and docs | `scripts/check_agent_files.py`: skill frontmatter, discovery links, agent definitions, Markdown links and anchors, config files parse |
| Workflow validation | actionlint 1.7.12 (checksum-verified) over all workflows |
| Protected paths | A PR that changes (or renames) a human-owned path fails until a human adds the `human-approved` label: `eval/thresholds.toml`, `.github/**`, `.claude/**`, `.agents/**`, `.greptile/**`, `scripts/agent/**`, `scripts/bootstrap/**`, `AGENTS.md`, `CLAUDE.md`, `LICENSE` |
| Build and test | `scripts/ci/run.sh setup`, `build`, `test`, `package` on Ubuntu 24.04, macOS 15 and Windows 2025 (Visual Studio generator); uploads `packages-<os>` from `dist/` |
| Synthetic eval | `scripts/ci/run.sh eval` (CI suites) scored against `eval/thresholds.toml`; uploads `eval-report` |
| `check` | All of the above succeeded; failures, cancellations and skips cannot pass |

Workflows are human-owned. Agents change what CI does by editing
`scripts/ci/run.sh` (lane:build), which every stage calls. Since SPG-01 the
build stages are real: `setup` runs `scripts/fetch_deps.py` against
`deps/manifest.json`, (since SPG-04) `scripts/fetch_models.py` against
`models/manifest.json` and (since SPG-13) `build-aux/build_libobs.py`, which
builds libobs from the pinned OBS sources on macOS and Windows for the plugin
(Linux uses the pinned Ubuntu package), `build` configures CMake (Ninja when installed, the newest
Visual Studio with C++ tools on Windows) and prints ccache statistics, and
`test` runs `ctest`, which fails if no tests are registered. One pin lives
outside the manifest: Catch2 is fetched by CMake `FetchContent` at configure
time, pinned by URL and SHA-256 in `tests/CMakeLists.txt`, so the first
configure needs network access.

Plugin coverage differs by OS. `plugin.loads` (headless libobs: module load,
filter registration, locale, defaults, property types, and the detection models
loaded through the ONNX Runtime copied next to the module) and
`plugin.loads.no_models` (an empty cache: the models are reported missing and
no scheduler is built) run on macOS and Windows only; on Linux the Ubuntu `libobs.so` needs system FFmpeg, PulseAudio
and Wayland libraries the pins do not provide, so the Linux job proves the
module builds, links and exports its entry points (`plugin.exports`) and
nothing more (#27). `plugin.models.*` (model pins, SHA-256, the loader thread,
and the stages run on a synthetic frame with the pinned models) runs on every
OS. No job renders through the filter: the fail-closed fill in
`video_render` is checked only by live OBS ([live check](live-check.md)).

Dependencies, models and the ccache cache (`~/.cache/spg/ccache`) are cached in
`~/.cache/spg` keyed on the manifests and `CMakeLists.txt`.
Action references are pinned to commit SHAs.

## Reviews on every PR

- [Claude staff review](../.github/workflows/claude-review.yml) runs the
  `spg-staff-review` skill on each non-draft PR push and posts one comment
  starting with a machine-readable `SPG-REVIEW:` line. It needs the
  `CLAUDE_CODE_OAUTH_TOKEN` repository secret (`claude setup-token`) and the
  Claude GitHub App. It uses the token owner's Claude subscription.
- Greptile reviews each PR when it opens, configured by
  [.greptile/config.json](../.greptile/config.json). To re-review on every push,
  change `autoReview` to `["open", "push"]` (uses more of the Greptile quota).
- Branch rules require the `check` job from GitHub Actions and resolved review
  threads before merge; there are no bypass actors.
- Only reviews posted by the workflow's bot count for the autopilot gate. Every
  local agent uses the owner's GitHub credentials, so the owner's account is not
  trusted as a reviewer.

`scripts/agent/pr_status.py <pr>` summarizes all of these for one PR and computes
the mechanical part of the autopilot merge gate.

## Reproduce locally

```sh
python3 scripts/check_agent_files.py
bash scripts/ci/run.sh setup && bash scripts/ci/run.sh build && bash scripts/ci/run.sh test
bash scripts/ci/run.sh eval            # CI suites
bash scripts/ci/run.sh eval --local    # all suites, acceptance gate
actionlint                             # needs actionlint installed
```

Green CI shows software and synthetic-eval behavior on CI runners only. Realtime
numbers come from the dev Mac, and live protection needs the
[live check](live-check.md).
