#!/usr/bin/env python3
"""Check the conservative pre-release support table against repository policy.

This offline guard does not prove publication. It permits the configured Working
development channel and the explicitly unpublished stable-v1 profile. Naming a
supported version needs a future reviewed publication-evidence contract; neither
local tags nor removing a disclaimer can authorize that claim.
"""

from __future__ import annotations

import re
from pathlib import Path

VERSION_LITERAL = re.compile(r"\bv?[0-9]+\.[0-9]+(?:\.(?:[0-9]+|x))?(?:[-+][0-9A-Za-z.-]+)?\b")
POLICY_STATUS = {
    "stable-v1": "Pre-release and blocked; no supported version has been published",
    "Working": "Development channel only; fixes are best-effort and do not constitute a release SLA",
}


def configured_development_channels(repo_root: Path) -> set[str]:
    """Require Working in the checked-in site validation workflow's branch list.

    A shallow detached CI checkout need not have the branch ref. The workflow
    declares a development channel, not a released product or a support SLA.
    """
    workflow = (repo_root / ".github/workflows/site-data.yml").read_text(encoding="utf-8")
    branch_lists = re.findall(r"^\s+branches:\s*\[([^]\n]+)\]", workflow, re.MULTILINE)
    return {"Working"} if any(
        "Working" in {value.strip().strip("\"'") for value in branches.split(",")}
        for branches in branch_lists
    ) else set()


def _table_rows(text: str) -> list[tuple[str, str]]:
    """Read pipe-table rows from the Supported Versions section only."""
    if text.count("## Supported Versions") != 1:
        return []
    section = text.split("## Supported Versions", 1)[1].split("\n## ", 1)[0]
    rows: list[tuple[str, str]] = []
    for line in section.splitlines():
        match = re.match(r"^\|\s*`?([^|`]+)`?\s*\|\s*([^|]+)\|", line)
        if match and match.group(1).strip().lower() not in {"release line", "---"}:
            rows.append((match.group(1).strip(), match.group(2).strip()))
    return rows


def _errors_for_text(text: str, channels: set[str], *, label: str) -> list[str]:
    errors: list[str] = []
    # Only the policy preamble and supported-version section carry release
    # declarations. SECURITY's later threat-model prose also contains IPs and
    # protocol versions, which are not product release claims.
    policy_text = text.split("\n## Reporting a Vulnerability", 1)[0] if label == "SECURITY.md" else text
    for version in VERSION_LITERAL.findall(policy_text):
        errors.append(f"{label} names version {version} without reviewed publication evidence")
    allowed = (set(channels) & {"Working"}) | {"stable-v1"}
    rows = _table_rows(text)
    if not rows:
        errors.append(f"{label} must contain a Supported Versions table")
    if len(rows) != len({channel for channel, _ in rows}):
        errors.append(f"{label} repeats a policy channel")
    if {channel for channel, _ in rows} != set(POLICY_STATUS):
        errors.append(f"{label} must declare the Working development channel and unpublished stable-v1 profile")
    for channel, status in rows:
        if channel not in allowed:
            errors.append(f"{label} names unavailable channel or release line {channel}")
        if channel in POLICY_STATUS and status != POLICY_STATUS[channel]:
            errors.append(f"{label} has unreviewed support status for {channel}")
        if VERSION_LITERAL.fullmatch(channel):
            errors.append(f"{label} names unpublished release line {channel}")
    return errors


def validate_security_text(text: str, channels: set[str]) -> list[str]:
    """Validate SECURITY.md's supported-version and reporting claims."""
    errors = _errors_for_text(text, channels, label="SECURITY.md")
    if "GitHub Security Advisories" not in text:
        errors.append("SECURITY.md must name GitHub Security Advisories as the private reporting channel")
    if "does not currently promise" not in text:
        errors.append("SECURITY.md must state that response timelines are best-effort")
    return errors


def validate_support_text(text: str, channels: set[str]) -> list[str]:
    """Validate SUPPORT.md's channel and release wording."""
    errors = _errors_for_text(text, channels, label="SUPPORT.md")
    if "github.com/Krilliac/SparkEngine/issues" not in text:
        errors.append("SUPPORT.md must name the repository-backed GitHub Issues channel")
    if "best-effort" not in text.lower():
        errors.append("SUPPORT.md must state that development-channel support is best-effort")
    return errors


def validate(repo_root: Path) -> list[str]:
    channels = configured_development_channels(repo_root)
    security = (repo_root / "SECURITY.md").read_text(encoding="utf-8")
    support = (repo_root / "SUPPORT.md").read_text(encoding="utf-8")
    return validate_security_text(security, channels) + validate_support_text(support, channels)


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    errors = validate(root)
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1
    print("governance policy tables match the configured development channel and pre-release boundary")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
