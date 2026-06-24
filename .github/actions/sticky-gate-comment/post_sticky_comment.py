#!/usr/bin/env python3
"""Create or update a sticky pull-request comment via the GitHub REST API."""
from __future__ import annotations

import json
import os
import re
import sys
import urllib.error
import urllib.request


def api_request(method: str, url: str, token: str, payload: dict | None = None) -> dict | list:
    data = None
    headers = {
        "Accept": "application/vnd.github+json",
        "Authorization": f"Bearer {token}",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req) as resp:
        raw = resp.read().decode("utf-8")
        return json.loads(raw) if raw else {}


def load_body(path: str, message: str) -> str:
    if path:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    if message:
        return message
    print("::error::Either path or message must be provided", file=sys.stderr)
    sys.exit(1)


def parse_coverage_from_log(log_path: str) -> float | None:
    if not log_path or not os.path.isfile(log_path):
        return None
    text = open(log_path, encoding="utf-8", errors="replace").read()
    matches = re.findall(r"Coverage:\s*([\d.]+)%", text)
    if not matches:
        return None
    return float(matches[-1])


def coverage_banner(current_raw: str, required_raw: str, log_path: str) -> str:
    required = required_raw.strip()
    if not required:
        return ""

    current_val = None
    if current_raw.strip():
        try:
            current_val = float(current_raw.strip().rstrip("%"))
        except ValueError:
            print(f"::warning::Invalid coverage_current input: {current_raw!r}", file=sys.stderr)

    if current_val is None:
        current_val = parse_coverage_from_log(log_path)

    try:
        required_val = float(required.strip().rstrip("%"))
    except ValueError:
        print(f"::warning::Invalid coverage_required input: {required_raw!r}", file=sys.stderr)
        return ""

    if current_val is None:
        return (
            f"**Diff coverage:** _unavailable_ (required ≥ {required_val:g}%)\n\n"
        )

    status = "✅" if current_val >= required_val else "❌"
    return (
        f"**Diff coverage:** {current_val:g}% vs required ≥ {required_val:g}% {status}\n\n"
    )


def pr_number_from_event() -> int:
    event_path = os.environ.get("GITHUB_EVENT_PATH", "")
    if not event_path or not os.path.isfile(event_path):
        print("::error::GITHUB_EVENT_PATH missing; run this action on pull_request workflows", file=sys.stderr)
        sys.exit(1)
    with open(event_path, encoding="utf-8") as fh:
        event = json.load(fh)
    number = event.get("pull_request", {}).get("number")
    if not number:
        print("::error::No pull_request.number in workflow event", file=sys.stderr)
        sys.exit(1)
    return int(number)


def main() -> None:
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    repo = os.environ.get("GITHUB_REPOSITORY")
    header = os.environ.get("INPUT_HEADER", "").strip()
    path = os.environ.get("INPUT_PATH", "").strip()
    message = os.environ.get("INPUT_MESSAGE", "").strip()
    coverage_current = os.environ.get("INPUT_COVERAGE_CURRENT", "")
    coverage_required = os.environ.get("INPUT_COVERAGE_REQUIRED", "")
    diff_cover_log = os.environ.get("INPUT_DIFF_COVER_LOG", "").strip()

    if not token:
        print("::error::GITHUB_TOKEN is required", file=sys.stderr)
        sys.exit(1)
    if not repo or "/" not in repo:
        print("::error::GITHUB_REPOSITORY is required", file=sys.stderr)
        sys.exit(1)
    if not header:
        print("::error::header input is required", file=sys.stderr)
        sys.exit(1)

    owner, name = repo.split("/", 1)
    pr_number = pr_number_from_event()
    content = load_body(path, message)
    banner = coverage_banner(coverage_current, coverage_required, diff_cover_log)
    marker = f"<!-- sticky-gate-comment:{header} -->"
    body = f"{marker}\n{banner}{content.lstrip()}".rstrip() + "\n"

    base = f"https://api.github.com/repos/{owner}/{name}"
    comments = api_request("GET", f"{base}/issues/{pr_number}/comments?per_page=100", token)
    existing_id = None
    for comment in comments:
        text = comment.get("body") or ""
        if marker in text or text.startswith(marker):
            existing_id = comment["id"]
            break

    if existing_id is not None:
        api_request("PATCH", f"{base}/issues/comments/{existing_id}", token, {"body": body})
        print(f"Updated sticky gate comment #{existing_id} on PR #{pr_number}")
        return

    api_request("POST", f"{base}/issues/{pr_number}/comments", token, {"body": body})
    print(f"Created sticky gate comment on PR #{pr_number}")


if __name__ == "__main__":
    try:
        main()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        print(f"::error::GitHub API request failed ({exc.code}): {detail}", file=sys.stderr)
        sys.exit(1)
