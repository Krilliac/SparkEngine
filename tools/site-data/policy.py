#!/usr/bin/env python3
"""Check the conservative pre-release support table against repository policy.

The offline guard does not prove publication. It permits the configured Working
development channel, the nightly prerelease channel the release workflow publishes
from it, and the explicitly unpublished stable-v1 profile. Naming a supported
version needs a future reviewed publication-evidence contract; neither local tags
nor removing a disclaimer can authorize that claim.

validate_against_releases() is the published-release half: given the GitHub
releases list (``policy.py --published-releases <json>`` in the build workflow's
license-compliance job), the tables must name what is actually published, no
more and no less.
"""

from __future__ import annotations

import argparse
import importlib.util
import re
from pathlib import Path
from typing import Any

VERSION_LITERAL = re.compile(r"\bv?[0-9]+\.[0-9]+(?:\.(?:[0-9]+|x))?(?:[-+][0-9A-Za-z.-]+)?\b")
POLICY_STATUS = {
    "stable-v1": "Pre-release and blocked; no supported version has been published",
    "Working": "Development channel only; fixes are best-effort and do not constitute a release SLA",
    "nightly": "Unsupported prerelease builds of `Working`; fixes are best-effort and do not constitute a release SLA",
}


def configured_development_channels(repo_root: Path) -> set[str]:
    """Return the development channels the checked-in workflows publish.

    Working must appear in the site validation workflow's branch list (a shallow
    detached CI checkout need not have the branch ref). nightly is a channel only
    while the release workflow publishes nightly prereleases through its
    nightly-release environment and tag helper. Neither is a released product or
    a support SLA.
    """
    workflow = (repo_root / ".github/workflows/site-data.yml").read_text(encoding="utf-8")
    branch_lists = re.findall(r"^\s+branches:\s*\[([^]\n]+)\]", workflow, re.MULTILINE)
    channels: set[str] = set()
    if any(
        "Working" in {value.strip().strip("\"'") for value in branches.split(",")}
        for branches in branch_lists
    ):
        channels.add("Working")
    release_path = repo_root / ".github/workflows/release.yml"
    release = release_path.read_text(encoding="utf-8") if release_path.is_file() else ""
    if "Working" in channels and "nightly_release_tag.py" in release and "'nightly-release'" in release:
        channels.add("nightly")
    return channels


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
    allowed = (set(channels) & {"Working", "nightly"}) | {"stable-v1"}
    rows = _table_rows(text)
    declared = {channel for channel, _ in rows}
    if not rows:
        errors.append(f"{label} must contain a Supported Versions table")
    if len(rows) != len(declared):
        errors.append(f"{label} repeats a policy channel")
    # Every published channel is disclosed, so a reader who finds a nightly
    # prerelease on the releases page also finds its unsupported status.
    if not allowed <= declared:
        missing = ", ".join(sorted(allowed - declared))
        errors.append(f"{label} must declare every published channel and the unpublished stable-v1 profile: {missing}")
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


def _nightly_tag_pattern() -> re.Pattern[str]:
    """The release workflow's own nightly tag rule, imported rather than duplicated."""
    helper = Path(__file__).resolve().parents[2] / ".github" / "scripts" / "nightly_release_tag.py"
    spec = importlib.util.spec_from_file_location("nightly_release_tag", helper)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot load {helper}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.TAG_RE


# The pre-run-identity nightly prerelease tag, still published on the releases page.
LEGACY_NIGHTLY_TAG = "nightly"


def published_releases(releases: Any) -> list[tuple[str, bool]]:
    """Return (tag, prerelease) for every non-draft release in GitHub REST release JSON.

    Accepts the ``GET /repos/{repo}/releases`` array, or the list of such pages
    that ``gh api --paginate --slurp`` writes. Anything else raises ValueError, so
    a failed or truncated fetch can never read as "nothing is published".
    """
    if not isinstance(releases, list):
        raise ValueError("published releases must be a JSON array")
    if releases and all(isinstance(page, list) for page in releases):
        releases = [release for page in releases for release in page]
    published: list[tuple[str, bool]] = []
    for index, release in enumerate(releases):
        if not isinstance(release, dict):
            raise ValueError(f"published release {index} is not an object")
        tag, draft, prerelease = release.get("tag_name"), release.get("draft"), release.get("prerelease")
        if not isinstance(tag, str) or not tag or type(draft) is not bool or type(prerelease) is not bool:
            raise ValueError(f"published release {index} lacks a tag_name, draft or prerelease field")
        if not draft:
            published.append((tag, prerelease))
    return published


def validate_against_releases(security: str, support: str, releases: Any) -> list[str]:
    """GOV-400: the policy tables name exactly the channels the releases page publishes.

    Drafts are ignored. The ``nightly`` row needs a published prerelease with a
    nightly tag; any other published prerelease, or any published stable
    release, is a publication the tables do not name, and a stable release also
    makes the "no supported version has been published" status false. Working is
    a configured development branch, not a release, and is checked offline.
    """
    published = published_releases(releases)
    nightly_tag = _nightly_tag_pattern()
    nightly = [tag for tag, prerelease in published if prerelease and (
        tag == LEGACY_NIGHTLY_TAG or nightly_tag.fullmatch(tag))]
    other_prereleases = [tag for tag, prerelease in published if prerelease and tag not in nightly]
    stable = [tag for tag, prerelease in published if not prerelease]
    errors: list[str] = []
    for label, text in (("SECURITY.md", security), ("SUPPORT.md", support)):
        declared = {channel for channel, _ in _table_rows(text)}
        if "nightly" in declared and not nightly:
            errors.append(f"{label} names an unpublished channel nightly: no nightly prerelease is published")
        if nightly and "nightly" not in declared:
            errors.append(f"{label} must declare the published nightly prerelease channel")
        for tag in other_prereleases:
            errors.append(f"{label} does not name published prerelease {tag}")
        for tag in stable:
            errors.append(f"{label} does not name published release {tag}")
        if stable and POLICY_STATUS["stable-v1"] in text:
            errors.append(f"{label} says no supported version has been published, but {', '.join(stable)} is")
    return errors


def validate(repo_root: Path, releases: Any = None) -> list[str]:
    """Offline policy checks, plus the published-release checks when ``releases`` is given."""
    channels = configured_development_channels(repo_root)
    security = (repo_root / "SECURITY.md").read_text(encoding="utf-8")
    support = (repo_root / "SUPPORT.md").read_text(encoding="utf-8")
    errors = validate_security_text(security, channels) + validate_support_text(support, channels)
    if releases is not None:
        errors += validate_against_releases(security, support, releases)
    return errors


def load_releases(path: Path) -> Any:
    """Read a bounded releases JSON file; an unreadable, empty or invalid file is an error."""
    from common import decode_json_bytes, read_bytes_stable

    limit = 8 * 1024 * 1024
    return decode_json_bytes(read_bytes_stable(path, limit, "published releases"), "published releases", limit)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--published-releases",
        type=Path,
        metavar="JSON",
        help="GitHub releases JSON (the REST array, or gh api --paginate --slurp pages) the tables must match",
    )
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[2]
    try:
        releases = load_releases(args.published_releases) if args.published_releases is not None else None
        errors = validate(root, releases)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"ERROR: cannot check the policy tables against published releases: {error}")
        return 1
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1
    scope = "published releases" if releases is not None else "published development channels"
    print(f"governance policy tables match the {scope} and pre-release boundary")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
