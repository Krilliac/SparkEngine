#!/usr/bin/env python3
"""Adversarial tests for tools/check-thirdparty-manifest-sync.sh.

The manifest-sync gate must stay fail-closed on real ThirdParty payload and
dependency-wiring drift while not firing on the ThirdParty governance metadata
that describes the policy itself (POLICY.md, README.md, supply-chain.lock).

Every case builds a throwaway git repository in a temporary directory.  This
suite must never edit a tracked repository file while it is running.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_REL = "tools/check-thirdparty-manifest-sync.sh"
MANIFEST_REL = "ThirdParty/dependencies.lock"
CMAKE_MODULE_REL = "cmake/SparkThirdPartyAudit.cmake"

# A minimal but schema-valid manifest.  The real repository manifest cannot be
# reused because cmake/SparkThirdPartyAudit.cmake resolves and reads every
# license notice it names, and those payload files do not exist in the fixture.
FIXTURE_MANIFEST = """# Fixture manifest for manifest-sync tests.
set(SPARK_THIRDPARTY_AUDIT_ENTRIES
    "stb_image|https://github.com/nothings/stb|snapshot|MIT|ThirdParty/Utils/stb|stb_image.h|SPARK_HAS_STB_IMAGE|SparkEngine/Source/Graphics/TextureSystem.cpp|WARN|ThirdParty/Utils/stb/LICENSE"
)
"""

FIXTURE_LICENSE = """MIT License

