#!/usr/bin/env python3
"""Verify the protected v0.9.0 source-seal publication boundary."""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

SHA_RE = re.compile(r"^[0-9a-f]{40}$")
ENVIRONMENT = "stable-release"
VARIABLE = "SPARKENGINE_V090_REVIEWED_SHA"
ALLOWED_PREFIXES = ("docs/readiness/work-items/",)
ALLOWED_FILES = {
    "docs/site/readiness.json",
    "docs/readiness/ENGINE_READINESS_HANDOFF.md",
}


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def validate_seal(source_sha: str, baseline_commit: str, parents: list[str], changed_paths: list[str], reviewed_sha: str) -> dict:
    """Validate the immutable metadata-only seal without mutating repository state."""
    if not SHA_RE.fullmatch(source_sha or ""):
        raise ValueError("source SHA must be 40 lowercase hexadecimal characters")
    if not SHA_RE.fullmatch(baseline_commit or ""):
        raise ValueError("baseline commit must be 40 lowercase hexadecimal characters")
    if parents != [baseline_commit]:
        raise ValueError("v0.9.0 source seal must have exactly one parent equal to baselineCommit")
    if reviewed_sha != source_sha:
        raise ValueError("protected reviewed SHA does not equal the release source SHA")
    invalid = [path for path in changed_paths
               if path not in ALLOWED_FILES and not any(path.startswith(prefix) for prefix in ALLOWED_PREFIXES)]
    if invalid:
        raise ValueError("source-seal commit changes non-readiness paths: " + ", ".join(sorted(invalid)))
    return {
        "source_sha": source_sha,
        "baseline_commit": baseline_commit,
        "reviewed_sha": reviewed_sha,
        "environment": ENVIRONMENT,
        "variable": VARIABLE,
        "changed_paths": sorted(changed_paths),
    }


def _api_variable(repository: str, token: str, *, runner=subprocess.run) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("invalid repository identity")
    if not token:
        raise ValueError("RELEASE_POLICY_READ_TOKEN is required to read the stable-release source seal")
    endpoint = f"repos/{repository}/environments/{ENVIRONMENT}/variables/{VARIABLE}"
    response = runner(["gh", "api", endpoint], capture_output=True, text=True,
                      timeout=30, check=False,
                      env={**os.environ, "GH_TOKEN": token})
    if response.returncode:
        raise ValueError("cannot read the protected stable-release source seal")
    try:
        payload = json.loads(response.stdout, object_pairs_hook=_unique_object)
    except (TypeError, ValueError) as error:
        raise ValueError("GitHub returned invalid source-seal evidence") from error
    if not isinstance(payload, dict) or set(payload) - {"name", "value", "created_at", "updated_at"}:
        raise ValueError("GitHub returned an unexpected source-seal variable shape")
    if payload.get("name") != VARIABLE or not isinstance(payload.get("value"), str) or not SHA_RE.fullmatch(payload["value"]):
        raise ValueError("protected source-seal variable is missing or malformed")
    return payload["value"]


def verify(repository: str, source_sha: str, baseline_commit: str, *, token: str | None = None,
           runner=subprocess.run, git_runner=subprocess.run) -> dict:
    token = token if token is not None else os.environ.get("RELEASE_POLICY_READ_TOKEN", "")
    parent_result = git_runner(["git", "rev-list", "--parents", "-n", "1", source_sha],
                               capture_output=True, text=True, timeout=30, check=False)
    if parent_result.returncode:
        raise ValueError("cannot resolve source-seal commit parents")
    fields = parent_result.stdout.strip().split()
    if not fields or fields[0] != source_sha:
        raise ValueError("source-seal parent evidence is not bound to the requested source SHA")
    parents = fields[1:]
    diff_result = git_runner(["git", "diff", "--name-only", "--diff-filter=ACDMRTUXB", f"{baseline_commit}^{{commit}}", source_sha],
                             capture_output=True, text=True, timeout=30, check=False)
    if diff_result.returncode:
        raise ValueError("cannot resolve source-seal changed paths")
    reviewed_sha = _api_variable(repository, token, runner=runner)
    return validate_seal(source_sha, baseline_commit, parents,
                         [line for line in diff_result.stdout.splitlines() if line], reviewed_sha)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--baseline-commit", required=True)
    parser.add_argument("--report", type=str)
    args = parser.parse_args()
    try:
        evidence = verify(args.repository, args.source_sha, args.baseline_commit)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"v0.9.0 source-seal verification failed: {error}", file=sys.stderr)
        return 1
    encoded = json.dumps(evidence, sort_keys=True) + "\n"
    if args.report:
        with open(args.report, "x", encoding="utf-8", newline="\n") as stream:
            stream.write(encoded)
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
