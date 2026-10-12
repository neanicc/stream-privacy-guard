#!/usr/bin/env python3
"""One-time, idempotent GitHub setup for Stream Privacy Guard. Run after the first push.

    python3 scripts/bootstrap/github_setup.py --dry-run      # show what would happen
    python3 scripts/bootstrap/github_setup.py                # apply

Creates or updates, never deletes:
- labels (ticket, tier-1/2, lane:*, in-progress, needs-human, human-approved, autopilot);
- repository merge settings (squash only, delete branch on merge);
- a "main protection" ruleset: PRs required, required `check` status, resolved threads,
  no force pushes or deletion, no bypass actors;
- the pinned "Autopilot status" issue;
- one issue per ticket in docs/plan.md section 9 (skips tickets that already have one).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

LABELS = {
    "ticket": ("1d76db", "One plan ticket, delivered as one PR"),
    "tier-1": ("0e8a16", "Required for Tier 1 acceptance"),
    "tier-2": ("c5def5", "After Tier 1 acceptance"),
    "lane:build": ("5319e7", "Build, deps, CI scripts, packaging"),
    "lane:vision": ("5319e7", "Model runtime, detectors, OCR"),
    "lane:policy": ("5319e7", "Contracts, policy, tracker, scheduler, masks"),
    "lane:eval": ("5319e7", "Eval generator, scorer, spg-cli"),
    "lane:obs": ("5319e7", "OBS plugin adapter"),
    "in-progress": ("fbca04", "An agent is working on this"),
    "needs-human": ("d93f0b", "Blocked on Ali; see the latest comment"),
    "human-approved": ("b60205", "Human reviewed changes to human-owned paths. Humans only."),
    "autopilot": ("000000", "Autopilot status and reports"),
}

RULESET = {
    "name": "main protection",
    "target": "branch",
    "enforcement": "active",
    "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
    "bypass_actors": [],
    "rules": [
        {"type": "deletion"},
        {"type": "non_fast_forward"},
        {"type": "pull_request", "parameters": {
            "required_approving_review_count": 0,
            "dismiss_stale_reviews_on_push": False,
            "require_code_owner_review": False,
            "require_last_push_approval": False,
            "required_review_thread_resolution": True,
            "allowed_merge_methods": ["squash"],
        }},
        {"type": "required_status_checks", "parameters": {
            "strict_required_status_checks_policy": False,
            "required_status_checks": [{"context": "check", "integration_id": 15368}],  # GitHub Actions app
        }},
    ],
}

STATUS_TITLE = "Autopilot status"
STATUS_BODY = """The autopilot posts progress here: merged PRs, PRs in flight, items that need Ali, and eval numbers.

Ali: comment here to steer it. Comments from the repository owner are read as instructions
(for example "pause", "skip SPG-15", "prioritize SPG-12"). Comments from anyone else are ignored.
See docs/autopilot.md."""

ROW = re.compile(r"^\|\s*(SPG-\d+[a-z]?)\s*\|\s*([^|]+?)\s*\|\s*(\w+)\s*\|\s*([^|]*?)\s*\|\s*(.+?)\s*\|\s*$")


def split_top_level(text: str) -> list[str]:
    """Split on ';' outside parentheses."""
    parts, depth, current = [], 0, []
    for char in text:
        depth += char == "("
        depth -= char == ")"
        if char == ";" and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(char)
    parts.append("".join(current))
    return parts


def parse_tickets() -> list[dict]:
    text = (ROOT / "docs" / "plan.md").read_text(encoding="utf-8")
    section = text.split("## 9. Tickets", 1)[1].split("\n## ", 1)[0]
    tickets = []
    for line in section.splitlines():
        match = ROW.match(line)
        if not match:
            continue
        ticket_id, title, lane, deps, done = match.groups()
        dependencies = [] if deps.strip() in {"", "—", "-"} else [
            f"SPG-{d.strip().zfill(2)}" for d in deps.split(",") if d.strip()]
        items = [item.strip().rstrip(".") for item in split_top_level(done) if item.strip()]
        tickets.append({"id": ticket_id, "title": title, "lane": lane, "deps": dependencies, "done": items})
    if not tickets:
        sys.exit("No tickets parsed from docs/plan.md section 9")
    return tickets


def plain_links(text: str) -> str:
    """Plan links are relative to docs/; issue bodies need repo paths."""
    def repl(match: re.Match) -> str:
        label, target = match.groups()
        return label if re.match(r"^[a-z]+:|^/", target) else f"{label} (docs/{target})"
    return re.sub(r"\[([^\]]+)\]\(([^)]+)\)", repl, text)


def issue_body(ticket: dict) -> str:
    deps = ", ".join(ticket["deps"]) if ticket["deps"] else "none"
    done = "\n".join(f"- [ ] {plain_links(item)}" for item in ticket["done"])
    return f"""**Plan reference:** docs/plan.md section 9, {ticket['id']}

