#!/usr/bin/env python3
"""Reject incompatible release immutability before any mutation."""
import json
import os
import re
import subprocess
import sys

from verify_release_environment import unique_object


def validate_policy(policy, is_versioned, immutable=None):
    # Stable and uniquely tagged nightly releases both require repository-wide
    # immutable releases. Keep the explicit argument for compatibility with
    # older unit callers that model the legacy rolling channel.
    if immutable is None:
        immutable = is_versioned
    if not isinstance(policy, dict) or type(policy.get("enabled")) is not bool:
        raise ValueError("repository release immutability cannot be proven")
    if immutable:
        if policy["enabled"] is not True:
            raise ValueError("immutable publication requires repository immutable releases enabled")
    elif policy["enabled"] is not False or policy.get("enforced_by_owner") is not False:
        raise ValueError("rolling nightly is incompatible with repository immutable releases; "
                         "no mutation is permitted until the owner resolves the release-channel policy")


def verify(repository, is_versioned, *, runner=subprocess.run, policy_token=None):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("invalid repository identity")
    policy_token = policy_token or os.environ.get("RELEASE_POLICY_READ_TOKEN")
    if not policy_token:
        raise ValueError("environment secret RELEASE_POLICY_READ_TOKEN with repository Administration(read) is required; GITHUB_TOKEN cannot supply this permission")
    read_environment = dict(os.environ)
    read_environment["GH_TOKEN"] = policy_token
    response = runner(["gh", "api", "-H", "X-GitHub-Api-Version: 2026-03-10",
                       f"repos/{repository}/immutable-releases"], capture_output=True,
                      text=True, timeout=30, check=False, env=read_environment)
    if response.returncode:
        raise ValueError("cannot read repository immutable-release policy; owner/API access setup is required, not a waiver")
    immutable = os.environ.get("RELEASE_IMMUTABLE", "") == "true"
    validate_policy(json.loads(response.stdout, object_pairs_hook=unique_object), is_versioned,
                    immutable=immutable if "RELEASE_IMMUTABLE" in os.environ else None)


if __name__ == "__main__":
    try:
        channel = os.environ.get("IS_VERSIONED")
        if channel not in {"true", "false"}:
            raise ValueError("IS_VERSIONED must be true or false")
        verify(os.environ.get("GITHUB_REPOSITORY", ""), channel == "true")
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"release policy preflight failed: {error}", file=sys.stderr)
        sys.exit(1)
    print("Verified immutable release policy" if os.environ.get("RELEASE_IMMUTABLE") == "true"
          else ("Verified immutable stable policy" if channel == "true" else "Verified mutable rolling-nightly policy"))
