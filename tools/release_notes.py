#!/usr/bin/env python3
"""Render the fail-closed stable release body (REL-190).

The stable body is part of the immutable release, so it is generated from the
same sources the release is gated on rather than hand-written in the workflow:

* the release profile in ``docs/site/readiness.json`` (support matrix, excluded
  capabilities and gates, and the profile limitations, copied verbatim);
* the single ``## [X.Y.Z]`` section of ``CHANGELOG.md``, including any
  migration subsection;
* the frozen ``SHA256SUMS`` manifest and expected asset inventory;
* the detached signature control asset and its pinned signer fingerprint;
* fixed verification instructions for checksums, signatures, the SBOM, and
  build provenance.

Output is deterministic for identical inputs and bounded below GitHub's release
body limit, so resuming an interrupted stable draft re-renders the same text.
Every inconsistency is fatal and exits non-zero; there is no advisory mode.
Nightly releases keep their short workflow body and never use this tool.
"""
from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
from pathlib import Path
import re
import sys
import tarfile
from typing import Any

SHA256SUMS_NAME = "SHA256SUMS"
SBOM_NAME = "SparkEngine-SBOM.spdx.json"
PROVENANCE_MANIFEST_NAME = "SparkEngine-Exact-CI-Evidence.json"
SIGNATURE_CONTROL_NAME = "SparkEngine-release-signature-bundle.tar.gz"
SIGNATURE_MANIFEST_NAME = "release-signatures.json"
SIGNATURE_PUBLIC_KEY_NAME = "spark-release-public-key.pem"
RELEASE_WORKFLOW_PATH = ".github/workflows/release.yml"

# GitHub rejects release bodies above 125000 characters; stay clearly below it.
MAX_BODY_CHARACTERS = 120_000
MAX_INPUT_BYTES = 4 * 1024 * 1024
MAX_ASSETS = 100
MAX_SIGNATURE_MEMBERS = MAX_ASSETS + 2
MAX_PUBLIC_KEY_BYTES = 16 * 1024

_SEMVER = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
_SHA1 = re.compile(r"[0-9a-f]{40}")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
_REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
_SERVER_URL = re.compile(r"https://[A-Za-z0-9.-]+(?::[0-9]+)?")
_TIMESTAMP = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z")
_TITLE_SUFFIX = re.compile(r"(?: \([A-Za-z0-9 ._-]{1,40}\))?")
_SUMS_LINE = re.compile(r"(?P<digest>[0-9a-f]{64}) [ *](?P<name>.+)")
_CHANGELOG_VERSION = re.compile(r"^## \[(?P<version>[^\]]+)\](?P<rest>.*)$")
_LEVEL2_HEADING = re.compile(r"^## ")
_MIGRATION_HEADING = re.compile(r"^### +(?:migrations?|migration notes|upgrading|upgrade notes)\s*$", re.IGNORECASE)
_LEVEL3_HEADING = re.compile(r"^### ")
_FENCE = re.compile(r"^\s*(?:```|~~~)")
_PEM_BLOCK = re.compile(r"-----BEGIN PUBLIC KEY-----\n(?P<body>[A-Za-z0-9+/=\n]+)-----END PUBLIC KEY-----\n?")


