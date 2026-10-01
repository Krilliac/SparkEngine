"""Write and validate the public, API-checkable site publication status.

status.json is separate from latest.json so a failed Build run can change the
visible status without replacing the last good content snapshot. The status is
an assertion until a consumer checks it against GitHub's public Actions API.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

from common import SiteDataError, load_json, write_json


REPOSITORY = "Krilliac/SparkEngine"
WORKFLOW_NAME = "Build SparkEngine"
WORKFLOW_PATH = ".github/workflows/build.yml"
SHA_PATTERN = re.compile(r"[0-9a-f]{40}\Z")
CONCLUSIONS = frozenset({
    "success", "failure", "cancelled", "timed_out", "action_required",
    "neutral", "skipped", "stale",
})
STATUS_MAX_BYTES = 8 * 1024
RUN_MAX_BYTES = 2 * 1024 * 1024


def _positive_int(value: Any, label: str) -> int:
    if type(value) is not int or value < 1:
        raise SiteDataError(f"{label} must be a positive integer")
    return value


def _commit(value: Any, label: str) -> str:
    if not isinstance(value, str) or not SHA_PATTERN.fullmatch(value):
        raise SiteDataError(f"{label} must be a full lowercase Git commit SHA")
    return value


def status_from_run(run: Any, *, run_id: int, attempt: int) -> dict[str, Any]:
    """Accept only a completed Build run on the trusted Working repository."""
    if not isinstance(run, dict):
        raise SiteDataError("Build run API response must be an object")
    if type(run.get("id")) is not int or run["id"] != _positive_int(run_id, "requested run id"):
        raise SiteDataError("Build run id differs from the requested run")
    if type(run.get("run_attempt")) is not int or run["run_attempt"] != _positive_int(attempt, "requested attempt"):
        raise SiteDataError("Build run attempt differs from the requested attempt")
    if (run.get("name") != WORKFLOW_NAME or run.get("path") != WORKFLOW_PATH or
            run.get("head_branch") != "Working" or run.get("event") not in {"push", "workflow_dispatch"} or
            run.get("status") != "completed"):
        raise SiteDataError("Build run workflow, branch, event, or completion state is invalid")
    conclusion = run.get("conclusion")
    if conclusion not in CONCLUSIONS:
        raise SiteDataError("Build run has no recognized completed conclusion")
    repository = run.get("repository")
    head_repository = run.get("head_repository")
    if (not isinstance(repository, dict) or not isinstance(head_repository, dict) or
            repository.get("full_name") != REPOSITORY or
            head_repository.get("full_name") != REPOSITORY):
        raise SiteDataError("Build run does not belong to the trusted repository")
    sha = _commit(run.get("head_sha"), "Build run head_sha")
    return {
        "schemaVersion": 1,
        "repository": REPOSITORY,
        "sourceCommit": sha,
        "state": "current" if conclusion == "success" else "blocked",
        "run": {
            "id": run_id,
            "attempt": attempt,
            "headSha": sha,
            "conclusion": conclusion,
            "url": f"https://github.com/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}",
        },
    }


def validate_status(status: Any, *, run: Any | None = None) -> dict[str, Any]:
    if not isinstance(status, dict) or set(status) != {
        "schemaVersion", "repository", "sourceCommit", "contentCommit", "state", "run"
    }:
        raise SiteDataError("status.json has an invalid field set")
    if status["schemaVersion"] != 1 or status["repository"] != REPOSITORY:
        raise SiteDataError("status.json schema or repository is invalid")
    source_commit = _commit(status["sourceCommit"], "status sourceCommit")
    content_commit = status["contentCommit"]
    if content_commit is not None:
        _commit(content_commit, "status contentCommit")
    evidence = status["run"]
    if not isinstance(evidence, dict) or set(evidence) != {"id", "attempt", "headSha", "conclusion", "url"}:
        raise SiteDataError("status.json run has an invalid field set")
    run_id = _positive_int(evidence["id"], "status run id")
    attempt = _positive_int(evidence["attempt"], "status run attempt")
    if evidence["conclusion"] not in CONCLUSIONS:
        raise SiteDataError("status.json run conclusion is invalid")
    if (_commit(evidence["headSha"], "status run headSha") != source_commit or
            evidence["url"] != f"https://github.com/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}"):
        raise SiteDataError("status.json run identity differs from its source commit")
    expected_state = "current" if evidence["conclusion"] == "success" else "blocked"
    if status["state"] != expected_state:
        raise SiteDataError("status.json state differs from its run conclusion")
    if expected_state == "current" and content_commit != source_commit:
        raise SiteDataError("current status must point to same-commit content")
    if run is not None:
        expected = status_from_run(run, run_id=run_id, attempt=attempt)
        if {key: value for key, value in status.items() if key != "contentCommit"} != expected:
            raise SiteDataError("status.json differs from the GitHub Build run response")
    return status


def content_commit_in(payload_dir: Path) -> str | None:
    latest_path = payload_dir / "latest.json"
    if not latest_path.exists():
        return None
    latest = load_json(latest_path, maximum=32 * 1024)
    if not isinstance(latest, dict) or not isinstance(latest.get("source"), dict):
        raise SiteDataError("retained latest.json has no source object")
    return _commit(latest["source"].get("commit"), "retained latest.json source.commit")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("write", "verify"))
    parser.add_argument("--run-file", required=True, type=Path)
    parser.add_argument("--run-id", required=True, type=int)
    parser.add_argument("--run-attempt", required=True, type=int)
    parser.add_argument("--payload-dir", type=Path)
    parser.add_argument("--status-file", type=Path)
    args = parser.parse_args()
    try:
        run = load_json(args.run_file, maximum=RUN_MAX_BYTES)
        expected = status_from_run(run, run_id=args.run_id, attempt=args.run_attempt)
        if args.mode == "write":
            if args.payload_dir is None or args.status_file is not None:
                raise SiteDataError("write requires --payload-dir only")
            try:
                expected["contentCommit"] = content_commit_in(args.payload_dir)
            except SiteDataError:
                if expected["state"] == "current":
                    raise
                # A broken retained pointer must not prevent a verified
                # failed-run status. The client will show no old content.
                expected["contentCommit"] = None
            validate_status(expected, run=run)
            status_path = args.payload_dir / "status.json"
            if status_path.is_symlink():
                raise SiteDataError("status.json must not be a symlink")
            write_json(status_path, expected, pretty=True)
        else:
            if args.status_file is None or args.payload_dir is not None:
                raise SiteDataError("verify requires --status-file only")
            status = load_json(args.status_file, maximum=STATUS_MAX_BYTES)
            validate_status(status, run=run)
        print(f"Verified site status for Build run {args.run_id} attempt {args.run_attempt}: {expected['state']}")
        return 0
    except (SiteDataError, OSError, KeyError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
