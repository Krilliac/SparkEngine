#!/usr/bin/env python3
"""Generate the repository's THIRD_PARTY_NOTICES file from on-disk license text.

Inputs (read-only):
  * ``ThirdParty/dependencies.lock``  - per-dependency name, source, version,
    declared license, local path, and the license notice files to ship.
  * ``ThirdParty/supply-chain.lock``  - the authoritative container inventory
    (managed vendored directories, submodule gitlinks, project-owned dirs).
  * the Git index (``git ls-files``)  - the tracked file list, so the output is
    identical whether or not submodules happen to be checked out.

The generator only reproduces license text that exists in the repository. It
never supplies a license from memory: a component whose notice file is absent,
whose declared license is not a single SPDX identifier, whose tracked payload is
a repository-authored stub, or whose tree carries undeclared license files is
listed under "ATTENTION REQUIRED" instead of being guessed at. Tracked font
files outside ``ThirdParty/`` are listed there too, because fonts carry their
own licenses and nothing else in the repository inventories them.

Usage:
  python tools/governance/generate_third_party_notices.py            # write
  python tools/governance/generate_third_party_notices.py --check    # exit 1 if stale
  python tools/governance/generate_third_party_notices.py --require-complete

Exit codes: 0 ok, 1 stale output (--check) or incomplete notices
(--require-complete), 2 malformed or unreadable inputs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Callable, Iterable

REPO_ROOT = Path(__file__).resolve().parents[2]
OUTPUT_NAME = "THIRD_PARTY_NOTICES"
MANIFEST_PATH = "ThirdParty/dependencies.lock"
SUPPLY_CHAIN_PATH = "ThirdParty/supply-chain.lock"
MANIFEST_VARIABLE = "SPARK_THIRDPARTY_AUDIT_ENTRIES"
MANIFEST_FIELD_COUNT = 10
MAX_NOTICE_BYTES = 1 << 20
RULE = "=" * 79
SUBRULE = "-" * 79

LICENSE_BASENAME = re.compile(
    r"^(?:licen[cs]e|copying|notice|unlicense|patents)(?:[._-][^/]*)?$", re.IGNORECASE
)
FONT_SUFFIXES = frozenset({".ttf", ".otf", ".woff", ".woff2", ".ttc"})
HEADER_SUFFIXES = frozenset({".h", ".hh", ".hpp"})
STUB_MARKER = re.compile(r"\bstub\b", re.IGNORECASE)
STUB_SCAN_LINES = 40
SINGLE_SPDX_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.+-]*$")
GITLINK_CALL = re.compile(
    r'_spark_manifest_gitlink_revision\(\s*"([^"]+)"\s+([A-Za-z_][A-Za-z0-9_]*)\s*\)'
)
VARIABLE_REF = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


class NoticeInputError(Exception):
    """Raised when an input is malformed; the generator fails closed (exit 2)."""


@dataclass(frozen=True)
class ManifestEntry:
    name: str
    source: str
    version: str
    license: str
    local_path: str
    required_files: tuple[str, ...]
    notice_files: tuple[str, ...]


@dataclass
class Component:
    path: str
    kind: str  # "vendored" or "submodule"
    entry: ManifestEntry | None
    notices: list[tuple[str, str]] = field(default_factory=list)  # (path, text)
    extra_notices: list[tuple[str, str]] = field(default_factory=list)
    findings: list[str] = field(default_factory=list)

    @property
    def display_name(self) -> str:
        return self.entry.name if self.entry else self.path

    @property
    def has_notice(self) -> bool:
        return bool(self.notices)


# --------------------------------------------------------------------------- parsing


def _strip_cmake_comments(text: str) -> str:
    lines = []
    for line in text.splitlines():
        # Manifest records never contain '#', so a leading-comment strip is exact.
        stripped = line.lstrip()
        lines.append("" if stripped.startswith("#") else line)
    return "\n".join(lines)


def parse_manifest(text: str, gitlinks: dict[str, str]) -> list[ManifestEntry]:
    """Parse the CMake-syntax dependency manifest without executing CMake.

    Only the single ``set(SPARK_THIRDPARTY_AUDIT_ENTRIES ...)`` form is accepted.
    Any other mutation of the variable (``list(APPEND ...)``, a second ``set``)
    is rejected rather than silently missed: tools/check-supply-chain.py expands
    the manifest through CMake and is the authority for those forms.
    """
    body = _strip_cmake_comments(text)
    mentions = len(re.findall(re.escape(MANIFEST_VARIABLE), body))
    match = re.search(r"set\(\s*" + re.escape(MANIFEST_VARIABLE) + r"\b(.*?)\n\)", body, re.DOTALL)
    if not match:
        raise NoticeInputError(f"{MANIFEST_PATH}: no set({MANIFEST_VARIABLE} ...) block")
    if mentions != 1:
        raise NoticeInputError(
            f"{MANIFEST_PATH}: {MANIFEST_VARIABLE} is referenced {mentions} times; only a single "
            "set(...) block is supported (use tools/check-supply-chain.py for expanded forms)"
        )

    variables: dict[str, str] = {}
    for path, variable in GITLINK_CALL.findall(body):
        if path not in gitlinks:
            raise NoticeInputError(f"{MANIFEST_PATH}: gitlink variable {variable} names unlocked path {path}")
        variables[variable] = gitlinks[path]

    records = re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1))
    if not records:
        raise NoticeInputError(f"{MANIFEST_PATH}: {MANIFEST_VARIABLE} has no entries")

    entries: list[ManifestEntry] = []
    seen_paths: set[str] = set()
    for record in records:

        def substitute(ref: re.Match[str]) -> str:
            name = ref.group(1)
            if name not in variables:
                raise NoticeInputError(f"{MANIFEST_PATH}: unresolved variable ${{{name}}}")
            return variables[name]

        expanded = VARIABLE_REF.sub(substitute, record)
        fields = expanded.split("|")
        if len(fields) != MANIFEST_FIELD_COUNT:
            raise NoticeInputError(
                f"{MANIFEST_PATH}: entry has {len(fields)} fields, expected {MANIFEST_FIELD_COUNT}: {record[:80]}"
            )
        name, source, version, license_, local_path, required, _macro, _fallback, _sev, notices = fields
        if local_path in seen_paths:
            raise NoticeInputError(f"{MANIFEST_PATH}: duplicate local_path {local_path}")
        seen_paths.add(local_path)
        entries.append(
            ManifestEntry(
                name=name.strip(),
                source=source.strip(),
                version=version.strip(),
                license=license_.strip(),
                local_path=local_path.strip(),
                required_files=tuple(p.strip() for p in required.split(",") if p.strip()),
                notice_files=tuple(p.strip() for p in notices.split(",") if p.strip()),
            )
        )
    return entries


def parse_supply_chain(text: str) -> dict:
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise NoticeInputError(f"{SUPPLY_CHAIN_PATH}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise NoticeInputError(f"{SUPPLY_CHAIN_PATH}: top level must be an object")
    vendored = data.get("managed_vendored_dirs")
    gitlinks = data.get("submodule_gitlinks")
    owned = data.get("project_owned_dirs")
    if not isinstance(vendored, list) or not all(isinstance(p, str) for p in vendored):
        raise NoticeInputError(f"{SUPPLY_CHAIN_PATH}: managed_vendored_dirs must be a list of strings")
    if not isinstance(gitlinks, dict) or not all(isinstance(v, str) for v in gitlinks.values()):
        raise NoticeInputError(f"{SUPPLY_CHAIN_PATH}: submodule_gitlinks must map paths to SHAs")
    if not isinstance(owned, dict) or not all(isinstance(v, str) for v in owned.values()):
        raise NoticeInputError(f"{SUPPLY_CHAIN_PATH}: project_owned_dirs must map paths to justifications")
    return data


# --------------------------------------------------------------------------- repository access


def git_tracked_files(root: Path) -> list[str]:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            check=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise NoticeInputError(f"git ls-files failed in {root}: {exc}") from exc
    return sorted(p for p in result.stdout.decode("utf-8").split("\0") if p)


def normalize_text(raw: bytes, label: str) -> str:
    if len(raw) > MAX_NOTICE_BYTES:
        raise NoticeInputError(f"{label}: exceeds {MAX_NOTICE_BYTES} bytes")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise NoticeInputError(f"{label}: not UTF-8: {exc}") from exc
    text = text.lstrip("﻿").replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.rstrip() for line in text.split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    while lines and not lines[0]:
        lines.pop(0)
    return "\n".join(lines) + "\n"


def _under(path: str, directory: str) -> bool:
    return path == directory or path.startswith(directory.rstrip("/") + "/")


# --------------------------------------------------------------------------- model


def build_components(
    root: Path,
    manifest: list[ManifestEntry],
    supply_chain: dict,
    tracked: list[str],
    read_bytes: Callable[[str], bytes | None] | None = None,
) -> list[Component]:
    """Assemble one Component per locked container, with findings."""

    def default_read(rel: str) -> bytes | None:
        path = root / rel
        return path.read_bytes() if path.is_file() else None

    read = read_bytes or default_read
    by_path = {entry.local_path: entry for entry in manifest}
    containers = [(p, "vendored") for p in supply_chain["managed_vendored_dirs"]]
    containers += [(p, "submodule") for p in supply_chain["submodule_gitlinks"]]

    components: list[Component] = []
    for path, kind in sorted(containers):
        component = Component(path=path, kind=kind, entry=by_path.get(path))
        entry = component.entry
        if entry is None:
            component.findings.append(f"no {MANIFEST_PATH} entry; license and notice file are unknown")
            components.append(component)
            continue

        if not entry.notice_files:
            component.findings.append("dependencies.lock declares no license notice file")
        for notice in entry.notice_files:
            raw = read(notice)
            if raw is None:
                component.findings.append(f"declared notice file is missing on disk: {notice}")
                continue
            component.notices.append((notice, normalize_text(raw, notice)))

        if not SINGLE_SPDX_ID.match(entry.license):
            component.findings.append(
                f"declared license {entry.license!r} is not a single SPDX identifier; "
                "the applicable choice or combination needs owner classification"
            )

        declared = set(entry.notice_files)
        in_tree = [p for p in tracked if _under(p, path)]
        for rel in in_tree:
            if rel in declared or not LICENSE_BASENAME.match(PurePosixPath(rel).name):
                continue
            raw = read(rel)
            if raw is None:
                continue
            component.extra_notices.append((rel, normalize_text(raw, rel)))
            component.findings.append(
                f"undeclared license file inside the component applies to part of it: {rel}"
            )

        for rel in in_tree:
            if PurePosixPath(rel).suffix.lower() in FONT_SUFFIXES and not _has_sibling_license(rel, tracked):
                component.findings.append(f"font file has no license file beside it: {rel}")

        for required in entry.required_files:
            rel = f"{path}/{required}"
            if PurePosixPath(rel).suffix.lower() not in HEADER_SUFFIXES:
                continue
            raw = read(rel)
            if raw is None:
                continue
            head = raw.decode("utf-8", errors="replace").splitlines()[:STUB_SCAN_LINES]
            if any(STUB_MARKER.search(line) for line in head):
                component.findings.append(
                    f"{rel} identifies itself as a repository-authored stub, not the upstream "
                    "library; whether the upstream notice applies to it needs owner review"
                )
        components.append(component)
    return components


def _has_sibling_license(rel: str, tracked: Iterable[str]) -> bool:
    parent = str(PurePosixPath(rel).parent)
    for other in tracked:
        other_path = PurePosixPath(other)
        if str(other_path.parent) == parent and LICENSE_BASENAME.match(other_path.name):
            return True
    return False


def uncovered_fonts(supply_chain: dict, tracked: list[str]) -> list[str]:
    """Tracked font files outside every locked ThirdParty container."""
    fonts = []
    for rel in tracked:
        if PurePosixPath(rel).suffix.lower() not in FONT_SUFFIXES:
            continue
        if _under(rel, "ThirdParty"):
            continue
        if not _has_sibling_license(rel, tracked):
            fonts.append(rel)
    return fonts


# --------------------------------------------------------------------------- rendering


def _sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def render(components: list[Component], supply_chain: dict, fonts: list[str]) -> str:
    out: list[str] = []
    add = out.append
    with_notice = sum(1 for c in components if c.has_notice)
    flagged = [c for c in components if c.findings]

    add("THIRD-PARTY SOFTWARE NOTICES")
    add("")
    add("This file is generated by tools/governance/generate_third_party_notices.py from")
    add(f"{MANIFEST_PATH}, {SUPPLY_CHAIN_PATH}, and the license files tracked in this")
    add("repository. Do not edit it by hand; regenerate it and commit the result.")
    add("")
    add("It reproduces only license text that exists on disk. It is an inventory, not a")
    add("legal opinion. SparkEngine's own terms are in LICENSE. Items listed under")
    add("ATTENTION REQUIRED are unresolved and are tracked by GOV-400; see")
    add("docs/governance/GOV-400-DECISIONS.md.")
    add("")
    add("Installed packages carry a separate THIRD_PARTY_NOTICES.txt that CMake builds at")
    add("configure time (cmake/SparkThirdPartyAudit.cmake) from the declared notice files")
    add("only; the attention items below are not reflected in that packaged file.")
    add("")
    add(RULE)
    add("SUMMARY")
    add(RULE)
    add(f"Locked third-party components: {len(components)}")
    add(f"Components with at least one notice file on disk: {with_notice}")
    add(f"Components with attention items: {len(flagged)}")
    add(f"Tracked font files outside ThirdParty/ without a license file: {len(fonts)}")
    add("")
    for index, component in enumerate(components, start=1):
        license_ = component.entry.license if component.entry else "UNKNOWN"
        marker = " [attention]" if component.findings else ""
        add(f"[{index:02d}] {component.display_name} - {license_} - {component.path} ({component.kind}){marker}")
    add("")

    add(RULE)
    add("ATTENTION REQUIRED")
    add(RULE)
    if not flagged and not fonts:
        add("None.")
    for component in flagged:
        add(f"* {component.display_name} ({component.path})")
        for finding in component.findings:
            add(f"    - {finding}")
    if fonts:
        add("* Font files outside ThirdParty/ (not covered by dependencies.lock)")
        for rel in fonts:
            add(f"    - no license file on disk for {rel}")
    add("")

    add(RULE)
    add("PROJECT-OWNED DIRECTORIES UNDER ThirdParty/ (first-party; no third-party notice)")
    add(RULE)
    for path, why in sorted(supply_chain["project_owned_dirs"].items()):
        add(f"* {path}: {why}")
    add("")

    for index, component in enumerate(components, start=1):
        entry = component.entry
        add(RULE)
        add(f"[{index:02d}] {component.display_name}")
        add(RULE)
        add(f"Path:             {component.path} ({component.kind})")
        if entry:
            add(f"Source:           {entry.source}")
            add(f"Version:          {entry.version}")
            add(f"Declared license: {entry.license}")
        else:
            add("Source:           UNKNOWN (no dependencies.lock entry)")
        if not component.notices:
            add("")
            add("NO LICENSE TEXT AVAILABLE ON DISK FOR THIS COMPONENT.")
        for rel, text in component.notices:
            add("")
            add(SUBRULE)
            add(f"License file: {rel} (sha256 of normalized text {_sha256(text)})")
            add(SUBRULE)
            out.append(text.rstrip("\n"))
        for rel, text in component.extra_notices:
            add("")
            add(SUBRULE)
            add(f"Additional license file inside this component (undeclared): {rel}")
            add(f"(sha256 of normalized text {_sha256(text)})")
            add(SUBRULE)
            out.append(text.rstrip("\n"))
        add("")
    add(RULE)
    add("END OF THIRD-PARTY SOFTWARE NOTICES")
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------------- entry points


def load(root: Path) -> tuple[list[Component], dict, list[str]]:
    try:
        manifest_text = (root / MANIFEST_PATH).read_text(encoding="utf-8")
        supply_text = (root / SUPPLY_CHAIN_PATH).read_text(encoding="utf-8")
    except OSError as exc:
        raise NoticeInputError(f"cannot read lockfile: {exc}") from exc
    supply_chain = parse_supply_chain(supply_text)
    manifest = parse_manifest(manifest_text, supply_chain["submodule_gitlinks"])
    tracked = git_tracked_files(root)
    components = build_components(root, manifest, supply_chain, tracked)
    return components, supply_chain, uncovered_fonts(supply_chain, tracked)


def generate(root: Path = REPO_ROOT) -> tuple[str, list[Component]]:
    components, supply_chain, fonts = load(root)
    return render(components, supply_chain, fonts), components


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root")
    parser.add_argument("--output", type=Path, help=f"output path (default: <root>/{OUTPUT_NAME})")
    parser.add_argument("--check", action="store_true", help="exit 1 if the output file is stale")
    parser.add_argument(
        "--require-complete",
        action="store_true",
        help="exit 1 if any locked component has no notice file on disk",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()
    output = args.output or root / OUTPUT_NAME

    try:
        text, components = generate(root)
    except NoticeInputError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    status = 0
    if args.check:
        current = output.read_bytes().decode("utf-8").replace("\r\n", "\n") if output.is_file() else None
        if current != text:
            print(f"{output}: stale or missing; run {Path(__file__).name} to regenerate", file=sys.stderr)
            status = 1
        else:
            print(f"{output}: up to date ({len(components)} components)")
    else:
        output.write_bytes(text.encode("utf-8"))
        print(f"wrote {output} ({len(components)} components)")

    if args.require_complete:
        missing = [c.path for c in components if not c.has_notice]
        if missing:
            print("components without a notice file on disk: " + ", ".join(missing), file=sys.stderr)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
