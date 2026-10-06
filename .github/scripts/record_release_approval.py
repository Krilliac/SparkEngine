#!/usr/bin/env python3
"""Record and verify the protected stable-release approval event for one workflow run.

The stable-release environment requires Krilliac's review before a protected job
starts. GitHub keeps the review history per workflow run; this helper reads it
from GET /repos/{repo}/actions/runs/{run_id}/approvals, binds it to the exact
run attempt, source commit, environment id and repository owner, and emits a
closed, deterministic JSON record. Every error fails closed: an unreadable,
truncated, duplicated, rejected, foreign or non-owner review never yields a
record.

The record is deterministic so an independent consumer can rebuild it from the
API and compare SHA-256 digests with the publisher's copy. The approvals API
exposes no review timestamp, so the record bounds the approval time from below
by the run attempt start; the retained workflow artifact's upload time bounds it
from above. A sole-owner approval is one person's review, not a second human
review.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

from receipt_publication import publish_receipt_no_replace

API_ROOT = "https://api.github.com"
ENVIRONMENT = "stable-release"
OWNER_ONLY_APPROVER = "Krilliac"
WORKFLOW_PATH = ".github/workflows/release.yml"
MAX_RESPONSE_BYTES = 1024 * 1024
MAX_PAGES = 5
MAX_REVIEWS = 64
REQUEST_TIMEOUT_SECONDS = 30
LIMITATIONS = (
    "The GitHub approvals API exposes no review timestamp; approval happened after run.startedAt "
    "and before the retained approval artifact was uploaded.",
    "Review history is per workflow run; every recorded review is attributed to this run, "
    "not to an individual job or attempt.",
    "An owner-only approval is a single-person review; it is not independent second-person review.",
)


class ApprovalError(ValueError):
    """The approval event could not be proven for this exact run."""


def require(condition, message):
    if not condition:
        raise ApprovalError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate GitHub approval field: {key}")
        result[key] = value
    return result


def positive_int(value):
    return type(value) is int and value > 0


def next_page(link_header, current_url):
    """Return the rel=next URL from a Link header, or None when this is the last page."""
    if not link_header:
        return None
    for part in link_header.split(","):
        match = re.fullmatch(r'\s*<([^<>]+)>\s*;\s*rel="([^"]+)"\s*', part)
        require(match is not None, "malformed GitHub pagination header")
        if "next" in match.group(2).split():
            candidate = urllib.parse.urlsplit(match.group(1))
            current = urllib.parse.urlsplit(current_url)
            require(candidate.scheme == "https" and candidate.netloc == current.netloc
                    and candidate.path == current.path and not candidate.fragment,
                    "GitHub pagination left the requested endpoint")
            return match.group(1)
    return None


class GitHubApi:
    """Bounded, authenticated, fail-closed JSON reader for the GitHub REST API."""

    def __init__(self, token, *, api_root=API_ROOT, urlopen=None):
        require(isinstance(token, str) and token.strip() != "", "GH_TOKEN is required to read approval history")
        root = urllib.parse.urlsplit(api_root)
        require(root.scheme == "https" and root.netloc and root.path in ("", "/")
                and not root.query and not root.fragment, "GitHub API root must be an https origin")
        self._token = token
        self._root = f"https://{root.netloc}"
        self._urlopen = urlopen or urllib.request.urlopen

    def _get(self, url):
        request = urllib.request.Request(url, headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self._token}",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "SparkEngine-release-approval-recorder",
        })
        try:
            with self._urlopen(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
                status = getattr(response, "status", None)
                require(status == 200, f"GitHub returned HTTP {status} for approval evidence")
                body = response.read(MAX_RESPONSE_BYTES + 1)
                declared = response.headers.get("Content-Length")
                link = response.headers.get("Link")
        except urllib.error.URLError as error:
            raise ApprovalError(f"cannot read approval evidence from GitHub: {error}") from error
        require(len(body) <= MAX_RESPONSE_BYTES, "GitHub approval evidence exceeds the size limit")
        if declared is not None:
            require(declared.isdigit() and int(declared) == len(body), "GitHub approval evidence is truncated")
        try:
            document = json.loads(body.decode("utf-8"), object_pairs_hook=unique_object)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ApprovalError("GitHub returned invalid approval evidence") from error
        return document, link

    def object(self, path):
        document, link = self._get(f"{self._root}/{path}")
        require(isinstance(document, dict), f"GitHub {path} must be an object")
        require(next_page(link, f"{self._root}/{path}") is None, f"GitHub {path} unexpectedly paginated")
        return document

    def array(self, path):
        url = f"{self._root}/{path}?per_page=100"
        items = []
        for _ in range(MAX_PAGES):
            document, link = self._get(url)
            require(isinstance(document, list), f"GitHub {path} must be an array")
            items.extend(document)
            require(len(items) <= MAX_REVIEWS, "approval history exceeds the bounded review count")
            url = next_page(link, url)
            if url is None:
                return items
        raise ApprovalError("approval history pagination exceeds the bounded page count")


def owner_identity(repository_document, repository):
    require(repository_document.get("full_name") == repository, "repository identity differs from the requested run")
    owner = repository_document.get("owner")
    require(isinstance(owner, dict) and owner.get("login") == OWNER_ONLY_APPROVER
            and positive_int(owner.get("id")), f"repository owner must be {OWNER_ONLY_APPROVER}")
    return {"login": owner["login"], "id": owner["id"]}


def run_identity(run, *, repository, run_id, run_attempt, source_commit):
    require(run.get("id") == run_id, "approval history belongs to a different workflow run")
    require(run.get("run_attempt") == run_attempt, "approval history belongs to a different run attempt")
    require(run.get("head_sha") == source_commit, "workflow run head SHA differs from the release commit")
    require(run.get("path") == WORKFLOW_PATH, "workflow run is not the release workflow")
    repo = run.get("repository")
    require(isinstance(repo, dict) and repo.get("full_name") == repository, "workflow run belongs to another repository")
    started = run.get("run_started_at")
    require(isinstance(started, str) and re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", started),
            "workflow run has no start time")
    return {"id": run_id, "attempt": run_attempt, "startedAt": started}


def review_identity(review, *, owner, environment_id):
    require(isinstance(review, dict), "approval review must be an object")
    state = review.get("state")
    require(state != "rejected", "the stable-release deployment review was rejected")
    require(state == "approved", f"unexpected stable-release review state: {state!r}")
    environments = review.get("environments")
    require(isinstance(environments, list) and len(environments) == 1 and isinstance(environments[0], dict),
            "an approval must cover exactly the stable-release environment")
    require(environments[0].get("name") == ENVIRONMENT and environments[0].get("id") == environment_id,
            "an approval covers an environment other than stable-release")
    user = review.get("user")
    require(isinstance(user, dict) and user.get("login") == owner["login"] and user.get("id") == owner["id"],
            f"stable-release was approved by someone other than {OWNER_ONLY_APPROVER}")
    comment = review.get("comment")
    require(isinstance(comment, str), "approval comment must be a string")
    return {"state": "approved", "login": user["login"], "id": user["id"],
            "commentSha256": hashlib.sha256(comment.encode("utf-8")).hexdigest()}


def collect(api, *, repository, run_id, run_attempt, source_commit):
    """Return the closed approval record for one exact release run attempt."""
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository or ""), "invalid repository identity")
    require(positive_int(run_id) and positive_int(run_attempt), "invalid workflow run identity")
    require(re.fullmatch(r"[0-9a-f]{40}", source_commit or ""), "invalid source commit")
    owner = owner_identity(api.object(f"repos/{repository}"), repository)
    environment = api.object(f"repos/{repository}/environments/{ENVIRONMENT}")
    require(environment.get("name") == ENVIRONMENT and positive_int(environment.get("id")),
            "stable-release environment identity is missing")
    run = run_identity(api.object(f"repos/{repository}/actions/runs/{run_id}/attempts/{run_attempt}"),
                       repository=repository, run_id=run_id, run_attempt=run_attempt, source_commit=source_commit)
    reviews = api.array(f"repos/{repository}/actions/runs/{run_id}/approvals")
    # Reject first so a rejection anywhere in the history is reported as such.
    require(not any(isinstance(review, dict) and review.get("state") == "rejected" for review in reviews),
            "the stable-release deployment review was rejected")
    approvals = [review_identity(review, owner=owner, environment_id=environment["id"]) for review in reviews]
    require(approvals, "no protected stable-release approval exists for this run")
    return {
        "schemaVersion": 1,
        "kind": "spark-stable-release-approval",
        "repository": repository,
        "sourceCommit": source_commit,
        "workflow": WORKFLOW_PATH,
        "run": run,
        "environment": {"name": ENVIRONMENT, "id": environment["id"]},
        "approver": owner,
        "approvals": approvals,
        "limitations": list(LIMITATIONS),
    }


def canonical_bytes(record):
    return (json.dumps(record, indent=2, sort_keys=True) + "\n").encode("utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repository", required=True)
    parser.add_argument("--run-id", required=True, type=int)
    parser.add_argument("--run-attempt", required=True, type=int)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        api = GitHubApi(os.environ.get("GH_TOKEN", ""), api_root=os.environ.get("GITHUB_API_URL", API_ROOT))
        record = collect(api, repository=args.repository, run_id=args.run_id,
                         run_attempt=args.run_attempt, source_commit=args.source_commit)
        payload = canonical_bytes(record)
        publish_receipt_no_replace(args.output.absolute(), payload)
    except (ValueError, OSError) as error:
        print(f"release approval recording failed: {error}", file=sys.stderr)
        return 1
    print(f"Recorded {len(record['approvals'])} owner stable-release approval(s) for run "
          f"{args.run_id} attempt {args.run_attempt}: sha256={hashlib.sha256(payload).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
