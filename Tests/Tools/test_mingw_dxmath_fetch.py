"""The MinGW toolchain's DirectXMath fetch must tie every header it exposes to the pinned SHA-256.

Drives cmake/toolchains/SparkVerifiedDirectXMath.cmake through `cmake -P` against file:// archives:
a hash-mismatched download or a tampered cached archive must never reach the include path, not even on the
configure after the one that failed, and an extraction without a matching stamp must be redone.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "cmake" / "toolchains" / "SparkVerifiedDirectXMath.cmake"
TOOLCHAIN = ROOT / "cmake" / "toolchains" / "mingw-w64-x86_64.cmake"
ARCHIVE_ROOT = "DirectXMath-oct2024"
GENUINE_HEADER = "// genuine DirectXMath\n"
HOSTILE_HEADER = "// hostile DirectXMath\n"


def _cmake() -> str:
    # A missing cmake must fail the test, never skip it: a skipped supply-chain check reads as a pass.
    found = os.environ.get("SPARK_CMAKE") or shutil.which("cmake")
    if not found:
        raise AssertionError("cmake not found; set SPARK_CMAKE")
    return found


def _make_archive(path: Path, header: str) -> str:
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(f"{ARCHIVE_ROOT}/Inc/DirectXMath.h", header)
    return hashlib.sha256(path.read_bytes()).hexdigest()


class VerifiedDirectXMathFetchTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.cache = self.root / "cache"
        self.genuine = self.root / "genuine.zip"
        self.hostile = self.root / "hostile.zip"
        self.pinned = _make_archive(self.genuine, GENUINE_HEADER)
        _make_archive(self.hostile, HOSTILE_HEADER)
        self.driver = self.root / "driver.cmake"
        self.driver.write_text(
            f'include("{HELPER.as_posix()}")\n'
            "spark_fetch_verified_directxmath(\"${CACHE}\" \"${URL}\" \"${PINNED}\" "
            f'"{ARCHIVE_ROOT}" inc)\n'
            'file(WRITE "${CACHE}/result.txt" "${inc}")\n',
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _configure(self, source: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                _cmake(),
                f"-DCACHE={self.cache.as_posix()}",
                f"-DURL={source.resolve().as_uri()}",
                f"-DPINNED={self.pinned}",
                "-P",
                str(self.driver),
            ],
            capture_output=True,
            text=True,
            timeout=120,
        )

    def _exposed_header(self) -> str:
        inc = Path((self.cache / "result.txt").read_text(encoding="utf-8"))
        return (inc / "DirectXMath.h").read_text(encoding="utf-8")

    def _extracted_headers(self) -> list[str]:
        return [path.read_text(encoding="utf-8") for path in self.cache.rglob("DirectXMath.h")]

    def test_genuine_archive_is_extracted_and_stamped(self) -> None:
        result = self._configure(self.genuine)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self._exposed_header(), GENUINE_HEADER)
        stamp = self.cache / "extract" / ".verified-sha256"
        self.assertEqual(stamp.read_text(encoding="utf-8").strip(), self.pinned)

    def test_mismatched_download_is_never_trusted_on_a_later_configure(self) -> None:
        first = self._configure(self.hostile)
        self.assertNotEqual(first.returncode, 0, "a mismatched download must stop the configure")
        self.assertFalse((self.cache / f"{ARCHIVE_ROOT}.zip").exists(), "mismatched bytes became the cache")
        # The finding: the old code skipped verification when the file already existed.
        second = self._configure(self.hostile)
        self.assertNotEqual(second.returncode, 0)
        self.assertNotIn(HOSTILE_HEADER, self._extracted_headers())
        self.assertFalse((self.cache / "result.txt").exists())

    def test_tampered_cached_archive_is_discarded_and_refetched(self) -> None:
        self.cache.mkdir()
        shutil.copyfile(self.hostile, self.cache / f"{ARCHIVE_ROOT}.zip")
        result = self._configure(self.genuine)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self._exposed_header(), GENUINE_HEADER)

    def test_tampered_cached_archive_fails_closed_when_refetch_fails(self) -> None:
        self.cache.mkdir()
        shutil.copyfile(self.hostile, self.cache / f"{ARCHIVE_ROOT}.zip")
        result = self._configure(self.root / "missing.zip")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(HOSTILE_HEADER, self._extracted_headers())

    def test_unstamped_extraction_is_replaced(self) -> None:
        self.assertEqual(self._configure(self.genuine).returncode, 0)
        planted = self.cache / "extract" / ARCHIVE_ROOT / "Inc" / "DirectXMath.h"
        planted.write_text(HOSTILE_HEADER, encoding="utf-8")
        (self.cache / "extract" / ".verified-sha256").unlink()
        result = self._configure(self.genuine)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self._exposed_header(), GENUINE_HEADER)

    def test_toolchain_routes_the_fetch_through_the_verified_helper(self) -> None:
        lines = TOOLCHAIN.read_text(encoding="utf-8").splitlines()
        text = "\n".join(line for line in lines if not line.lstrip().startswith("#"))
        self.assertIn("SparkVerifiedDirectXMath.cmake", text)
        self.assertIn("spark_fetch_verified_directxmath(", text)
        self.assertNotIn("file(DOWNLOAD", text, "the toolchain must not fetch outside the verified helper")
        self.assertNotIn("file(ARCHIVE_EXTRACT", text, "the toolchain must not extract unverified archives")


if __name__ == "__main__":
    unittest.main()
