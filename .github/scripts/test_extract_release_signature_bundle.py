#!/usr/bin/env python3
"""Adversarial archive metadata tests for signature bundle provisioning."""

from __future__ import annotations

import io
from pathlib import Path
import tarfile
import tempfile
import unittest

from extract_release_signature_bundle import ArchiveError, extract_archive


class SignatureArchiveTests(unittest.TestCase):
    def _archive(self, members: list[tarfile.TarInfo]) -> tuple[tempfile.TemporaryDirectory, Path]:
        temporary = tempfile.TemporaryDirectory()
        path = Path(temporary.name) / "bundle.tar.gz"
        with tarfile.open(path, "w:gz") as stream:
            for member in members:
                stream.addfile(member, io.BytesIO(b"payload") if member.isfile() else None)
        return temporary, path

    def _member(self, name: str, kind: str = "file") -> tarfile.TarInfo:
        member = tarfile.TarInfo(name)
        member.size = 7
        if kind == "symlink":
            member.type = tarfile.SYMTYPE
            member.linkname = "public.pem"
        elif kind == "hardlink":
            member.type = tarfile.LNKTYPE
            member.linkname = "public.pem"
        elif kind == "fifo":
            member.type = tarfile.FIFOTYPE
        elif kind == "device":
            member.type = tarfile.CHRTYPE
        return member

    def test_extracts_only_regular_files(self) -> None:
        temporary, archive = self._archive([self._member("public.pem")])
        try:
            destination = Path(temporary.name) / "out"
            extract_archive(archive, destination)
            self.assertEqual((destination / "public.pem").read_bytes(), b"payload")
        finally:
            temporary.cleanup()

    def test_rejects_nonregular_members(self) -> None:
        for kind in ("symlink", "hardlink", "fifo", "device"):
            with self.subTest(kind=kind):
                temporary, archive = self._archive([self._member("public.pem", kind)])
                try:
                    with self.assertRaisesRegex(ArchiveError, "not a regular file"):
                        extract_archive(archive, Path(temporary.name) / "out")
                finally:
                    temporary.cleanup()

    def test_rejects_traversal_absolute_and_ambiguous_names(self) -> None:
        for name in ("../escape", "/absolute", "dir\\escape", "public.pem "):
            with self.subTest(name=name):
                temporary, archive = self._archive([self._member(name)])
                try:
                    with self.assertRaises(ArchiveError):
                        extract_archive(archive, Path(temporary.name) / "out")
                finally:
                    temporary.cleanup()

    def test_rejects_casefold_duplicate_members(self) -> None:
        temporary, archive = self._archive([self._member("public.pem"), self._member("PUBLIC.PEM")])
        try:
            with self.assertRaisesRegex(ArchiveError, "case-fold duplicate"):
                extract_archive(archive, Path(temporary.name) / "out")
        finally:
            temporary.cleanup()


if __name__ == "__main__":
    unittest.main()
