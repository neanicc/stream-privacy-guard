#!/usr/bin/env python3
"""Structural checks for agent files and docs. Runs in CI (Check / Agent files and docs).

Verifies:
- every canonical skill has SKILL.md frontmatter whose name matches its directory;
- .claude/skills and .cursor/skills hold one relative link per canonical skill, none dangling;
- AGENTS.md lists every skill;
- .claude/agents/*.md have name/description and preload only existing skills;
- relative Markdown links (and #anchors into Markdown files) resolve;
- JSON and TOML config files parse; CLAUDE.md imports AGENTS.md.
Standard library only.
"""
from __future__ import annotations

import json
import os
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CANONICAL = ROOT / ".agents" / "skills" / "workflow"
SKIP_DIRS = {".git", "build", "eval-out", "node_modules", ".claude/worktrees", "_deps"}
LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")

errors: list[str] = []


def fail(message: str) -> None:
    errors.append(message)


def frontmatter(path: Path) -> dict[str, object]:
    text = path.read_text(encoding="utf-8")
    if not text.startswith("---\n"):
        fail(f"{rel(path)}: missing frontmatter")
        return {}
    end = text.find("\n---", 4)
    if end == -1:
        fail(f"{rel(path)}: unterminated frontmatter")
        return {}
    data: dict[str, object] = {}
    key = None
    for line in text[4:end].splitlines():
        if not line.strip():
            continue
        item = re.match(r"^\s+-\s+(.*)$", line)
        if item and key:
            existing = data.get(key)
            data[key] = (existing if isinstance(existing, list) else []) + [item.group(1).strip()]
            continue
        pair = re.match(r"^([A-Za-z][\w-]*):\s*(.*)$", line)
        if not pair:
            fail(f"{rel(path)}: unparseable frontmatter line: {line!r}")
            continue
        key, value = pair.group(1), pair.group(2).strip()
        if " #" in value or value.startswith("#"):
            fail(f"{rel(path)}: '{key}' contains ' #', which YAML reads as a comment and truncates")
        data[key] = value if value else []
    return data


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def slug(heading: str) -> str:
    text = heading.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors(path: Path) -> set[str]:
    found: set[str] = set()
    in_code = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("```"):
            in_code = not in_code
            continue
        if not in_code and (m := re.match(r"^#{1,6}\s+(.*)$", line)):
            found.add(slug(m.group(1)))
    return found


def check_skills() -> list[str]:
    names = []
    if not CANONICAL.is_dir():
        fail("missing .agents/skills/workflow")
        return names
    for skill_dir in sorted(p for p in CANONICAL.iterdir() if p.is_dir()):
        skill_md = skill_dir / "SKILL.md"
        if not skill_md.is_file():
            fail(f"{rel(skill_dir)}: missing SKILL.md")
            continue
        meta = frontmatter(skill_md)
        if meta.get("name") != skill_dir.name:
            fail(f"{rel(skill_md)}: name {meta.get('name')!r} does not match directory {skill_dir.name!r}")
        description = meta.get("description")
        if not isinstance(description, str) or not description:
            fail(f"{rel(skill_md)}: missing description")
        elif len(description) > 1024:
            fail(f"{rel(skill_md)}: description longer than 1024 characters")
        elif ": " in description:
            fail(f"{rel(skill_md)}: description contains ': ' which breaks YAML; rephrase")
        names.append(skill_dir.name)

    for tool in (".claude", ".cursor"):
        link_dir = ROOT / tool / "skills"
        present = {p.name: p for p in link_dir.iterdir()} if link_dir.is_dir() else {}
        for name in names:
            link = present.get(name)
            expected = f"../../.agents/skills/workflow/{name}"
            if link is None:
                fail(f"{tool}/skills/{name}: missing discovery link (run scripts/link_skills.sh)")
            elif not link.is_symlink() or os.readlink(link) != expected:
                fail(f"{tool}/skills/{name}: must be a symlink to {expected}")
        for name, link in present.items():
            if link.is_symlink() and not link.exists():
                fail(f"{tool}/skills/{name}: dangling link")

    agents_md = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
    for name in names:
        if f"[{name}]" not in agents_md:
            fail(f"AGENTS.md: skills table does not list {name}")
    return names


def check_agents(skill_names: list[str]) -> None:
    agent_dir = ROOT / ".claude" / "agents"
    for path in sorted(agent_dir.glob("*.md")) if agent_dir.is_dir() else []:
        meta = frontmatter(path)
        if meta.get("name") != path.stem:
            fail(f"{rel(path)}: name {meta.get('name')!r} does not match file name")
        if not meta.get("description"):
            fail(f"{rel(path)}: missing description")
        skills = meta.get("skills") or []
        for skill in skills if isinstance(skills, list) else [skills]:
            if skill not in skill_names:
                fail(f"{rel(path)}: preloads unknown skill {skill!r}")
        isolation = meta.get("isolation")
        if isolation not in (None, [], "worktree"):
            fail(f"{rel(path)}: unexpected isolation {isolation!r}")


def markdown_files() -> list[Path]:
    files = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        relative = os.path.relpath(dirpath, ROOT)
        dirnames[:] = [d for d in dirnames
                       if d not in SKIP_DIRS and os.path.normpath(os.path.join(relative, d)) not in SKIP_DIRS
                       and not (Path(dirpath) / d).is_symlink()]
        files += [Path(dirpath) / f for f in filenames if f.endswith(".md")]
    return sorted(files)


def check_links() -> None:
    anchor_cache: dict[Path, set[str]] = {}
    for path in markdown_files():
        in_code = False
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if line.lstrip().startswith("```"):
                in_code = not in_code
                continue
            if in_code:
                continue
            for target in LINK.findall(re.sub(r"`[^`]*`", "", line)):
                if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith("<"):
                    continue
                file_part, _, anchor = target.partition("#")
                dest = (path.parent / file_part).resolve() if file_part else path
                if not dest.exists():
                    fail(f"{rel(path)}:{number}: broken link {target}")
                    continue
                if anchor and dest.suffix == ".md":
                    anchor_cache.setdefault(dest, anchors(dest))
                    if anchor not in anchor_cache[dest]:
                        fail(f"{rel(path)}:{number}: missing anchor #{anchor} in {rel(dest)}")


def check_configs() -> None:
    for path in [ROOT / ".claude" / "settings.json", ROOT / ".greptile" / "config.json"]:
        try:
            json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            fail(f"{rel(path)}: {error}")
    for path in [ROOT / "eval" / "thresholds.toml"]:
        try:
            tomllib.loads(path.read_text(encoding="utf-8"))
        except (OSError, tomllib.TOMLDecodeError) as error:
            fail(f"{rel(path)}: {error}")
    claude_md = ROOT / "CLAUDE.md"
    if not claude_md.is_file() or "@AGENTS.md" not in claude_md.read_text(encoding="utf-8"):
        fail("CLAUDE.md must import @AGENTS.md")


def main() -> int:
    names = check_skills()
    check_agents(names)
    check_links()
    check_configs()
    if errors:
        for error in errors:
            print(f"::error::{error}")
        print(f"{len(errors)} problem(s) found.")
        return 1
    print(f"OK: {len(names)} skills, agents, links and configs verified.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
