#!/usr/bin/env python3
"""Safely validate and extract an externally provisioned signature tarball."""

from __future__ import annotations

import argparse
from pathlib import Path, PurePosixPath
import shutil
import tarfile


class ArchiveError(ValueError):
    """The external signature archive is unsafe or malformed."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ArchiveError(message)


def _member_name(member: tarfile.TarInfo) -> str:
    name = member.name
    _require(isinstance(name, str) and bool(name), "archive member has an empty name")
    _require("\\" not in name and ":" not in name and not name.startswith("/"),
             f"unsafe archive member path: {name!r}")
    path = PurePosixPath(name)
    _require(not path.is_absolute() and ".." not in path.parts,
             f"archive member escapes extraction root: {name!r}")
    normalized = str(path)
    _require(normalized not in {"", "."}, f"archive member has an invalid path: {name!r}")
    _require(normalized == name.rstrip("/"), f"archive member path is not normalized: {name!r}")
    _require(len(path.parts) == 1, f"signature archive must be flat: {name!r}")
    _require(not normalized.endswith((".", " ")),
             f"archive member has an ambiguous Windows name: {name!r}")
    stem = normalized.split(".", 1)[0].casefold()
    _require(stem not in {"con", "prn", "aux", "nul"}
             and not (len(stem) == 4 and stem[:3] in {"com", "lpt"} and stem[3].isdigit()),
             f"archive member uses a reserved Windows device name: {name!r}")
    return normalized


def extract_archive(archive: Path, destination: Path) -> None:
    _require(archive.is_file() and not archive.is_symlink(), "signature archive is missing")
    destination.mkdir(parents=True, exist_ok=False)
    seen: set[str] = set()
    try:
        with tarfile.open(archive, mode="r:gz") as stream:
            members = stream.getmembers()
            _require(bool(members), "signature archive is empty")
            for member in members:
                name = _member_name(member)
                _require(name in {"release-signatures.json", "spark-release-public-key.pem"}
                         or name.casefold().endswith(".sig"),
                         f"archive member is not an approved signature control or .sig: {name!r}")
                folded = name.casefold()
                _require(folded not in seen, f"archive contains a case-fold duplicate: {name!r}")
                seen.add(folded)
                _require(member.isfile(), f"archive member is not a regular file: {name!r}")
            _require("release-signatures.json" in seen and "spark-release-public-key.pem" in seen,
                     "signature archive is missing a required control file")
            stream.extractall(destination, members=members, filter="data")
    except ArchiveError:
        shutil.rmtree(destination, ignore_errors=True)
        raise
    except (OSError, tarfile.TarError) as exc:
        shutil.rmtree(destination, ignore_errors=True)
        raise ArchiveError(f"cannot validate or extract signature archive: {exc}") from exc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        extract_archive(args.archive, args.destination)
    except ArchiveError as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