**Lane:** lane:{ticket['lane']}

**Depends on:** {deps}

**Done when:**

{done}
- [ ] Tests or eval evidence in the PR's Validation section
- [ ] `docs/agent-log.md` entry for decisions or measurements
"""


class GitHub:
    def __init__(self, dry_run: bool):
        self.dry_run = dry_run

    def run(self, *args: str, mutate: bool = False, check: bool = True, stdin: str | None = None) -> str:
        if mutate and self.dry_run:
            print("  [dry-run] gh " + " ".join(args[:6]) + (" ..." if len(args) > 6 else ""))
            return ""
        result = subprocess.run(["gh", *args], capture_output=True, text=True, input=stdin)
        if check and result.returncode != 0:
            raise RuntimeError(f"gh {' '.join(args[:4])}: {result.stderr.strip()}")
        return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dry-run", action="store_true", help="read state, print changes, change nothing")
    parser.add_argument("--offline", action="store_true", help="with --dry-run: only parse the plan")
    args = parser.parse_args()

    tickets = parse_tickets()
    if args.offline:
        for t in tickets:
            print(f"{t['id']}: {t['title']} [lane:{t['lane']}] deps={t['deps'] or '-'} done_items={len(t['done'])}")
        print(f"{len(tickets)} tickets parsed.")
        return 0

    gh = GitHub(args.dry_run)
    repo = json.loads(gh.run("repo", "view", "--json", "nameWithOwner,visibility,defaultBranchRef"))
    slug = repo["nameWithOwner"]
    print(f"Repository: {slug} ({repo['visibility'].lower()}), default branch {repo['defaultBranchRef']['name']}")

    print("Labels")
    for name, (color, description) in LABELS.items():
        gh.run("label", "create", name, "--color", color, "--description", description, "--force", mutate=True)

    print("Merge settings")
    gh.run("repo", "edit", "--enable-squash-merge", "--enable-merge-commit=false",
           "--enable-rebase-merge=false", "--delete-branch-on-merge", "--enable-auto-merge=false", mutate=True)

    print("Ruleset")
    existing = json.loads(gh.run("api", f"repos/{slug}/rulesets", check=False) or "[]")
    if isinstance(existing, list) and any(r.get("name") == RULESET["name"] for r in existing):
        print("  'main protection' already exists; left unchanged")
    else:
        try:
            gh.run("api", "--method", "POST", f"repos/{slug}/rulesets", "--input", "-",
                   mutate=True, stdin=json.dumps(RULESET))
        except RuntimeError as error:
            if "allowed_merge_methods" in str(error):
                fallback = json.loads(json.dumps(RULESET))
                fallback["rules"][2]["parameters"].pop("allowed_merge_methods")
                gh.run("api", "--method", "POST", f"repos/{slug}/rulesets", "--input", "-",
                       mutate=True, stdin=json.dumps(fallback))
            else:
                print(f"  WARNING: could not create ruleset ({error}).")
                print("  Private repos on a free plan cannot use rulesets; make the repo public or add it by hand.")

    print("Status issue")
    issues = json.loads(gh.run("issue", "list", "--state", "all", "--limit", "500",
                               "--json", "number,title,state,labels"))
    status = next((i for i in issues if i["title"] == STATUS_TITLE and i["state"] == "OPEN"), None)
    if status:
        print(f"  #{status['number']} exists")
    else:
        url = gh.run("issue", "create", "--title", STATUS_TITLE, "--label", "autopilot",
                     "--body", STATUS_BODY, mutate=True).strip()
        if url:
            gh.run("issue", "pin", url, mutate=True, check=False)
            print(f"  created and pinned {url}")

    print("Ticket issues")
    for ticket in tickets:
        prefix = f"{ticket['id']}:"
        found = next((i for i in issues if i["title"].startswith(prefix)), None)
        if found:
            print(f"  {ticket['id']} -> #{found['number']} exists")
            continue
        url = gh.run("issue", "create", "--title", f"{ticket['id']}: {ticket['title']}",
                     "--label", "ticket", "--label", "tier-1", "--label", f"lane:{ticket['lane']}",
                     "--body", issue_body(ticket), mutate=True).strip()
        print(f"  {ticket['id']} -> {url or 'would create'}")

    print("\nDone. Remaining manual steps are in docs/autopilot.md (Greptile app, Claude GitHub app and secret).")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
