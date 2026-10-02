#!/usr/bin/env python3
"""Enforce the stable release tag / source version / changelog contract.

A stable tag ``vX.Y.Z`` may only be published when:

* ``CMakeLists.txt`` declares ``SPARK_ENGINE_VERSION`` exactly once, as the
  ``set(SPARK_ENGINE_VERSION "X.Y.Z" CACHE ...)`` default, so there is one
  authoritative version source;
* the tag version equals that default, so an override can never manufacture a
  stable version absent from the source; and
* ``CHANGELOG.md`` contains exactly one ``## [X.Y.Z]`` (optionally
  ``## [X.Y.Z] - YYYY-MM-DD``) heading.

Nightly publication has no versioned-changelog requirement; it only needs the
single default version (``default-version``) for its CMake configure, and takes
its immutable tag from ``nightly_release_tag.py``.

Usage (from the repository root, as release.yml runs it)::

    stable_release_tag.py default-version [--root DIR]
    stable_release_tag.py verify vX.Y.Z [--root DIR]

Both print the version on stdout and exit 0; any contract violation prints a
diagnostic on stderr and exits 1 without printing a version.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

TAG_RE = re.compile(r"v(?P<version>[0-9]+\.[0-9]+\.[0-9]+)")
# Any set() call naming SPARK_ENGINE_VERSION, including multiline and indented
# forms, counts as a declaration; exactly one is allowed.
_DECLARATION = re.compile(r"(?im)^[ \t]*set\s*\(\s*SPARK_ENGINE_VERSION(?=\s|\))")
_DEFAULT = re.compile(r'(?m)^set\(SPARK_ENGINE_VERSION "(?P<version>[0-9]+\.[0-9]+\.[0-9]+)" CACHE\b')


class ContractError(ValueError):
    """The source tree does not satisfy the release tag contract."""


def default_version(cmake_text: str) -> str:
    """Return the single ``SPARK_ENGINE_VERSION`` CACHE default declared in CMakeLists.txt."""
    declarations = len(_DECLARATION.findall(cmake_text))
    if declarations != 1:
        raise ContractError(
            f"CMakeLists.txt must declare SPARK_ENGINE_VERSION exactly once; found {declarations}")
    match = _DEFAULT.search(cmake_text)
    if match is None:
        raise ContractError('CMakeLists.txt must declare set(SPARK_ENGINE_VERSION "X.Y.Z" CACHE ...)')
    return match.group("version")


def changelog_heading_count(changelog_text: str, version: str) -> int:
    """Count level-2 ``[version]`` headings, with or without an ISO release date."""
    heading = re.compile(r"## \[" + re.escape(version) + r"\](?: - \d{4}-\d{2}-\d{2})?[ \t]*")
    return sum(heading.fullmatch(line) is not None for line in changelog_text.splitlines())


def verify_stable_tag(tag: str, cmake_text: str, changelog_text: str | None) -> str:
    """Return the stable version for ``tag`` or raise :class:`ContractError`."""
    match = TAG_RE.fullmatch(tag)
    if match is None:
        raise ContractError(f"Release tag must have the form vMAJOR.MINOR.PATCH; got: {tag}")
    version = match.group("version")
    default = default_version(cmake_text)
    if version != default:
        raise ContractError(
            f"Stable tag {tag} must equal the single CMake SPARK_ENGINE_VERSION default {default}")
    if changelog_text is None:
        raise ContractError("Stable publication requires CHANGELOG.md")
    count = changelog_heading_count(changelog_text, version)
    if count != 1:
        raise ContractError(
            f"Stable publication requires exactly one CHANGELOG.md heading for [{version}]; found {count}")
    return version


def _read(path: Path) -> str | None:
    return path.read_text(encoding="utf-8") if path.is_file() else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--root", type=Path, default=Path("."), help="repository root (default: cwd)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("default-version", help="print the single CMake SPARK_ENGINE_VERSION default")
    verify = commands.add_parser("verify", help="verify a stable vX.Y.Z tag and print its version")
    verify.add_argument("tag")
    args = parser.parse_args(argv)

    try:
        cmake_text = _read(args.root / "CMakeLists.txt")
        if cmake_text is None:
            raise ContractError("CMakeLists.txt is missing")
        if args.command == "default-version":
            version = default_version(cmake_text)
        else:
            version = verify_stable_tag(args.tag, cmake_text, _read(args.root / "CHANGELOG.md"))
    except (ContractError, OSError, UnicodeDecodeError) as error:
        print(f"stable release tag contract failed: {error}", file=sys.stderr)
        return 1
    print(version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
