"""Root-run Wine/MinGW setup scripts must not execute or install unauthenticated bytes.

tools/setup-mingw-wine.sh (finding 43): packages come only through APT's signed-index verification. Its old
fallback re-fetched APT's URIs with wget and ran `dpkg -i --force-depends` on whatever arrived.

tools/build-wine-patched.sh (finding 42): the Wine source and every file the root build executes live in a
private directory. The old script trusted a predictable /tmp/wine-build and a pre-planted
/tmp/wine_<ver>~repack.orig.tar.xz.

The static checks run everywhere. The behavioral checks drive the real shell functions under bash with stub
commands on PATH and need POSIX ownership and permission semantics, so they run on Linux/macOS CI.
"""

from __future__ import annotations

import os
import re
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SETUP_SCRIPT = ROOT / "tools" / "setup-mingw-wine.sh"
BUILD_SCRIPT = ROOT / "tools" / "build-wine-patched.sh"
POSIX = os.name == "posix"


def _code(path: Path) -> str:
    """The script without comment lines, so prose about the old behavior does not count as code."""
    lines = path.read_text(encoding="utf-8").splitlines()
    return "\n".join(line for line in lines if not line.lstrip().startswith("#"))


def _bash() -> str:
    found = shutil.which("bash")
    if not found:
        raise AssertionError("bash not found on a POSIX host")
    return found


def _function_source(path: Path, name: str, *, required: bool = True) -> str:
    match = re.search(rf"^{re.escape(name)}\(\) \{{\n.*?^\}}\n", path.read_text(encoding="utf-8"), re.M | re.S)
    if match is None:
        if required:
            raise AssertionError(f"{name}() not found in {path.name}")
        return ""
    return match.group(0)


class SetupMingwWineStaticTests(unittest.TestCase):
    def test_no_unverified_package_download_path(self) -> None:
        code = _code(SETUP_SCRIPT)
        self.assertNotIn("--print-uris", code)
        self.assertNotIn("install_packages_direct", code)
        self.assertNotRegex(code, r"dpkg\s+-i", "packages must be installed by apt-get, not raw dpkg")
        self.assertNotIn("--force-depends", code)

    def test_apt_failures_are_not_swallowed(self) -> None:
        code = _code(SETUP_SCRIPT)
        self.assertNotRegex(code, r"apt-get update[^\n]*\|\|\s*true")


class BuildWinePatchedStaticTests(unittest.TestCase):
    def test_no_predictable_tmp_paths(self) -> None:
        code = _code(BUILD_SCRIPT)
        self.assertNotIn("/tmp/wine-build", code)
        self.assertNotIn("/tmp/wine_", code, "a tarball found in shared /tmp must never be trusted")
        self.assertNotIn("/tmp/spark-wine", code)

    def test_default_build_dir_is_private_and_checked(self) -> None:
        code = _code(BUILD_SCRIPT)
        self.assertIn('mktemp -d "${TMPDIR:-/tmp}/wine-build.', code)
        self.assertRegex(code, r'require_private_dir "\$BUILD_DIR"\ncd "\$BUILD_DIR"')

    def test_source_comes_from_apt_source_verification(self) -> None:
        code = _code(BUILD_SCRIPT)
        self.assertIn("apt-get source --download-only wine ||", code)


