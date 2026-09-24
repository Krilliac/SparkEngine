#!/usr/bin/env python3
"""Verify that the Working branch ruleset enforces the Required CI Gate.

The required-check policy (CONTRIBUTING.md, wiki/advanced/Testing.md) is only
true while GitHub enforces it. This verifier checks two GitHub REST responses:

* ``GET /repos/{repo}/rules/branches/Working``: the rules that apply to the
  branch right now.
* ``GET /repos/{repo}/rulesets/{id}``: the ruleset that must supply them.

It asserts that the expected repository ruleset is active, targets Working
with no bypass actors, and supplies deletion and non-fast-forward protection.
It must also require exactly the ``Required CI Gate`` check from the GitHub
Actions app. Legacy branch protection is not consulted: rulesets and branch
protection are cumulative, so legacy settings cannot weaken what this checks.

``--fixture`` checks a captured response pair and needs no network. CI uses
it to test the verifier. ``--live`` queries the API (``GITHUB_TOKEN`` is used
when set) and is the operator re-verification command. The verdict is printed
as JSON, and the exit code is 0 only when every assertion holds.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

REPOSITORY = "Krilliac/SparkEngine"
BRANCH = "Working"
RULESET_ID = 21968740
REQUIRED_CHECK_CONTEXT = "Required CI Gate"
GITHUB_ACTIONS_INTEGRATION_ID = 15368
REQUIRED_RULE_TYPES = frozenset({"deletion", "non_fast_forward", "required_status_checks"})
API_URL = "https://api.github.com"
MAX_RESPONSE_BYTES = 1024 * 1024


def _required_checks(rule: dict[str, Any]) -> list[tuple[Any, Any]]:
    parameters = rule.get("parameters")
    checks = parameters.get("required_status_checks") if isinstance(parameters, dict) else None
    if not isinstance(checks, list):
        return []
    return [
        (check.get("context"), check.get("integration_id")) if isinstance(check, dict) else (None, None)
        for check in checks
    ]


def _check_required_status_rule(rule: dict[str, Any], label: str) -> list[str]:
    checks = _required_checks(rule)
    if checks != [(REQUIRED_CHECK_CONTEXT, GITHUB_ACTIONS_INTEGRATION_ID)]:
        return [
            f"{label} must require exactly '{REQUIRED_CHECK_CONTEXT}' from integration "
            f"{GITHUB_ACTIONS_INTEGRATION_ID}; found {checks}"
        ]
    return []


def verify_branch_rules(rules: Any) -> list[str]:
    """Check the rules GitHub reports as applying to the Working branch."""
    if not isinstance(rules, list):
        return ["branch rules response is not a list"]
    errors: list[str] = []
    supplied: dict[str, dict[str, Any]] = {}
    for rule in rules:
        if not isinstance(rule, dict):
            errors.append("branch rules response contains a non-object rule")
            continue
        rule_type = rule.get("type")
        if rule_type not in REQUIRED_RULE_TYPES or rule.get("ruleset_id") != RULESET_ID:
            continue
        if rule.get("ruleset_source_type") != "Repository" or rule.get("ruleset_source") != REPOSITORY:
            errors.append(f"branch rule '{rule_type}' from ruleset {RULESET_ID} has an unexpected source")
            continue
        if rule_type in supplied:
            errors.append(f"branch rule '{rule_type}' from ruleset {RULESET_ID} appears twice")
            continue
        supplied[str(rule_type)] = rule
    for missing in sorted(REQUIRED_RULE_TYPES - set(supplied)):
        errors.append(f"ruleset {RULESET_ID} does not supply the '{missing}' rule to {BRANCH}")
    if "required_status_checks" in supplied:
        errors.extend(_check_required_status_rule(supplied["required_status_checks"], "branch required_status_checks"))
    for rule in rules:
        if isinstance(rule, dict) and rule.get("type") == "required_status_checks" and rule.get("ruleset_id") != RULESET_ID:
            errors.append(f"another ruleset ({rule.get('ruleset_id')}) adds required status checks to {BRANCH}")
    return errors


def verify_ruleset(ruleset: Any) -> list[str]:
    """Check the ruleset definition itself: identity, enforcement, scope, bypass."""
    if not isinstance(ruleset, dict):
        return ["ruleset response is not an object"]
    errors: list[str] = []
    if ruleset.get("id") != RULESET_ID:
        errors.append(f"ruleset id is {ruleset.get('id')}, expected {RULESET_ID}")
    if ruleset.get("source_type") != "Repository" or ruleset.get("source") != REPOSITORY:
        errors.append("ruleset is not owned by the repository")
    if ruleset.get("target") != "branch":
        errors.append("ruleset does not target branches")
    if ruleset.get("enforcement") != "active":
        errors.append(f"ruleset enforcement is '{ruleset.get('enforcement')}', expected 'active'")
    if ruleset.get("bypass_actors") != []:
        errors.append("ruleset declares bypass actors")
    conditions = ruleset.get("conditions")
    ref_name = conditions.get("ref_name") if isinstance(conditions, dict) else None
    include = ref_name.get("include") if isinstance(ref_name, dict) else None
    exclude = ref_name.get("exclude") if isinstance(ref_name, dict) else None
    working_ref = f"refs/heads/{BRANCH}"
    if not isinstance(include, list) or working_ref not in include:
        errors.append(f"ruleset does not include {working_ref}")
    if not isinstance(exclude, list) or exclude:
        errors.append("ruleset excludes refs")
    rules = ruleset.get("rules")
    if not isinstance(rules, list):
        return errors + ["ruleset has no rules list"]
    types = [rule.get("type") for rule in rules if isinstance(rule, dict)]
    for missing in sorted(REQUIRED_RULE_TYPES - set(types)):
        errors.append(f"ruleset does not define the '{missing}' rule")
    status_rules = [rule for rule in rules if isinstance(rule, dict) and rule.get("type") == "required_status_checks"]
    if len(status_rules) == 1:
        errors.extend(_check_required_status_rule(status_rules[0], "ruleset required_status_checks"))
    elif len(status_rules) > 1:
        errors.append("ruleset defines required_status_checks more than once")
    return errors


def verdict(rules: Any, ruleset: Any, source: str) -> dict[str, Any]:
    errors = verify_ruleset(ruleset) + verify_branch_rules(rules)
    return {
        "ok": not errors,
        "source": source,
        "repository": REPOSITORY,
        "branch": BRANCH,
        "rulesetId": RULESET_ID,
        "requiredCheck": {"context": REQUIRED_CHECK_CONTEXT, "integrationId": GITHUB_ACTIONS_INTEGRATION_ID},
        "errors": errors,
    }


def _fetch(path: str) -> Any:
    request = Request(f"{API_URL}{path}")
    request.add_header("Accept", "application/vnd.github+json")
    request.add_header("X-GitHub-Api-Version", "2022-11-28")
    token = os.environ.get("GITHUB_TOKEN", "")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urlopen(request, timeout=30) as response:  # noqa: S310 - fixed https API origin
        body = response.read(MAX_RESPONSE_BYTES + 1)
    if len(body) > MAX_RESPONSE_BYTES:
        raise ValueError(f"{path} response exceeds {MAX_RESPONSE_BYTES} bytes")
    return json.loads(body)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--live", action="store_true", help="query the GitHub REST API")
    mode.add_argument("--fixture", type=Path, help="captured {rulesForBranch, ruleset} JSON")
    args = parser.parse_args(argv)

    try:
        if args.live:
            rules = _fetch(f"/repos/{REPOSITORY}/rules/branches/{BRANCH}")
            ruleset = _fetch(f"/repos/{REPOSITORY}/rulesets/{RULESET_ID}")
            source = "live"
        else:
            captured = json.loads(args.fixture.read_text(encoding="utf-8"))
            if not isinstance(captured, dict):
                raise ValueError("fixture is not a JSON object")
            rules = captured.get("rulesForBranch")
            ruleset = captured.get("ruleset")
            source = f"fixture:{args.fixture.as_posix()}"
    except (HTTPError, URLError, OSError, ValueError) as error:
        print(json.dumps({"ok": False, "errors": [f"could not read ruleset evidence: {error}"]}, indent=2))
        return 1

    result = verdict(rules, ruleset, source)
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
