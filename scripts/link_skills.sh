#!/usr/bin/env bash
# (Re)create the relative discovery links in .claude/skills and .cursor/skills
# for every canonical skill in .agents/skills/workflow. Safe to rerun.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

for tool in .claude .cursor; do
  mkdir -p "$tool/skills"
  # Remove stale links that point into the canonical directory.
  find "$tool/skills" -maxdepth 1 -type l -lname '../../.agents/skills/workflow/*' -exec rm -f {} +
  for dir in .agents/skills/workflow/*/; do
    name="$(basename "$dir")"
    ln -s "../../.agents/skills/workflow/$name" "$tool/skills/$name"
  done
done
echo "Linked $(find .agents/skills/workflow -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ') skills into .claude/skills and .cursor/skills."