@unittest.skipUnless(POSIX, "needs POSIX ownership/permission semantics and a real bash")
class SetupMingwWineBehaviorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.marker = self.root / "unverified-install"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _stub(self, name: str, body: str) -> None:
        path = self.bin / name
        path.write_text(f"#!/bin/sh\n{body}\n", encoding="utf-8")
        path.chmod(0o755)

    def _run_install_packages(self, update_rc: int, install_rc: int) -> subprocess.CompletedProcess[str]:
        self._stub(
            "apt-get",
            f'case "$1" in update) exit {update_rc};; install) exit {install_rc};; '
            f'*) echo "\'http://mirror/pkg.deb\' pkg.deb 1 SHA256:00"; exit 0;; esac',
        )
        for tool in ("wget", "dpkg", "curl"):
            self._stub(tool, f'echo {tool} >> "{self.marker}"; exit 0')
        # Present so the post-install wrapper steps are no-ops and cannot fail the harness for another reason.
        for tool in ("wine64", "wineboot"):
            self._stub(tool, "exit 0")
        harness = (
            "set -euo pipefail\n"
            'ok() { :; }; warn() { :; }; fail() { echo "FAIL: $1" >&2; }; info() { :; }\n'
            + _function_source(SETUP_SCRIPT, "install_packages")
            # A fallback the script still defines must be callable, or the harness would fail for the wrong reason.
            + _function_source(SETUP_SCRIPT, "install_packages_direct", required=False)
            + "install_packages\n"
        )
        env = dict(os.environ, PATH=f"{self.bin}{os.pathsep}{os.environ.get('PATH', '')}")
        return subprocess.run([_bash(), "-c", harness], env=env, capture_output=True, text=True, timeout=60)

    def test_install_failure_fails_closed_without_fallback(self) -> None:
        result = self._run_install_packages(update_rc=0, install_rc=100)
        self.assertNotEqual(result.returncode, 0, "a failed apt-get install must stop the setup")
        self.assertFalse(self.marker.exists(), "nothing may be fetched or installed outside APT")

    def test_update_failure_fails_closed(self) -> None:
        result = self._run_install_packages(update_rc=100, install_rc=0)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("apt-get update failed", result.stderr)
        self.assertFalse(self.marker.exists())

    def test_successful_apt_install_succeeds(self) -> None:
        # Control: proves the harness itself does not fail, so the fail-closed results above mean something.
        result = self._run_install_packages(update_rc=0, install_rc=0)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.marker.exists())


@unittest.skipUnless(POSIX, "needs POSIX ownership/permission semantics and a real bash")
class BuildDirPrivacyBehaviorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        os.chmod(self.root, 0o700)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _require_private(self, target: Path) -> subprocess.CompletedProcess[str]:
        harness = (
            "set -euo pipefail\n"
            'fail() { echo "ERROR: $*" >&2; exit 1; }\n'
            + _function_source(BUILD_SCRIPT, "require_private_dir")
            + f'require_private_dir "{target}"\n'
        )
        return subprocess.run([_bash(), "-c", harness], capture_output=True, text=True, timeout=60)

    def _assert_rejected(self, target: Path) -> None:
        result = self._require_private(target)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ERROR: build directory", result.stderr, "rejected for the wrong reason")

    def test_private_directory_is_accepted(self) -> None:
        target = self.root / "build"
        target.mkdir(mode=0o700)
        result = self._require_private(target)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_world_writable_directory_is_rejected(self) -> None:
        target = self.root / "build"
        target.mkdir()
        os.chmod(target, 0o777)
        self._assert_rejected(target)

    def test_symlinked_directory_is_rejected(self) -> None:
        real = self.root / "real"
        real.mkdir(mode=0o700)
        link = self.root / "build"
        link.symlink_to(real, target_is_directory=True)
        self._assert_rejected(link)

    def test_writable_non_sticky_ancestor_is_rejected(self) -> None:
        shared = self.root / "shared"
        shared.mkdir()
        os.chmod(shared, 0o777)
        target = shared / "build"
        target.mkdir(mode=0o700)
        self._assert_rejected(target)

    def test_writable_sticky_ancestor_is_accepted(self) -> None:
        shared = self.root / "sticky"
        shared.mkdir()
        os.chmod(shared, 0o777 | stat.S_ISVTX)
        target = shared / "build"
        target.mkdir(mode=0o700)
        result = self._require_private(target)
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(POSIX and os.geteuid() == 0, "needs root to plant a directory owned by another uid")
    def test_directory_owned_by_another_user_is_rejected(self) -> None:
        target = self.root / "build"
        target.mkdir(mode=0o700)
        os.chown(target, 65534, 65534)
        self._assert_rejected(target)


if __name__ == "__main__":
    unittest.main()