class ReleaseNotesError(Exception):
    """The release notes cannot be rendered without misrepresenting the release."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ReleaseNotesError(message)


def _read_text(path: Path, label: str) -> str:
    _require(path.is_file() and not path.is_symlink(), f"{label} is missing or not a regular file: {path}")
    _require(path.stat().st_size <= MAX_INPUT_BYTES, f"{label} exceeds {MAX_INPUT_BYTES} bytes")
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        raise ReleaseNotesError(f"{label} is not UTF-8: {exc}") from exc
    return text.replace("\r\n", "\n")


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        _require(key not in result, f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _nonempty_string(value: Any, label: str) -> str:
    _require(isinstance(value, str) and value.strip() != "", f"{label} must be a non-empty string")
    _require("\n" not in value, f"{label} must be a single line")
    return value.strip()


def load_profile(readiness_path: Path, profile_id: str) -> dict[str, Any]:
    """Return the named profile plus capability and gate names, validated for rendering."""
    try:
        readiness = json.loads(_read_text(readiness_path, "readiness contract"), object_pairs_hook=_reject_duplicate_keys)
    except json.JSONDecodeError as exc:
        raise ReleaseNotesError(f"readiness contract is not valid JSON: {exc}") from exc
    _require(isinstance(readiness, dict), "readiness contract must be a JSON object")
    profiles = [entry for entry in readiness.get("releaseProfiles", []) if isinstance(entry, dict)
                and entry.get("id") == profile_id]
    _require(len(profiles) == 1, f"readiness contract must define release profile {profile_id!r} exactly once")
    profile = profiles[0]

    capabilities = {entry.get("id"): entry.get("name") for entry in readiness.get("capabilities", [])
                    if isinstance(entry, dict)}
    gates = {entry.get("id"): entry.get("name") for entry in readiness.get("gates", []) if isinstance(entry, dict)}

    scope = profile.get("scope")
    _require(isinstance(scope, list) and bool(scope), f"profile {profile_id} has no support scope")
    rows = [(_nonempty_string(entry.get("label"), "profile scope label"),
             _nonempty_string(entry.get("value"), "profile scope value"))
            for entry in scope if isinstance(entry, dict)]
    _require(len(rows) == len(scope), f"profile {profile_id} scope entries must be objects")

    hosts = profile.get("supportedHosts")
    _require(isinstance(hosts, list) and bool(hosts), f"profile {profile_id} declares no supported host")
    limitations = profile.get("limitations")
    _require(isinstance(limitations, list) and bool(limitations), f"profile {profile_id} declares no limitations")

    boundaries = profile.get("boundaries")
    _require(isinstance(boundaries, dict), f"profile {profile_id} has no capability boundaries")

    def capability_rows(key: str) -> list[tuple[str, str]]:
        identifiers = boundaries.get(key)
        _require(isinstance(identifiers, list), f"profile {profile_id} boundaries.{key} must be an array")
        rows_out = []
        for identifier in identifiers:
            _require(identifier in capabilities, f"profile {profile_id} names unknown capability {identifier!r}")
            rows_out.append((identifier, _nonempty_string(capabilities[identifier], f"capability {identifier} name")))
        return rows_out

    excluded_gates = []
    for entry in profile.get("excludedGates", []):
        _require(isinstance(entry, dict), f"profile {profile_id} excludedGates entries must be objects")
        gate_id = entry.get("gateId")
        _require(gate_id in gates, f"profile {profile_id} excludes unknown gate {gate_id!r}")
        excluded_gates.append((gate_id, _nonempty_string(gates[gate_id], f"gate {gate_id} name"),
                               _nonempty_string(entry.get("reason"), f"gate {gate_id} exclusion reason")))

    return {
        "id": profile_id,
        "name": _nonempty_string(profile.get("name"), "profile name"),
        "state": _nonempty_string(profile.get("state"), "profile state"),
        "scope": rows,
        "hosts": [_nonempty_string(host, "supported host") for host in hosts],
        "limitations": [_nonempty_string(item, "profile limitation") for item in limitations],
        "experimental": capability_rows("experimentalCapabilityIds"),
        "unsupported": capability_rows("unsupportedCapabilityIds"),
        "excludedGates": excluded_gates,
    }


def load_changelog_section(changelog_path: Path, version: str) -> tuple[str, str, str | None]:
    """Return (heading suffix, section body, migration subsection or None) for exactly one version."""
    lines = _read_text(changelog_path, "changelog").split("\n")
    starts = []
    in_fence = False
    for index, line in enumerate(lines):
        if _FENCE.match(line):
            in_fence = not in_fence
            continue
        match = None if in_fence else _CHANGELOG_VERSION.match(line)
        if match and match.group("version") == version:
            starts.append((index, match.group("rest").strip()))
    _require(starts, f"CHANGELOG.md has no ## [{version}] section")
    _require(len(starts) == 1, f"CHANGELOG.md has {len(starts)} ## [{version}] sections; exactly one is required")
    start, rest = starts[0]

    end = len(lines)
    in_fence = False
    for index in range(start + 1, len(lines)):
        if _FENCE.match(lines[index]):
            in_fence = not in_fence
        elif not in_fence and _LEVEL2_HEADING.match(lines[index]):
            end = index
            break
    body_lines = lines[start + 1:end]
    while body_lines and not body_lines[0].strip():
        body_lines.pop(0)
    while body_lines and not body_lines[-1].strip():
        body_lines.pop()
    _require(any(line.strip() for line in body_lines), f"CHANGELOG.md section [{version}] is empty")

    # The migration subsection runs from its ### heading to the next ### heading.
    migration: list[str] | None = None
    capturing = False
    in_fence = False
    for line in body_lines:
        if _FENCE.match(line):
            in_fence = not in_fence
        elif not in_fence and _LEVEL3_HEADING.match(line):
            is_migration = _MIGRATION_HEADING.match(line) is not None
            _require(not (is_migration and migration is not None),
                     f"CHANGELOG.md section [{version}] has more than one migration subsection")
            capturing = is_migration
            if is_migration:
                migration = []
            continue
        if capturing and migration is not None:
            migration.append(line)
    migration_text = None
    if migration is not None:
        migration_text = "\n".join(migration).strip()
        _require(migration_text != "", f"CHANGELOG.md section [{version}] has an empty migration subsection")

    # Demote the section's own headings so they nest under the notes' "Changes" heading.
    demoted = []
    in_fence = False
    for line in body_lines:
        if _FENCE.match(line):
            in_fence = not in_fence
        demoted.append("#" + line if not in_fence and line.startswith("#") else line)
    return rest, "\n".join(demoted), migration_text


def load_expected_assets(path: Path) -> list[str]:
    names = [line.strip() for line in _read_text(path, "expected asset inventory").split("\n") if line.strip()]
    _require(0 < len(names) <= MAX_ASSETS, "expected asset inventory is empty or unbounded")
    for name in names:
        _require(_SAFE_NAME.fullmatch(name) is not None, f"unsafe expected asset name {name!r}")
    _require(len({name.casefold() for name in names}) == len(names), "expected asset inventory repeats a name")
    _require(SHA256SUMS_NAME in names, f"expected asset inventory does not contain {SHA256SUMS_NAME}")
    _require(SBOM_NAME in names, f"expected asset inventory does not contain the SBOM {SBOM_NAME}")
    _require(SIGNATURE_CONTROL_NAME not in names,
             f"{SIGNATURE_CONTROL_NAME} is a control asset and must not be a distributable")
    return names


def load_sha256sums(path: Path, expected: list[str]) -> list[tuple[str, str]]:
    entries = []
    for line in _read_text(path, SHA256SUMS_NAME).split("\n"):
        if not line.strip():
            continue
        match = _SUMS_LINE.fullmatch(line)
        _require(match is not None, f"malformed {SHA256SUMS_NAME} line: {line!r}")
        entries.append((match.group("digest"), match.group("name")))
    _require(bool(entries), f"{SHA256SUMS_NAME} is empty")
    names = [name for _digest, name in entries]
    _require(len({name.casefold() for name in names}) == len(names), f"{SHA256SUMS_NAME} repeats an asset")
    expected_set = set(expected)
    for name in names:
        _require(name in expected_set, f"{SHA256SUMS_NAME} lists {name}, which is not an expected release asset")
    # Every distributable is checksummed; only the manifest itself and the
    # separately digest-bound exact-CI evidence manifest are exempt.
    unsummed = sorted(expected_set - set(names) - {SHA256SUMS_NAME, PROVENANCE_MANIFEST_NAME})
    _require(not unsummed, f"{SHA256SUMS_NAME} does not cover expected assets: {', '.join(unsummed)}")
    _require(SBOM_NAME in names, f"{SHA256SUMS_NAME} does not cover the SBOM {SBOM_NAME}")
    return entries


def verify_signature_control(path: Path, fingerprint: str) -> None:
    """Require the signature control asset to carry the key the notes tell users to pin."""
    _require(path.is_file() and not path.is_symlink() and path.stat().st_size > 0,
             f"signature control asset is missing or empty: {path}")
    try:
        with tarfile.open(path, mode="r:gz") as archive:
            members = archive.getmembers()
            _require(len(members) <= MAX_SIGNATURE_MEMBERS, "signature control asset has too many members")
            by_name = {}
            for member in members:
                _require(member.isfile() and _SAFE_NAME.fullmatch(member.name) is not None,
                         f"signature control asset member {member.name!r} is not one flat regular file")
                _require(member.name not in by_name, f"signature control asset repeats {member.name}")
                by_name[member.name] = member
            for required in (SIGNATURE_MANIFEST_NAME, SIGNATURE_PUBLIC_KEY_NAME):
                _require(required in by_name, f"signature control asset does not contain {required}")
            _require(any(name.endswith(".sig") for name in by_name), "signature control asset has no detached signature")
            key_member = by_name[SIGNATURE_PUBLIC_KEY_NAME]
            _require(0 < key_member.size <= MAX_PUBLIC_KEY_BYTES, "signature public key size is out of bounds")
            handle = archive.extractfile(key_member)
            _require(handle is not None, "signature public key cannot be read")
            pem = handle.read().decode("ascii")
    except (tarfile.TarError, OSError, EOFError, UnicodeDecodeError) as exc:
        raise ReleaseNotesError(f"signature control asset is not a readable gzip tar archive: {exc}") from exc
    match = _PEM_BLOCK.fullmatch(pem.replace("\r\n", "\n"))
    _require(match is not None, "signature public key is not one PEM SubjectPublicKeyInfo block")
    try:
        der = base64.b64decode(match.group("body").replace("\n", ""), validate=True)
    except binascii.Error as exc:
        raise ReleaseNotesError(f"signature public key PEM is not valid base64: {exc}") from exc
    _require(hashlib.sha256(der).hexdigest() == fingerprint,
             "signature public key does not match the pinned signer fingerprint")


def _cell(value: str) -> str:
    return value.replace("|", "\\|")


def render(*, version: str, title_suffix: str, source_commit: str, built_at: str, server_url: str,
           repository: str, profile: dict[str, Any], changelog_rest: str, changelog_body: str,
           migration: str | None, expected: list[str], sums: list[tuple[str, str]], fingerprint: str) -> str:
    tag = f"v{version}"
    commit_url = f"{server_url}/{repository}/commit/{source_commit}"
    signer_workflow = f"{repository}/{RELEASE_WORKFLOW_PATH}"
    out: list[str] = []
    add = out.append

    add(f"## SparkEngine {version} — Stable Release{title_suffix}")
    add("")
    add(f"Built from commit [`{source_commit[:7]}`]({commit_url})")
    add(f"Built on: {built_at}")
    add("")
    add("> **Stable-v1 releases** contain Windows x64 Shipping packages built with the authoritative "
        "`windows-shipping` preset and validated separately in Release. Stable releases and uniquely tagged "
        "nightly prereleases are immutable; the historical `nightly` release is never modified.")
    add("")

    add(f"### Support matrix: {profile['name']} (`{profile['id']}`)")
    add("")
    add(f"Release profile state recorded in `docs/site/readiness.json` at this commit: `{profile['state']}`.")
    add(f"Supported hosts: {', '.join(profile['hosts'])}.")
    add("")
    add("| Scope | Supported configuration |")
    add("|---|---|")
    for label, value in profile["scope"]:
        add(f"| {_cell(label)} | {_cell(value)} |")
    add("")

    add("### Outside this release")
    add("")
    add("These capabilities are not part of this release's supported surface and must not be presented as "
        "supported.")
    add("")
    add("Experimental:")
    add("")
    for identifier, name in profile["experimental"]:
        add(f"- {name} (`{identifier}`)")
    if not profile["experimental"]:
        add("- None declared.")
    add("")
    add("Unsupported:")
    add("")
    for identifier, name in profile["unsupported"]:
        add(f"- {name} (`{identifier}`)")
    if not profile["unsupported"]:
        add("- None declared.")
    add("")
    add("Release gates excluded from this profile (they may remain blocked):")
    add("")
    for gate_id, name, reason in profile["excludedGates"]:
        add(f"- {gate_id} {name}: {reason}")
    if not profile["excludedGates"]:
        add("- None declared.")
    add("")

    add("### Known limitations")
    add("")
    for limitation in profile["limitations"]:
        add(f"- {limitation}")
    add("")

    heading_suffix = f" {changelog_rest}" if changelog_rest else ""
    add(f"### Changes in {version}{heading_suffix}")
    add("")
    add(changelog_body)
    add("")

    add("### Migrations")
    add("")
    if migration is None:
        add(f"CHANGELOG.md records no migration subsection for {version}. Review the changes above before "
            "upgrading.")
    else:
        add(migration)
    add("")

    add("### Release assets")
    add("")
    for name in expected:
        add(f"- `{name}`")
    add(f"- `{SIGNATURE_CONTROL_NAME}` (detached signature control asset)")
    add("")

    add(f"### SHA-256 checksums (`{SHA256SUMS_NAME}`)")
    add("")
    add("```text")
    for digest, name in sums:
        add(f"{digest}  {name}")
    add("```")
    add("")

    add("### Verify this release")
    add("")
    add("1. Checksums. Download the assets you need next to `SHA256SUMS`, then run the command below. "
        "`SHA256SUMS` itself is covered by the build provenance attestation in step 4.")
    add("")
    add("   ```sh")
    add("   sha256sum --ignore-missing -c SHA256SUMS")
    add("   ```")
    add("")
    add(f"2. Signatures. Extract `{SIGNATURE_CONTROL_NAME}`, confirm the public key is the pinned signer key, "
        "and verify each asset's detached signature (each check must print `Verified OK`):")
    add("")
    add("   ```sh")
    add(f"   tar -xzf {SIGNATURE_CONTROL_NAME}")
    add(f"   openssl pkey -pubin -in {SIGNATURE_PUBLIC_KEY_NAME} -outform DER | sha256sum")
    add(f"   # expected: {fingerprint}")
    add(f"   openssl dgst -sha256 -verify {SIGNATURE_PUBLIC_KEY_NAME} -signature <asset>.sig <asset>")
    add("   ```")
    add("")
    add(f"3. SBOM. `{SBOM_NAME}` is the SPDX software bill of materials for these assets. It is covered by "
        "`SHA256SUMS`, carries its own detached signature, and has a build provenance attestation:")
    add("")
    add("   ```sh")
    add(f"   gh attestation verify {SBOM_NAME} --repo {repository} --signer-workflow {signer_workflow}")
    add("   ```")
    add("")
    add("4. Provenance. Every asset has a GitHub build provenance attestation from this repository's release "
        f"workflow, and the release itself carries an immutable-release attestation. `{PROVENANCE_MANIFEST_NAME}` "
        "records the exact-commit CI evidence the release was gated on.")
    add("")
    add("   ```sh")
    add(f"   gh attestation verify <asset> --repo {repository} --signer-workflow {signer_workflow}")
    add(f"   gh release verify {tag} --repo {repository}")
    add("   ```")

    body = "\n".join(out) + "\n"
    _require(len(body) <= MAX_BODY_CHARACTERS,
             f"rendered release notes are {len(body)} characters; the bound is {MAX_BODY_CHARACTERS}")
    return body


def build_notes(args: argparse.Namespace) -> str:
    _require(_SEMVER.fullmatch(args.version) is not None, "--version must be X.Y.Z")
    _require(_SHA1.fullmatch(args.source_commit) is not None, "--source-commit must be a 40-hex commit SHA")
    _require(_TIMESTAMP.fullmatch(args.built_at) is not None, "--built-at must be a UTC YYYY-MM-DDTHH:MM:SSZ timestamp")
    _require(_SERVER_URL.fullmatch(args.server_url) is not None, "--server-url must be an https origin")
    _require(_REPOSITORY.fullmatch(args.repository) is not None, "--repository must be OWNER/REPO")
    _require(_TITLE_SUFFIX.fullmatch(args.title_suffix) is not None, "--title-suffix must be empty or ' (Label)'")
    _require(_SHA256.fullmatch(args.signer_fingerprint) is not None,
             "--signer-fingerprint must be a lowercase SHA-256 digest")

    profile = load_profile(args.readiness, args.profile)
    changelog_rest, changelog_body, migration = load_changelog_section(args.changelog, args.version)
    expected = load_expected_assets(args.expected_assets_file)
    sums = load_sha256sums(args.sha256sums, expected)
    verify_signature_control(args.signature_control_asset, args.signer_fingerprint)
    return render(version=args.version, title_suffix=args.title_suffix, source_commit=args.source_commit,
                  built_at=args.built_at, server_url=args.server_url, repository=args.repository,
                  profile=profile, changelog_rest=changelog_rest, changelog_body=changelog_body,
                  migration=migration, expected=expected, sums=sums, fingerprint=args.signer_fingerprint)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True, help="stable version X.Y.Z (the CHANGELOG.md section)")
    parser.add_argument("--profile", default="stable-v1", help="release profile id in the readiness contract")
    parser.add_argument("--readiness", type=Path, default=Path("docs/site/readiness.json"))
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    parser.add_argument("--sha256sums", type=Path, required=True)
    parser.add_argument("--expected-assets-file", type=Path, required=True)
    parser.add_argument("--signature-control-asset", type=Path, required=True,
                        help=f"local copy of {SIGNATURE_CONTROL_NAME}")
    parser.add_argument("--signer-fingerprint", required=True, help="pinned SPKI SHA-256 of the signing key")
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--built-at", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--server-url", default="https://github.com")
    parser.add_argument("--title-suffix", default="")
    parser.add_argument("--output", type=Path, help="write the body here instead of stdout")
    args = parser.parse_args(argv)
    try:
        body = build_notes(args)
    except ReleaseNotesError as exc:
        print(f"release notes: {exc}", file=sys.stderr)
        return 1
    if args.output:
        args.output.write_text(body, encoding="utf-8")
    else:
        sys.stdout.write(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
