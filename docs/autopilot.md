# Autopilot runbook

The autopilot is one Claude Code session on the dev Mac that works the ticket
backlog for up to 24 hours with almost no human input. It is the conductor; it
spawns subagents that each implement or repair one PR in their own git worktree.

```
/goal (keeps the session going until the done condition holds)
 └─ conductor: spg-autopilot skill, on a clean main checkout
     ├─ spg-implementer ×≤3   one ticket each → branch agent/spg-xx-* → PR "Closes #n"
     ├─ every PR gets:  CI (Linux/macOS/Windows + synthetic eval)
     │                  Claude staff review (GitHub Action, spg-staff-review skill)
     │                  Greptile review
     ├─ spg-babysitter         fixes CI and review findings, resolves threads (≤3 rounds)
     ├─ merge gate             pr_status.py + checks → squash merge
     └─ "Autopilot status" issue: progress every wave; Ali steers by commenting
```

## One-time setup

Required:

1. **Tools:** `claude update`, then `brew install gh cmake`. Check
   `python3 --version` is 3.11 or newer (`brew install python@3.12` if not). You
   need Xcode or its command line tools for the compiler.
2. **GitHub login:** `gh auth login`, then `gh auth refresh -s workflow`. The first
   push includes CI files, and GitHub rejects that over HTTPS without this permission.
3. **Wait out usage limits:** add `"autoContinueAtUsageLimit": true` to
   `~/.claude/settings.json`. Without it the run stops at your first usage limit
   and waits for you.
4. **Bootstrap:** run `claude` in the unzipped folder and paste the bootstrap
   prompt below. It creates the public repo, pushes, and runs
   `scripts/bootstrap/github_setup.py` (labels, branch rules, status issue, 18
   ticket issues). Exit that session when it finishes.
