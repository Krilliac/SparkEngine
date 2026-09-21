#!/usr/bin/env python3
"""Read-only, fail-closed preflight for the stable-release GitHub environment."""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys

ENVIRONMENT = "stable-release"


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate environment protection field: {key}")
        result[key] = value
    return result


def protection_errors(environment, policies):
    errors = []
    if not isinstance(environment, dict) or environment.get("name") != ENVIRONMENT:
        return ["the stable-release environment does not exist or has the wrong identity"]
    if environment.get("can_admins_bypass") is not False:
        errors.append("disable administrative protection bypass and expose can_admins_bypass=false")
    rules = environment.get("protection_rules", [])
    if not isinstance(rules, list):
        return ["environment protection_rules must be an array"]
    reviewers = [rule for rule in rules if isinstance(rule, dict) and rule.get("type") == "required_reviewers"]
    if len(reviewers) != 1:
        errors.append("configure exactly one required-reviewers rule")
    else:
        rule = reviewers[0]
        if rule.get("prevent_self_review") is not True:
            errors.append("enable prevention of self-review")
        assigned = rule.get("reviewers")
        if not isinstance(assigned, list) or not assigned or any(
            not isinstance(entry, dict) or entry.get("type") not in {"User", "Team"}
            or not isinstance(entry.get("reviewer"), dict)
            or type(entry["reviewer"].get("id")) is not int or entry["reviewer"]["id"] <= 0
            for entry in assigned or []
        ):
            errors.append("assign at least one real required reviewer")
    if environment.get("deployment_branch_policy") != {"protected_branches": False, "custom_branch_policies": True}:
        errors.append("restrict deployment to the custom Working branch policy")
    entries = policies.get("branch_policies") if isinstance(policies, dict) else None
    if (not isinstance(entries, list) or len(entries) != 1
            or type(policies.get("total_count")) is not int or policies["total_count"] != 1
            or not isinstance(entries[0], dict)
            or entries[0].get("name") != "Working" or entries[0].get("type") != "branch"):
        errors.append("allow exactly the Working branch, with no wildcard or tag policy")
    return errors


def verify(repository, *, runner=subprocess.run):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("invalid repository identity")
    documents = []
    for suffix in ("", "/deployment-branch-policies?per_page=100"):
        response = runner(["gh", "api", f"repos/{repository}/environments/{ENVIRONMENT}{suffix}"],
                          capture_output=True, text=True, timeout=30, check=False)
        if response.returncode:
            raise ValueError("cannot prove stable-release protection through the GitHub API; "
                             "the repository owner must provision the environment and permit Actions read access; "
                             "API denial is not an approval waiver")
        try:
            documents.append(json.loads(response.stdout, object_pairs_hook=unique_object))
        except (TypeError, json.JSONDecodeError) as error:
            raise ValueError("GitHub returned invalid environment protection evidence") from error
    errors = protection_errors(*documents)
    if errors:
        raise ValueError("stable-release is not protected: " + "; ".join(errors))


if __name__ == "__main__":
    try:
        verify(os.environ.get("GITHUB_REPOSITORY", ""))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"release environment preflight failed: {error}", file=sys.stderr)
        sys.exit(1)
    print("Verified stable-release required reviewers, self-review prevention, and exact Working policy")