Copyright (c) 2026 SparkEngine manifest-sync fixture

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
"""

IN_CI = os.environ.get("CI") == "true" or os.environ.get("GITHUB_ACTIONS") == "true"


def _require(tool: str) -> str:
    """Locate a required external tool.

    In CI a missing tool is a hard failure: a gate that cannot run is
    indistinguishable from a gate that passes.  Locally it degrades to a skip
    with an explicit reason.
    """
    path = shutil.which(tool)
    if path:
        return path
    message = f"required tool not found on PATH: {tool}"
    if IN_CI:
        raise AssertionError(message)
    raise unittest.SkipTest(message)


class ManifestSyncHarness(unittest.TestCase):
    """Builds a minimal repository that the real script can be run against."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.bash = _require("bash")
        cls.git = _require("git")
        cls.cmake = _require("cmake")

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="spark-manifest-sync-")
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"

        (self.repo / "tools").mkdir(parents=True)
        (self.repo / "cmake").mkdir(parents=True)
        (self.repo / "ThirdParty" / "Utils" / "stb").mkdir(parents=True)

        shutil.copy2(REPO_ROOT / SCRIPT_REL, self.repo / SCRIPT_REL)
        os.chmod(self.repo / SCRIPT_REL, 0o755)
        shutil.copy2(REPO_ROOT / CMAKE_MODULE_REL, self.repo / CMAKE_MODULE_REL)
        self.write(MANIFEST_REL, FIXTURE_MANIFEST)
        self.write("ThirdParty/Utils/stb/LICENSE", FIXTURE_LICENSE)
        self.write("ThirdParty/POLICY.md", "# policy\n")
        self.write("ThirdParty/README.md", "# readme\n")
        self.write("ThirdParty/supply-chain.lock", '{"version": 1}\n')
        self.write("ThirdParty/Utils/stb/stb_image.h", "/* baseline */\n")
        self.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)\n")
        self.write(".gitmodules", "")

        self.git_run("init", "-q", "-b", "Working")
        self.git_run("config", "user.email", "test@example.invalid")
        self.git_run("config", "user.name", "Manifest Sync Test")
        self.git_run("config", "commit.gpgsign", "false")
        self.git_run("add", "-A")
        self.git_run("commit", "-q", "-m", "baseline")

    # ── helpers ──────────────────────────────────────────────────────

    def write(self, rel: str, content: str) -> None:
        target = self.repo / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8", newline="\n")

    def git_run(self, *args: str) -> str:
        result = subprocess.run(
            [self.git, *args],
            cwd=str(self.repo),
            capture_output=True,
            text=True,
            timeout=120,
        )
        self.assertEqual(
            result.returncode,
            0,
            f"git {' '.join(args)} failed: {result.stderr.strip()}",
        )
        return result.stdout

    def commit(self, message: str) -> None:
        self.git_run("add", "-A")
        self.git_run("commit", "-q", "-m", message)

    def run_check(self, *, ci: bool) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ)
        env.pop("GITHUB_EVENT_NAME", None)
        env.pop("GITHUB_ACTOR", None)
        env.pop("GITHUB_EVENT_PATH", None)
        if ci:
            env["CI"] = "true"
            env["GITHUB_ACTIONS"] = "true"
        else:
            env.pop("CI", None)
            env.pop("GITHUB_ACTIONS", None)
        return subprocess.run(
            [self.bash, SCRIPT_REL],
            cwd=str(self.repo),
            capture_output=True,
            text=True,
            timeout=300,
            env=env,
        )

    def assert_passes(self, result: subprocess.CompletedProcess[str]) -> None:
        self.assertEqual(
            result.returncode,
            0,
            f"expected pass, got {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )

    def assert_blocks(self, result: subprocess.CompletedProcess[str]) -> None:
        self.assertEqual(
            result.returncode,
            1,
            f"expected manifest-sync block, got {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        self.assertIn("was not updated", result.stdout)


class GovernanceExemptionTests(ManifestSyncHarness):
    """Governance metadata must not demand a dependencies.lock bump."""

    def test_baseline_clean_tree_passes(self) -> None:
        self.assert_passes(self.run_check(ci=False))

    def test_policy_only_change_passes_in_ci_mode(self) -> None:
        self.write("ThirdParty/POLICY.md", "# policy\n\nrevised\n")
        self.commit("policy edit")
        self.assert_passes(self.run_check(ci=True))

    def test_all_governance_paths_together_pass(self) -> None:
        self.write("ThirdParty/POLICY.md", "# policy v2\n")
        self.write("ThirdParty/README.md", "# readme v2\n")
        self.write("ThirdParty/supply-chain.lock", '{"version": 1, "n": 2}\n')
        self.commit("governance edit")
        self.assert_passes(self.run_check(ci=True))

    def test_governance_change_in_worktree_mode_passes(self) -> None:
        self.write("ThirdParty/supply-chain.lock", '{"version": 1, "n": 3}\n')
        self.assert_passes(self.run_check(ci=False))


class PayloadDriftStillBlockedTests(ManifestSyncHarness):
    """RED proofs: the exemption must not weaken real drift detection."""

    def test_vendored_payload_edit_blocks(self) -> None:
        self.write("ThirdParty/Utils/stb/stb_image.h", "/* tampered */\n")
        self.commit("payload edit")
        self.assert_blocks(self.run_check(ci=True))

    def test_payload_edit_alongside_governance_blocks(self) -> None:
        self.write("ThirdParty/POLICY.md", "# policy v2\n")
        self.write("ThirdParty/Utils/stb/stb_image.h", "/* tampered */\n")
        self.commit("governance plus payload")
        self.assert_blocks(self.run_check(ci=True))

    def test_new_unlisted_thirdparty_root_file_blocks(self) -> None:
        self.write("ThirdParty/EVIL.md", "payload smuggled at the root\n")
        self.commit("new root file")
        self.assert_blocks(self.run_check(ci=True))

    def test_case_variant_of_governance_path_blocks(self) -> None:
        # A case-variant path is a different tracked file; it must not inherit
        # the exemption on a case-sensitive filesystem, and on a
        # case-insensitive one git still records the distinct name.
        self.write("ThirdParty/policy_alias.md", "not the governance file\n")
        self.commit("case variant")
        self.assert_blocks(self.run_check(ci=True))

    def test_nested_path_under_governance_prefix_blocks(self) -> None:
        self.write("ThirdParty/POLICY.md.d/payload.h", "/* smuggled */\n")
        self.commit("prefix lookalike directory")
        self.assert_blocks(self.run_check(ci=True))

    def test_untracked_payload_file_blocks(self) -> None:
        self.write("ThirdParty/Utils/stb/rogue.h", "/* untracked */\n")
        self.assert_blocks(self.run_check(ci=False))

    def test_untracked_governance_only_file_passes(self) -> None:
        self.write("ThirdParty/POLICY.md", "# policy edited in worktree\n")
        self.assert_passes(self.run_check(ci=False))

    def test_new_submodule_wiring_blocks(self) -> None:
        self.write(
            ".gitmodules",
            '[submodule "ThirdParty/New/dep"]\n'
            "\tpath = ThirdParty/New/dep\n"
            "\turl = https://example.invalid/dep.git\n",
        )
        self.commit("wiring edit")
        self.assert_blocks(self.run_check(ci=True))

    def test_payload_edit_with_manifest_bump_passes(self) -> None:
        self.write("ThirdParty/Utils/stb/stb_image.h", "/* updated */\n")
        manifest = self.repo / MANIFEST_REL
        manifest.write_text(
            manifest.read_text(encoding="utf-8") + "\n# resynced\n",
            encoding="utf-8",
            newline="\n",
        )
        self.commit("payload plus manifest")
        self.assert_passes(self.run_check(ci=True))


class GovernanceListContractTests(unittest.TestCase):
    """The allowlist itself is part of the security contract."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.script = (REPO_ROOT / SCRIPT_REL).read_text(encoding="utf-8")

    def test_allowlist_is_exactly_the_three_governance_paths(self) -> None:
        start = self.script.index("THIRDPARTY_GOVERNANCE_PATHS=(")
        end = self.script.index(")", start)
        block = self.script[start:end]
        entries = sorted(
            line.strip().strip('"')
            for line in block.splitlines()[1:]
            if line.strip()
        )
        self.assertEqual(
            entries,
            [
                "ThirdParty/POLICY.md",
                "ThirdParty/README.md",
                "ThirdParty/supply-chain.lock",
            ],
            "manifest-sync governance allowlist changed — widening it requires "
            "matching bidirectional reconciliation in check-supply-chain.py",
        )

    def test_allowlist_uses_exact_match_not_prefix_match(self) -> None:
        self.assertIn('if [ "$changed_path" = "$governance_path" ]', self.script)
        self.assertNotIn("ThirdParty/POLICY.md*", self.script)


if __name__ == "__main__":
    unittest.main(verbosity=2)
