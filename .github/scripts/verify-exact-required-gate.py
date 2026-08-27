#!/usr/bin/env python3
"""Fail closed unless an exact commit has a successful Required CI Gate."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
import re
import sys
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


FetchJson = Callable[[str], Any]
ALLOWED_EVENTS = frozenset({"push", "workflow_dispatch"})
SHA_PATTERN = re.compile(r"[0-9a-fA-F]{40}")


@dataclass(frozen=True)
class GateEvidence:
    run_id: int
    run_url: str
    event: str


def _object(payload: Any, label: str) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValueError(f"{label} response must be an object")
    return payload


def verify_exact_gate(fetch_json: FetchJson, repository: str, target_sha: str) -> GateEvidence:
    if repository.count("/") != 1 or any(not part for part in repository.split("/")):
        raise ValueError("repository must have the form owner/name")
    if not SHA_PATTERN.fullmatch(target_sha):
        raise ValueError("target SHA must be a full 40-character hexadecimal commit ID")

    query = urlencode({"head_sha": target_sha.lower(), "status": "completed", "per_page": 100})
    runs_payload = _object(
        fetch_json(f"/repos/{repository}/actions/workflows/build.yml/runs?{query}"),
        "workflow runs",
    )
    runs = runs_payload.get("workflow_runs")
    if not isinstance(runs, list):
        raise ValueError("workflow runs response is missing workflow_runs[]")

    candidates: list[dict[str, Any]] = []
    for run in runs:
        if not isinstance(run, dict):
            raise ValueError("workflow_runs[] entries must be objects")
        if str(run.get("head_sha", "")).lower() != target_sha.lower():
            continue
        if run.get("event") not in ALLOWED_EVENTS or run.get("status") != "completed":
            continue
        if run.get("conclusion") != "success":
            continue
        candidates.append(run)

    for run in candidates:
        run_id = run.get("id")
        if not isinstance(run_id, int) or run_id <= 0:
            raise ValueError("candidate workflow run has an invalid id")
        jobs_payload = _object(
            fetch_json(f"/repos/{repository}/actions/runs/{run_id}/jobs?filter=latest&per_page=100"),
            "workflow jobs",
        )
        jobs = jobs_payload.get("jobs")
        if not isinstance(jobs, list):
            raise ValueError("workflow jobs response is missing jobs[]")
        for job in jobs:
            if not isinstance(job, dict):
                raise ValueError("jobs[] entries must be objects")
            if job.get("name") != "Required CI Gate":
                continue
            if job.get("status") == "completed" and job.get("conclusion") == "success":
                return GateEvidence(
                    run_id=run_id,
                    run_url=str(run.get("html_url", "")),
                    event=str(run["event"]),
                )

    raise ValueError(
        f"no completed successful Required CI Gate from push/workflow_dispatch certifies {target_sha}"
    )


def github_fetcher(api_url: str, token: str) -> FetchJson:
    if not token:
        raise ValueError("GH_TOKEN is required")

    def fetch(path: str) -> Any:
        request = Request(
            api_url.rstrip("/") + path,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {token}",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "SparkEngine-exact-required-gate",
            },
        )
        try:
            with urlopen(request, timeout=30) as response:  # noqa: S310 - fixed GitHub API base.
                return json.load(response)
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as error:
            raise RuntimeError(f"GitHub API request failed for {path}: {error}") from error

    return fetch


def main() -> int:
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    target_sha = os.environ.get("TARGET_SHA", "")
    try:
        evidence = verify_exact_gate(
            github_fetcher(os.environ.get("GITHUB_API_URL", "https://api.github.com"), os.environ.get("GH_TOKEN", "")),
            repository,
            target_sha,
        )
    except (RuntimeError, ValueError) as error:
        print(f"error: exact-SHA Required CI Gate verification failed: {error}", file=sys.stderr)
        return 1

    print(
        f"Required CI Gate certified {target_sha} in run {evidence.run_id} "
        f"({evidence.event}): {evidence.run_url}"
    )
    output_path = os.environ.get("GITHUB_OUTPUT")
    if output_path:
        with open(output_path, "a", encoding="utf-8", newline="\n") as stream:
            stream.write(f"run_id={evidence.run_id}\n")
            stream.write(f"run_url={evidence.run_url}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
