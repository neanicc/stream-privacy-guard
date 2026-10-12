#!/usr/bin/env python3
"""Summarize one pull request's merge-gate signals as JSON. HUMAN-OWNED (the merge hook runs it).

    python3 scripts/agent/pr_status.py 12
    python3 scripts/agent/pr_status.py 12 --wait 30      # poll up to 30 min until signals settle

Read-only: never comments, labels or merges. `.claude/hooks/guard_merge.py` refuses
any `gh pr merge` unless this script reports `gate.mechanical_pass` for the exact
head SHA being merged.

Trust model: every agent acts with the repository owner's GitHub credentials, so
nothing the owner's account posts can count as a review. A staff review counts only
when posted by the review workflow's GitHub App bot (user type "Bot"); Greptile
counts only from a Bot whose login contains "greptile".
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import subprocess
import sys
import time

PROTECTED = re.compile(
    r"^(eval/thresholds\.toml|\.github/|\.claude/|\.greptile/|\.agents/|scripts/agent/|"
    r"scripts/bootstrap/|AGENTS\.md$|CLAUDE\.md$|LICENSE$)"
)
REVIEW_MARKER = re.compile(r"<!--\s*spg-staff-review\s+head=([0-9a-f]{40})\s*-->")
REVIEW_SUMMARY = re.compile(
    r"SPG-REVIEW:\s*verdict=(?P<verdict>[a-z-]+)\s+score=(?P<score>\d+)\s+"
    r"p0=(?P<p0>\d+)\s+p1=(?P<p1>\d+)\s+p2=(?P<p2>\d+)\s+p3=(?P<p3>\d+)"
)
GREPTILE_CONFIDENCE = re.compile(r"confidence[^0-9\n]{0,60}(\d)\s*/\s*5", re.IGNORECASE)
CLOSES = re.compile(r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\s+#(\d+)", re.IGNORECASE)
REVIEW_BOTS = {"github-actions[bot]", "claude[bot]"}
APPROVING = {"approve", "approve-with-nits"}
MIN_SCORE = 85
CI_WORKFLOW, CI_JOB = "Check", "check"
REVIEW_WORKFLOW, REVIEW_JOB = "Claude staff review", "review"
ACCEPTABLE_MERGE_STATES = {"CLEAN", "UNSTABLE", "HAS_HOOKS", "BEHIND"}
FAILED = {"FAILURE", "TIMED_OUT", "CANCELLED", "ERROR", "ACTION_REQUIRED", "STARTUP_FAILURE", "STALE"}

THREADS_QUERY = """
query($owner: String!, $name: String!, $number: Int!) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $number) {
      reviewThreads(first: 100) {
        nodes { isResolved comments(first: 1) { nodes { author { login } } } }
      }
    }
  }
}
"""


def gh(*args: str) -> str:
    result = subprocess.run(["gh", *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"gh {' '.join(args[:3])} failed: {result.stderr.strip()}")
    return result.stdout


def gh_json(*args: str):
    return json.loads(gh(*args))


def gh_lines(*args: str) -> list[dict]:
    """`gh api --paginate --jq '.[] | {...}'` output: one JSON object per line."""
    return [json.loads(line) for line in gh(*args).splitlines() if line.strip()]


def parse_time(value: str | None) -> dt.datetime:
    floor = dt.datetime.min.replace(tzinfo=dt.timezone.utc)
    if not value:
        return floor
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return floor
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=dt.timezone.utc)


def latest_run(rollup: list[dict], workflow: str, job: str) -> dict | None:
    """Latest CheckRun for one workflow job. Commit statuses never count."""
    runs = [r for r in rollup or [] if r.get("__typename") == "CheckRun"
            and r.get("name") == job and (r.get("workflowName") or "") == workflow]
    if not runs:
        return None
    # A queued or running run is the newest by definition, even without a start time.
    run = max(runs, key=lambda r: ((r.get("status") or "").upper() != "COMPLETED",
                                   parse_time(r.get("startedAt")), parse_time(r.get("completedAt"))))
    return {"status": (run.get("status") or "").upper(), "conclusion": (run.get("conclusion") or "").upper()}


def summarize(number: int, slug: str, default_branch: str, greptile_grace_min: int) -> dict:
    pr = gh_json("pr", "view", str(number), "--json",
                 "number,url,state,isDraft,headRefOid,headRefName,baseRefName,mergeable,"
                 "mergeStateStatus,body,labels,statusCheckRollup,createdAt")
    head = pr["headRefOid"]
    owner, name = slug.split("/", 1)
    rollup = pr.get("statusCheckRollup") or []

    ci_run = latest_run(rollup, CI_WORKFLOW, CI_JOB)
    if ci_run is None:
        ci_state = "MISSING"
    elif ci_run["status"] != "COMPLETED":
        ci_state = "PENDING"
    else:
        ci_state = ci_run["conclusion"] or "UNKNOWN"
    latest_by_job: dict = {}
    for r in rollup:
        if r.get("__typename") != "CheckRun":
            continue
        key = (r.get("workflowName") or "", r.get("name") or "?")
        rank = ((r.get("status") or "").upper() != "COMPLETED", parse_time(r.get("startedAt")))
        if key not in latest_by_job or rank >= latest_by_job[key][0]:
            latest_by_job[key] = (rank, r)
    latest_by_job = {k: v[1] for k, v in latest_by_job.items()}
    failing = sorted(f"{wf}/{job}" if wf else job for (wf, job), r in latest_by_job.items()
                     if (r.get("conclusion") or "").upper() in FAILED)
    review_run = latest_run(rollup, REVIEW_WORKFLOW, REVIEW_JOB)

    comments = gh_lines("api", "--paginate", f"repos/{slug}/issues/{number}/comments", "--jq",
                        ".[] | {login: .user.login, type: .user.type, body: (.body // \"\"), at: .created_at}")
    reviews = gh_lines("api", "--paginate", f"repos/{slug}/pulls/{number}/reviews", "--jq",
                       ".[] | {login: .user.login, type: .user.type, body: (.body // \"\"), at: .submitted_at}")
    posts = sorted(comments + reviews, key=lambda p: p.get("at") or "")

    review = {"found": False, "stale_reports": 0, "untrusted_reports": 0,
              "workflow": "ABSENT" if review_run is None else (review_run["conclusion"] or review_run["status"])}
    for post in posts:
        marker = REVIEW_MARKER.search(post["body"])
        if not marker:
            continue
        if post["type"] != "Bot" or post["login"] not in REVIEW_BOTS:
            review["untrusted_reports"] += 1
            continue
        if marker.group(1) != head:
            review["stale_reports"] += 1
            continue
        summary = REVIEW_SUMMARY.search(post["body"])
        if summary:
            review.update(found=True, author=post["login"], verdict=summary["verdict"],
                          score=int(summary["score"]), p0=int(summary["p0"]), p1=int(summary["p1"]),
                          p2=int(summary["p2"]), p3=int(summary["p3"]))

    greptile_posts = [p["body"] for p in posts if p["type"] == "Bot" and "greptile" in p["login"].lower()]
    confidences = [int(m.group(1)) for body in greptile_posts if (m := GREPTILE_CONFIDENCE.search(body))]
    age_min = (dt.datetime.now(dt.timezone.utc) - parse_time(pr["createdAt"])).total_seconds() / 60
    greptile = {"reviewed": bool(greptile_posts), "confidence": confidences[-1] if confidences else None,
                "grace_expired": age_min >= greptile_grace_min}

    threads: dict = {"unresolved": None, "by_author": {}}
    try:
        data = gh_json("api", "graphql", "-f", f"query={THREADS_QUERY}", "-F", f"owner={owner}",
                       "-F", f"name={name}", "-F", f"number={number}")
        nodes = data["data"]["repository"]["pullRequest"]["reviewThreads"]["nodes"]
        unresolved = [n for n in nodes if not n["isResolved"]]
        threads["unresolved"] = len(unresolved)
        for node in unresolved:
            first = (node["comments"]["nodes"] or [{}])[0]
            key = ((first.get("author") or {}).get("login")) or "?"
            threads["by_author"][key] = threads["by_author"].get(key, 0) + 1
    except (RuntimeError, KeyError, TypeError) as error:
        threads["error"] = str(error)

    files = gh_lines("api", "--paginate", f"repos/{slug}/pulls/{number}/files", "--jq",
                     ".[] | {filename, previous: (.previous_filename // \"\")}")
    paths = {f["filename"] for f in files} | {f["previous"] for f in files if f["previous"]}
    protected = sorted(p for p in paths if PROTECTED.match(p))
    closes = sorted({int(n) for n in CLOSES.findall(pr.get("body") or "")})
    labels = sorted(label["name"] for label in pr.get("labels") or [])

    reasons = []
    if pr["state"] != "OPEN":
        reasons.append(f"PR is {pr['state']}")
    if pr["isDraft"]:
        reasons.append("PR is a draft")
    if pr["baseRefName"] != default_branch:
        reasons.append(f"base is {pr['baseRefName']}, not {default_branch}")
    if ci_state != "SUCCESS":
        reasons.append(f"{CI_WORKFLOW}/{CI_JOB} is {ci_state}" + (f"; failing: {', '.join(failing)}" if failing else ""))
    if pr["mergeable"] != "MERGEABLE" or pr["mergeStateStatus"] not in ACCEPTABLE_MERGE_STATES:
        reasons.append(f"mergeable={pr['mergeable']} mergeState={pr['mergeStateStatus']}")
    if not review["found"]:
        reasons.append(f"no bot-posted staff review for head {head[:12]} (review workflow: {review['workflow']})")
    else:
        if review["verdict"] not in APPROVING:
            reasons.append(f"staff review verdict is {review['verdict']}")
        if review["score"] < MIN_SCORE:
            reasons.append(f"staff review score {review['score']} < {MIN_SCORE}")
        if review["p0"] or review["p1"]:
            reasons.append(f"staff review has P0={review['p0']} P1={review['p1']}")
    if threads["unresolved"] is None:
        reasons.append("could not read review threads")
    elif threads["unresolved"]:
        reasons.append(f"{threads['unresolved']} unresolved review thread(s)")
    if not greptile["reviewed"] and not greptile["grace_expired"]:
        reasons.append("waiting for Greptile (grace period not over)")
    if protected:
        reasons.append("touches human-owned paths: " + ", ".join(protected))
    if not closes:
        reasons.append("PR body has no 'Closes #<issue>'")
    if "needs-human" in labels:
        reasons.append("labeled needs-human")

    return {
        "pr": pr["number"], "url": pr["url"], "head": head, "branch": pr["headRefName"],
        "base": pr["baseRefName"], "state": pr["state"], "draft": pr["isDraft"],
        "mergeable": pr["mergeable"], "merge_state": pr["mergeStateStatus"], "labels": labels,
        "closes": closes, "protected_paths": protected,
        "ci": {"check": ci_state, "failing": failing},
        "claude_review": review, "greptile": greptile, "threads": threads,
        "gate": {"mechanical_pass": not reasons, "reasons": reasons},
    }


def settled(summary: dict) -> bool:
    ci_done = summary["ci"]["check"] not in {"PENDING", "MISSING"}
    mergeability_known = summary["mergeable"] != "UNKNOWN"
    review = summary["claude_review"]
    review_done = review["found"] or review["workflow"] in FAILED | {"SUCCESS", "NEUTRAL", "SKIPPED"}
    greptile_done = summary["greptile"]["reviewed"] or summary["greptile"]["grace_expired"]
    return ci_done and mergeability_known and review_done and greptile_done


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pr", type=int)
    parser.add_argument("--wait", type=int, default=0, metavar="MINUTES", help="poll until signals settle")
    parser.add_argument("--greptile-grace", type=int, default=20, metavar="MINUTES")
    args = parser.parse_args()

    repo = gh_json("repo", "view", "--json", "nameWithOwner,defaultBranchRef")
    slug, default_branch = repo["nameWithOwner"], repo["defaultBranchRef"]["name"]
    deadline = time.monotonic() + args.wait * 60
    failures = 0
    while True:
        try:
            summary = summarize(args.pr, slug, default_branch, args.greptile_grace)
            failures = 0
        except (RuntimeError, ValueError):  # gh failure or unparseable output: retry while waiting
            failures += 1
            if not args.wait or failures >= 5 or time.monotonic() >= deadline:
                raise
            time.sleep(30)
            continue
        done = settled(summary)
        if not args.wait or done or time.monotonic() >= deadline:
            summary["settled"] = done
            print(json.dumps(summary, indent=2))
            return 0
        time.sleep(60)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, ValueError) as error:
        print(json.dumps({"error": str(error)}), file=sys.stderr)
        sys.exit(1)
