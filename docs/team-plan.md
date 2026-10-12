# Lanes and file ownership

Parallel agents (or people) work in lanes so two PRs rarely edit the same files.
This assigns boundaries; it does not authorize delegation by itself. Each ticket
issue carries one lane label.

| Lane label | Owns | Coordinates with |
| --- | --- | --- |
| `lane:build` | Root `CMakeLists.txt`, `CMakePresets.json`, `buildspec.json`, `deps/`, `scripts/ci/run.sh`, `scripts/package.sh`, `build-aux/`, packaging | Everyone: changes to shared build files go in their own small PR |
| `lane:vision` | `core/src/runtime/`, `core/src/detect/`, `core/src/ocr/`, `models/`, `scripts/fetch_models.py` | policy (detection contracts) |
| `lane:policy` | `core/include/spg/` contracts, `core/src/frame/`, `core/src/policy/`, `core/src/track/`, `core/src/schedule/`, `core/src/mask/` | vision, obs |
| `lane:eval` | `eval/` except `thresholds.toml`, `cli/`, `tests/fixtures/` generators | policy (ground-truth format) |
| `lane:obs` | `plugin/` | policy (mask plan), build (plugin CMake) |

Rules for parallel work:

- At most one open PR per lane at a time unless the files are disjoint.
- Shared contract headers (`core/include/spg/*.hpp`) change in a dedicated PR
  that updates every consumer, fixture and `docs/contracts.md` together.
- Each lane's CMake lives in its own subdirectory `CMakeLists.txt`. Adding a
  subdirectory to the root file is a one-line change; rebase rather than
  hand-merge conflicts there.
- `docs/agent-log.md` is append-only. Add your entry at the end of the file, so
  concurrent PRs conflict trivially; resolve by keeping both entries in merge order.
- Human-owned: `eval/thresholds.toml`, `.github/**`, `.claude/**`, `.agents/**`,
  `.greptile/**`, `scripts/agent/**`, `scripts/bootstrap/**`, `AGENTS.md`,
  `CLAUDE.md`, `LICENSE`.