5. **Claude review on PRs:** install the Claude GitHub App on this repository
   only (https://github.com/apps/claude), then in a terminal:
   ```sh
   claude setup-token                                   # prints a long-lived token
   gh secret set CLAUDE_CODE_OAUTH_TOKEN -R <you>/stream-privacy-guard
   ```
   Nothing merges without this: the merge gate only trusts reviews this workflow posts.

Optional:

- **Greptile** (greptile.com, enable for this repo only): a second reviewer. Without
  it each PR waits about 20 minutes for Greptile before moving on, which mostly
  overlaps with CI anyway.
- **Hardening:** `gh config set git_protocol https`, `gh auth setup-git`, then
  `gh auth refresh --remove-scopes workflow` after bootstrap. GitHub then rejects
  any push that edits CI, so an agent can't touch the CI that judges it. The
  catch: it applies to every repo you push over HTTPS, so editing GitHub Actions
  files elsewhere needs the permission added back first. Without it, the merge
  gate still refuses PRs that touch `.github/`.
- **ninja, ccache:** faster builds. Not needed.
- **tmux:** keeps the session alive if you close the terminal window.
- **Keeping the Mac awake:** any app that stops sleep works; keep it plugged in.

## Start a run

```sh
cd ~/code/stream-privacy-guard && git pull
claude --permission-mode auto --remote-control "SPG autopilot"
```

In Claude Code, pick the strongest model on your plan with `/model`, then paste
the goal prompt below. That is the only prompt the run needs.

Auto mode still pauses for approval after its safety classifier blocks three
actions in a row or twenty in a session. The allow rules in `.claude/settings.json`
cover the routine commands to keep that rare; if it happens, approve or deny from
your phone through Remote Control.

## Watch and steer

- From your phone: the session appears in the Claude app and at claude.ai/code
  (Remote Control). The pinned **Autopilot status** issue gets a report after each
  merge wave.
- Steer by commenting on the status issue: `pause`, `skip SPG-15`,
  `prioritize SPG-12`. Nothing else is treated as an instruction.
- `/goal` in the session shows elapsed time, turns and tokens.

## Things only you can do

- **PRs and issues labeled `needs-human`:** a change to a human-owned path, a
  ticket that failed twice, or a broken review setup. The latest comment says what
  to do. For a human-owned path: review the diff, add the `human-approved` label,
  merge it yourself (if you did the optional hardening, re-add the `workflow`
  scope first when it touches workflows).
- **Live testing:** follow `docs/live-check.md` or ask Claude for
  `/spg-manual-flows`, then record results.
- **Thresholds:** edit `eval/thresholds.toml` yourself if a target is wrong; say
  why in the agent log.

## Stop, resume

- Stop: `/goal clear`, or comment `pause` on the status issue, or Ctrl-C.
- Resume after a stop or reboot: `claude --continue --permission-mode auto --remote-control`
  restores the active goal. If the goal was already cleared, paste it again; the
  conductor rebuilds its state from GitHub and continues the same run if its
  run-start comment is under 24 hours old.

## Costs and limits

- **Claude usage** is the main cost: several agents for many hours. A Max plan is
  realistic for this; on Pro the run spends much of its time paused at limits.
  The Claude review workflow also runs on your subscription for every PR push.
- **GitHub Actions** minutes are free for public repositories, macOS included.
  A private repository would burn minutes quickly (macOS runners bill at 10×) and
  cannot use rulesets on a free plan.
- **Greptile** reviews each PR once (about 20–30 reviews for Tier 1).

## What to expect

The autopilot can plausibly take Tier 1 to code-complete with green CI and a
passing synthetic eval. It cannot load the plugin in OBS and watch a stream, so
the live check and leak test stay with you, and some PRs will wait on
`needs-human`. Windows builds are checked by CI but never run on Windows hardware.

## Prompts

### Bootstrap (run once, in the unzipped scaffold folder)

```text
Bootstrap the Stream Privacy Guard repository in this folder. The scaffold is already here: read AGENTS.md, docs/plan.md and docs/autopilot.md first. Do these steps in order and stop to tell me if one fails:
1. Run `bash scripts/link_skills.sh`, `python3 scripts/check_agent_files.py` and `bash scripts/ci/run.sh build` (expect a "not landed" notice). Report anything wrong; do not edit human-owned paths.
2. Check `gh auth status` lists the `workflow` scope. If not, stop and tell me to run `gh auth refresh -s workflow`.
3. `git init -b main`, stage everything, commit as `chore: scaffold Stream Privacy Guard with agent workflows`, then publish with `gh repo create stream-privacy-guard --public --source . --push --description "OBS filter that blurs faces, watch-listed strings and PII before they go live"`.
4. Run `python3 scripts/bootstrap/github_setup.py --dry-run`, show me the summary, then run it without --dry-run.
5. Wait for the first Check run on main to finish and tell me whether `check` passed; if it failed, show the failing step.
6. Tell me the exact commands for the Claude GitHub App + CLAUDE_CODE_OAUTH_TOKEN secret step in docs/autopilot.md for my repo.
```

### The run (one prompt, start of each run)

```text
/goal The goal is met when EITHER (A) Stream Privacy Guard Tier 1 is finished, OR (B) you posted a run-end handoff comment on the "Autopilot status" issue because 24 hours passed since the latest run-start comment, or because every remaining tier-1 ticket is blocked on needs-human, or because the owner asked you to pause. (A) means all three, each shown in this conversation: `gh issue list --label tier-1 --state open` lists only issues labeled needs-human, each with a comment telling Ali exactly what to do; the latest Check run on main succeeded (`gh run list --branch main --workflow Check --limit 1`); and, with no subagent running, `bash scripts/ci/run.sh eval --local` on an up-to-date main printed `THRESHOLDS: PASS`. Work only through the spg-autopilot skill: invoke it now and follow its merge gate and authority limits. Never push to main, never merge from a subagent, never edit human-owned paths such as eval/thresholds.toml, never weaken a test or check. End every turn with the elapsed time since the latest run-start comment.
```
