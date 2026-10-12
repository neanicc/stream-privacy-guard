#!/usr/bin/env python3
"""PreToolUse hook: the only way a `gh pr merge` runs. HUMAN-OWNED.

Blocks the merge (exit 2, reason on stderr) unless all hold:
- the call comes from the main session, not a subagent (no agent_id in the input);
- the command is exactly `gh pr merge <n> --squash --delete-branch --match-head-commit <sha>`;
- scripts/agent/pr_status.py (from the main checkout) reports gate.mechanical_pass
  and its head SHA equals <sha>.

Fails closed: any error or internal timeout exits 2. settings.json also sets
onFailure "block" so a crash or hook timeout blocks the call.
"""
from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
import sys

COMMAND = re.compile(
    r"^\s*gh\s+pr\s+merge\s+(\d+)\s+--squash\s+--delete-branch\s+--match-head-commit\s+([0-9a-f]{40})\s*$"
)


def block(reason: str) -> None:
    print(f"Merge blocked by .claude/hooks/guard_merge.py: {reason}", file=sys.stderr)
    sys.exit(2)


def looks_like_merge(command: str) -> bool:
    if re.search(r"\bgh\b.*\bmerge\b", command, re.DOTALL) is None:
        return False
    try:
        tokens = shlex.split(command, posix=True)
    except ValueError:
        return True  # unparseable command that mentions gh and merge: treat as a merge
    return "gh" in tokens and "merge" in tokens


def main() -> None:
    event = json.load(sys.stdin)
    command = (event.get("tool_input") or {}).get("command") or ""
    if not looks_like_merge(command):
        sys.exit(0)
    if event.get("agent_id") or event.get("agent_type"):
        block("only the autopilot conductor in the main session may merge; subagents never merge.")
    match = COMMAND.match(command)
    if not match:
        block("use exactly `gh pr merge <n> --squash --delete-branch --match-head-commit <40-char head sha>` "
              "as a single command.")
    number, sha = match.groups()
    project = os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()
    script = os.path.join(project, "scripts", "agent", "pr_status.py")
    try:
        result = subprocess.run([sys.executable, script, number], capture_output=True, text=True,
                                cwd=project, timeout=120)
    except subprocess.TimeoutExpired:
        block("pr_status.py timed out; try again shortly.")
    if result.returncode != 0:
        block(f"pr_status.py failed: {result.stderr.strip()[:500]}")
    status = json.loads(result.stdout)
    if status["head"] != sha:
        block(f"head moved: PR head is {status['head']}, command names {sha}. Re-run the gate.")
    if not status["gate"]["mechanical_pass"]:
        block("gate not passed: " + "; ".join(status["gate"]["reasons"]))
    sys.exit(0)


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except BaseException as error:  # fail closed on anything unexpected
        block(f"hook error: {type(error).__name__}: {error}")
