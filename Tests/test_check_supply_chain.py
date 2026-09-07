#!/usr/bin/env python3
"""Adversarial tests for tools/check-supply-chain.py.

Every negative case starts from a fake repository that the real checker passes
cleanly, mutates exactly one thing, and requires the checker to fail.  A test
that would still pass with the guard it names deleted is not a test, so the
suite is built so that the mutation is the only difference from green.

Nothing here writes inside the SparkEngine repository.  Fixtures live in
temporary directories and are removed on teardown.
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TESTS_DIR.parent
CHECKER_REL = "tools/check-supply-chain.py"
AUDIT_CMAKE_REL = "cmake/SparkThirdPartyAudit.cmake"

_spec = importlib.util.spec_from_file_location(
    "check_supply_chain", str(PROJECT_ROOT / CHECKER_REL),
)
sc = importlib.util.module_from_spec(_spec)
sys.modules["check_supply_chain"] = sc
_spec.loader.exec_module(sc)

IN_CI = os.environ.get("CI") == "true" or os.environ.get("GITHUB_ACTIONS") == "true"


def _require(tool: str) -> str:
    """A gate that cannot run is not a gate. In CI, missing tooling fails."""
    path = shutil.which(tool)
    if path:
        return path
    message = f"required tool not found on PATH: {tool}"
    if IN_CI:
        raise AssertionError(message)
    raise unittest.SkipTest(message)


def _require_yaml():
    try:
        import yaml  # noqa: PLC0415
    except ImportError as exc:
        if IN_CI:
            raise AssertionError("PyYAML is required in CI") from exc
        raise unittest.SkipTest("PyYAML not installed")
    return yaml


LICENSE_TEXT = """MIT License

Copyright (c) 2026 SparkEngine supply-chain fixture

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
"""

MANIFEST_TEXT = """# Fixture dependency manifest.
set(SPARK_THIRDPARTY_AUDIT_ENTRIES
    "demo|https://github.com/example/demo|v1.2.3 (vendored snapshot)|MIT|ThirdParty/Utils/demo|demo.h,demo_impl.cpp|SPARK_HAS_DEMO|SparkEngine/Source/DemoStub.cpp|WARN|ThirdParty/Utils/demo/LICENSE"
)
"""

WORKFLOW_TEXT = """name: fixture
on: [push]
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
      - run: |
          echo 'uses: actions/checkout@v4 is script text, not a key'
"""

CHECKOUT_SHA = "3d3c42e5aac5ba805825da76410c181273ba90b1"

LOCK_SKELETON = {
    "version": 2,
    "description": "fixture lockfile",
    "submodule_gitlinks": {},
    "managed_vendored_dirs": ["ThirdParty/Utils/demo"],
    "project_owned_dirs": {
        "ThirdParty/Local": "First-party fixture directory authored in this repository.",
    },
    "allowed_root_files": [
        "ThirdParty/POLICY.md",
        "ThirdParty/dependencies.lock",
        "ThirdParty/supply-chain.lock",
    ],
    "tree_digests": {},
    "sentinel_files": {},
    "action_pins": {},
}


# ═══════════════════════════════════════════════════════════════════════
# Fake repository fixture
# ═══════════════════════════════════════════════════════════════════════

class _Baseline:
    """A fake repository the real checker passes, built once and copied."""

    path: Path | None = None
    _holder: tempfile.TemporaryDirectory | None = None

    @classmethod
    def get(cls) -> Path:
        if cls.path is None:
            cls._holder = tempfile.TemporaryDirectory(prefix="spark-sc-baseline-")
            cls.path = cls._build(Path(cls._holder.name) / "repo")
        return cls.path

    @classmethod
    def _build(cls, repo: Path) -> Path:
        git = _require("git")
        _require("cmake")
        _require_yaml()

        def write(rel: str, content: str) -> None:
            target = repo / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8", newline="\n")

        (repo / "tools").mkdir(parents=True)
        (repo / "cmake").mkdir(parents=True)
        shutil.copy2(PROJECT_ROOT / CHECKER_REL, repo / CHECKER_REL)
        shutil.copy2(PROJECT_ROOT / AUDIT_CMAKE_REL, repo / AUDIT_CMAKE_REL)

        write("ThirdParty/dependencies.lock", MANIFEST_TEXT)
        write("ThirdParty/POLICY.md", "# fixture policy\n")
        write("ThirdParty/Utils/demo/demo.h", "/* demo header */\n")
        write("ThirdParty/Utils/demo/demo_impl.cpp", "/* demo impl */\n")
        write("ThirdParty/Utils/demo/LICENSE", LICENSE_TEXT)
        write("ThirdParty/Utils/demo/nested/deep/extra.h", "/* nested payload */\n")
        write("ThirdParty/Local/notes.h", "/* first-party */\n")
        write("ThirdParty/supply-chain.lock", json.dumps(LOCK_SKELETON, indent=2) + "\n")
        write(".gitmodules", "")
        write(".github/workflows/ci.yml", WORKFLOW_TEXT)
        write("SparkEngine/Source/DemoStub.cpp", "// stub\n")

        for args in (
            ["init", "-q", "-b", "Working"],
            ["config", "user.email", "fixture@example.invalid"],
            ["config", "user.name", "Supply Chain Fixture"],
            ["config", "commit.gpgsign", "false"],
            ["config", "core.autocrlf", "false"],
            ["add", "-A"],
            ["commit", "-q", "-m", "baseline"],
        ):
            done = subprocess.run(
                [git, *args], cwd=str(repo), capture_output=True, text=True, timeout=120
            )
            if done.returncode != 0:
                raise AssertionError(f"git {args}: {done.stderr}")

        done = subprocess.run(
            [sys.executable, CHECKER_REL, "--update"],
            cwd=str(repo), capture_output=True, text=True, timeout=300,
        )
        if done.returncode != 0:
            raise AssertionError(
                f"baseline --update failed:\n{done.stdout}\n{done.stderr}"
            )
        subprocess.run([git, "add", "-A"], cwd=str(repo), capture_output=True, timeout=120)
        subprocess.run(
            [git, "commit", "-q", "-m", "lockfile"],
            cwd=str(repo), capture_output=True, timeout=120,
        )
        return repo


class FakeRepoCase(unittest.TestCase):
    """Per-test copy of the passing baseline; mutate one thing, expect failure."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.baseline = _Baseline.get()
        cls.git = _require("git")

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="spark-sc-case-")
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"
        shutil.copytree(self.baseline, self.repo)

    # ── mutation helpers ─────────────────────────────────────────────

    def write(self, rel: str, content: str) -> None:
        target = self.repo / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8", newline="\n")

    def lock(self) -> dict:
        return json.loads((self.repo / sc.LOCKFILE_REL).read_text(encoding="utf-8"))

    def set_lock(self, data: dict) -> None:
        self.write(sc.LOCKFILE_REL, json.dumps(data, indent=2) + "\n")

    def git_run(self, *args: str) -> None:
        done = subprocess.run(
            [self.git, *args], cwd=str(self.repo),
            capture_output=True, text=True, timeout=120,
        )
        if done.returncode != 0:
            raise AssertionError(f"git {args}: {done.stderr}")

    def commit(self, message: str = "mutation") -> None:
        self.git_run("add", "-A")
        self.git_run("commit", "-q", "-m", message)

    # ── invocation ───────────────────────────────────────────────────

    def check(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, CHECKER_REL, *args],
            cwd=str(self.repo), capture_output=True, text=True, timeout=300,
        )

    def assert_baseline_passes(self) -> None:
        done = self.check()
        self.assertEqual(
            done.returncode, 0,
            f"baseline must pass before mutation\n{done.stdout}\n{done.stderr}",
        )

    def assert_violation(self, *needles: str) -> subprocess.CompletedProcess[str]:
        done = self.check("--json")
        self.assertEqual(
            done.returncode, 1,
            f"expected a policy violation (exit 1), got {done.returncode}\n"
            f"{done.stdout}\n{done.stderr}",
        )
        payload = json.loads(done.stdout)
        self.assertFalse(payload["passed"])
        blob = json.dumps(payload["violations"])
        for needle in needles:
            self.assertIn(needle, blob, f"missing {needle!r} in {blob}")
        return done

    def assert_fatal(self, *needles: str) -> None:
        done = self.check()
        self.assertEqual(
            done.returncode, 2,
            f"expected checker-level failure (exit 2), got {done.returncode}\n"
            f"{done.stdout}\n{done.stderr}",
        )
        for needle in needles:
            self.assertIn(needle, done.stderr)


# ═══════════════════════════════════════════════════════════════════════
# Baseline sanity — every negative case below depends on this being green
# ═══════════════════════════════════════════════════════════════════════

class TestBaseline(FakeRepoCase):

    def test_untouched_fixture_passes(self) -> None:
        self.assert_baseline_passes()

    def test_json_output_is_machine_readable(self) -> None:
        done = self.check("--json")
        self.assertEqual(done.returncode, 0)
        payload = json.loads(done.stdout)
        self.assertTrue(payload["passed"])
        self.assertEqual(payload["violation_count"], 0)
        self.assertGreaterEqual(payload["tree_digest_count"], 2)

    def test_ci_flag_matches_default_verdict(self) -> None:
        default = self.check("--json")
        ci_mode = self.check("--ci", "--json")
        self.assertEqual(default.returncode, ci_mode.returncode)
        self.assertEqual(json.loads(default.stdout), json.loads(ci_mode.stdout))


# ═══════════════════════════════════════════════════════════════════════
# Complete tracked inventory — the two-level walk's blind spots
# ═══════════════════════════════════════════════════════════════════════

class TestTrackedInventory(FakeRepoCase):

    def test_payload_nested_below_depth_two_is_detected(self) -> None:
        self.write("ThirdParty/Utils/demo/nested/deep/backdoor.h", "/* payload */\n")
        self.commit()
        self.assert_violation("tracked tree digest drift")

    def test_content_added_inside_a_managed_dir_is_detected(self) -> None:
        self.write("ThirdParty/Utils/demo/extra_impl.cpp", "/* payload */\n")
        self.commit()
        self.assert_violation("tracked tree digest drift")

    def test_content_added_inside_a_project_owned_dir_is_detected(self) -> None:
        self.write("ThirdParty/Local/injected.h", "/* payload */\n")
        self.commit()
        self.assert_violation("tracked tree digest drift")

    def test_edit_to_an_unsentinelled_file_is_detected(self) -> None:
        self.write("ThirdParty/Utils/demo/nested/deep/extra.h", "/* tampered */\n")
        self.commit()
        self.assert_violation("tracked tree digest drift")

    def test_file_at_depth_one_of_an_undeclared_dir_is_detected(self) -> None:
        self.write("ThirdParty/Rogue/toolchain.cmake", "# payload\n")
        self.commit()
        self.assert_violation("tracked file is in no declared container")

    def test_dot_prefixed_directory_is_detected(self) -> None:
        self.write("ThirdParty/.build/payload.cmake", "# payload\n")
        self.commit()
        self.assert_violation("tracked file is in no declared container")

    def test_dot_prefixed_root_file_is_detected(self) -> None:
        self.write("ThirdParty/.toolchain.cmake", "# payload\n")
        self.commit()
        self.assert_violation("tracked file is in no declared container")

    def test_undeclared_root_file_is_detected(self) -> None:
        self.write("ThirdParty/EVIL.md", "payload at the root\n")
        self.commit()
        self.assert_violation("tracked file is in no declared container")

    def test_untracked_payload_is_detected(self) -> None:
        self.write("ThirdParty/Utils/demo/untracked.h", "/* payload */\n")
        self.assert_violation("worktree differs from the index")

    def test_worktree_edit_without_commit_is_detected(self) -> None:
        self.write("ThirdParty/Utils/demo/nested/deep/extra.h", "/* tampered */\n")
        self.assert_violation("worktree differs from the index")

    def test_deleted_payload_is_detected(self) -> None:
        (self.repo / "ThirdParty/Utils/demo/nested/deep/extra.h").unlink()
        self.assert_violation("worktree differs from the index")

    def test_declared_container_with_no_tracked_files_is_rejected(self) -> None:
        data = self.lock()
        data["managed_vendored_dirs"].append("ThirdParty/Future")
        data["tree_digests"]["ThirdParty/Future"] = {
            "digest": "0" * 64, "file_count": 0,
        }
        self.set_lock(data)
        self.assert_violation("holds no tracked files")

    def test_tree_digest_covers_every_non_submodule_container(self) -> None:
        data = self.lock()
        del data["tree_digests"]["ThirdParty/Local"]
        self.set_lock(data)
        self.assert_violation("has no tree digest")

    def test_file_count_drift_alone_is_detected(self) -> None:
        data = self.lock()
        data["tree_digests"]["ThirdParty/Local"]["file_count"] += 1
        self.set_lock(data)
        self.assert_violation("file count drift", "ThirdParty/Local")

    def test_digest_for_unknown_container_is_rejected(self) -> None:
        data = self.lock()
        data["tree_digests"]["ThirdParty/Nowhere"] = {
            "digest": "a" * 64, "file_count": 1,
        }
        self.set_lock(data)
        self.assert_violation("not a declared non-submodule")


# ═══════════════════════════════════════════════════════════════════════
# Container model — categories must not overlap, nest, or alias
# ═══════════════════════════════════════════════════════════════════════

class TestContainerModel(FakeRepoCase):

    def test_cross_category_overlap_is_rejected(self) -> None:
        data = self.lock()
        data["project_owned_dirs"]["ThirdParty/Utils/demo"] = (
            "claiming a vendored directory as first-party"
        )
        self.set_lock(data)
        self.assert_violation("categories must be disjoint")

    def test_nested_containers_are_rejected(self) -> None:
        data = self.lock()
        data["managed_vendored_dirs"].append("ThirdParty/Utils/demo/nested")
        data["tree_digests"]["ThirdParty/Utils/demo/nested"] = {
            "digest": "0" * 64, "file_count": 0,
        }
        self.set_lock(data)
        self.assert_violation("nested inside container")

    def test_case_variant_container_alias_is_rejected(self) -> None:
        data = self.lock()
        data["managed_vendored_dirs"].append("ThirdParty/Utils/DEMO")
        data["tree_digests"]["ThirdParty/Utils/DEMO"] = {
            "digest": "0" * 64, "file_count": 0,
        }
        self.set_lock(data)
        self.assert_violation("case-variant alias")

    def test_project_owned_dir_requires_a_justification(self) -> None:
        data = self.lock()
        data["project_owned_dirs"]["ThirdParty/Local"] = "mine"
        self.set_lock(data)
        self.assert_fatal("recorded justification")

    def test_declared_container_missing_from_disk_is_rejected(self) -> None:
        shutil.rmtree(self.repo / "ThirdParty/Local")
        self.assert_violation("does not exist on disk")


# ═══════════════════════════════════════════════════════════════════════
# Link and reparse-point hygiene
# ═══════════════════════════════════════════════════════════════════════

class TestLinkHygiene(FakeRepoCase):

    def _make_outside_tree(self) -> Path:
        outside = Path(self._tmp.name) / "outside"
        (outside / "payload").mkdir(parents=True)
        (outside / "payload" / "evil.h").write_text("/* outside */\n", encoding="utf-8")
        return outside / "payload"

    @unittest.skipUnless(sys.platform == "win32", "junctions are a Windows concept")
    def test_directory_junction_is_rejected(self) -> None:
        target = self._make_outside_tree()
        link = self.repo / "ThirdParty" / "Junction"
        done = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            capture_output=True, text=True, timeout=60,
        )
        if done.returncode != 0:
            self.fail(f"could not create junction: {done.stdout}{done.stderr}")
        # A junction reports is_symlink() == False and stat() clears the
        # reparse bit, so only an lstat-based check can see it.
        self.assertFalse(link.is_symlink())
        self.assertIsNotNone(sc.link_reason(link))
        self.assert_violation("reparse point")

    @unittest.skipUnless(sys.platform == "win32", "junctions are a Windows concept")
    def test_junction_replacing_a_managed_dir_is_rejected(self) -> None:
        target = self._make_outside_tree()
        managed = self.repo / "ThirdParty" / "Local"
        shutil.rmtree(managed)
        done = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(managed), str(target)],
            capture_output=True, text=True, timeout=60,
        )
        if done.returncode != 0:
            self.fail(f"could not create junction: {done.stdout}{done.stderr}")
        self.assert_violation("reparse point")

    def test_directory_symlink_is_rejected(self) -> None:
        target = self._make_outside_tree()
        link = self.repo / "ThirdParty" / "Linked"
        try:
            os.symlink(str(target), str(link), target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            if IN_CI:
                self.fail(f"symlink creation must work in CI: {exc}")
            self.skipTest(f"symlink creation unavailable: {exc}")
        self.assert_violation("symbolic link rejected")

    def test_file_symlink_over_a_sentinel_is_rejected(self) -> None:
        outside = Path(self._tmp.name) / "outside.h"
        outside.write_text("/* outside */\n", encoding="utf-8")
        sentinel = self.repo / "ThirdParty/Utils/demo/demo.h"
        sentinel.unlink()
        try:
            os.symlink(str(outside), str(sentinel))
        except (OSError, NotImplementedError) as exc:
            if IN_CI:
                self.fail(f"symlink creation must work in CI: {exc}")
            self.skipTest(f"symlink creation unavailable: {exc}")
        self.assert_violation("symbolic link rejected")

    def test_link_reason_reports_missing_paths_instead_of_swallowing(self) -> None:
        reason = sc.link_reason(self.repo / "ThirdParty" / "does-not-exist")
        self.assertIsNotNone(reason)
        self.assertIn("cannot lstat", reason)

    def test_link_reason_accepts_a_plain_file(self) -> None:
        self.assertIsNone(sc.link_reason(self.repo / "ThirdParty/Utils/demo/demo.h"))


class TestContainment(unittest.TestCase):
    """assert_regular_file_no_escape must separate its failure modes."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="spark-sc-contain-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name) / "root"
        self.root.mkdir()
        self.root_resolved = self.root.resolve(strict=True)

    def test_plain_file_inside_root_is_accepted(self) -> None:
        target = self.root / "ok.txt"
        target.write_text("hello", encoding="utf-8")
        self.assertIsNone(
            sc.assert_regular_file_no_escape(target, self.root_resolved)
        )

    def test_missing_file_is_reported_as_a_stat_failure(self) -> None:
        reason = sc.assert_regular_file_no_escape(
            self.root / "missing.txt", self.root_resolved
        )
        self.assertIn("cannot lstat", reason)

    def test_directory_is_not_a_regular_file(self) -> None:
        (self.root / "dir").mkdir()
        reason = sc.assert_regular_file_no_escape(
            self.root / "dir", self.root_resolved
        )
        self.assertEqual(reason, "not a regular file")

    def test_hardlink_is_rejected(self) -> None:
        outside = Path(self._tmp.name) / "outside.txt"
        outside.write_text("secret", encoding="utf-8")
        inside = self.root / "hard.txt"
        try:
            os.link(str(outside), str(inside))
        except (OSError, NotImplementedError, AttributeError) as exc:
            self.skipTest(f"hardlink creation unavailable: {exc}")
        reason = sc.assert_regular_file_no_escape(inside, self.root_resolved)
        self.assertIsNotNone(reason, "a hardlinked file is not contained by the root")
        self.assertIn("hardlink", reason)

    def test_symlink_escaping_root_is_rejected(self) -> None:
        outside = Path(self._tmp.name) / "outside.txt"
        outside.write_text("secret", encoding="utf-8")
        inside = self.root / "link.txt"
        try:
            os.symlink(str(outside), str(inside))
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"symlink creation unavailable: {exc}")
        reason = sc.assert_regular_file_no_escape(inside, self.root_resolved)
        self.assertEqual(reason, "symbolic link rejected")


# ═══════════════════════════════════════════════════════════════════════
# GitHub Actions pinning — structural, not line-regex
# ═══════════════════════════════════════════════════════════════════════

class TestActionPinning(FakeRepoCase):

    def _workflow(self, body: str) -> None:
        self.write(".github/workflows/ci.yml", body)
        self.commit()

    HEADER = "name: fixture\non: [push]\njobs:\n  build:\n    runs-on: ubuntu-24.04\n    steps:\n"

    def test_unpinned_tag_is_rejected(self) -> None:
        self._workflow(self.HEADER + "      - uses: actions/checkout@v4\n")
        self.assert_violation("unpinned action")

    def test_quoted_uses_key_is_rejected(self) -> None:
        # A line regex looking for `uses:` never fires on a quoted key.
        self._workflow(self.HEADER + '      - "uses": actions/checkout@v4\n')
        self.assert_violation("unpinned action")

    def test_quoted_uses_key_with_valid_sha_is_still_identity_checked(self) -> None:
        self._workflow(
            self.HEADER + f'      - "uses": attacker/backdoor@{"a" * 40}\n'
        )
        self.assert_violation("action_pins")

    def test_value_on_continuation_line_is_rejected(self) -> None:
        self._workflow(self.HEADER + "      - uses:\n          actions/checkout@v4\n")
        self.assert_violation("unpinned action")

    def test_space_before_colon_is_rejected(self) -> None:
        self._workflow(self.HEADER + "      - uses : actions/checkout@v4\n")
        self.assert_violation("unpinned action")

    def test_flow_mapping_step_is_rejected(self) -> None:
        self._workflow(self.HEADER + "      - {uses: actions/checkout@v4}\n")
        self.assert_violation("unpinned action")

    def test_explicit_key_syntax_is_rejected(self) -> None:
        self._workflow(self.HEADER + "      - ? uses\n        : actions/checkout@v4\n")
        self.assert_violation("unpinned action")

    def test_anchor_alias_indirection_is_rejected(self) -> None:
        self._workflow(
            "name: fixture\non: [push]\nenv:\n  PIN: &p actions/checkout@v4\n"
            "jobs:\n  build:\n    runs-on: ubuntu-24.04\n    steps:\n"
            "      - {uses: *p}\n"
        )
        self.assert_violation("unpinned action")

    def test_merge_key_indirection_is_rejected(self) -> None:
        self._workflow(
            "name: fixture\non: [push]\n"
            "x-base: &base\n  uses: actions/checkout@v4\n"
            "jobs:\n  build:\n    runs-on: ubuntu-24.04\n    steps:\n"
            "      - <<: *base\n        name: innocuous\n"
        )
        self.assert_violation("unpinned action")

    def test_block_scalar_behind_quoted_key_is_rejected(self) -> None:
        self._workflow(self.HEADER + '      - "uses": >-\n          actions/checkout@v4\n')
        self.assert_violation("unpinned action")

    def test_job_level_reusable_workflow_must_be_pinned(self) -> None:
        self._workflow(
            "name: fixture\non: [push]\njobs:\n  call:\n"
            '    "uses": attacker/evil/.github/workflows/w.yml@main\n'
        )
        self.assert_violation("unpinned action")

    def test_runtime_expression_cannot_be_pinned(self) -> None:
        self._workflow(
            self.HEADER + "      - uses: ${{ env.ACTION_REF }}\n"
        )
        self.assert_violation("computed at runtime")

    def test_uppercase_sha_is_rejected(self) -> None:
        self._workflow(self.HEADER + f"      - uses: actions/checkout@{'A' * 40}\n")
        self.assert_violation("unpinned action")

    def test_unknown_owner_with_valid_sha_is_rejected(self) -> None:
        self._workflow(self.HEADER + f"      - uses: attacker/backdoor@{'b' * 40}\n")
        self.assert_violation("not in supply-chain.lock action_pins")

    def test_known_owner_with_unrecorded_sha_is_rejected(self) -> None:
        self._workflow(self.HEADER + f"      - uses: actions/checkout@{'c' * 40}\n")
        self.assert_violation("not a recorded pin")

    def test_docker_tag_reference_is_rejected(self) -> None:
        self._workflow(self.HEADER + "      - uses: docker://alpine:latest\n")
        self.assert_violation("digest-pinned")

    def test_docker_digest_reference_is_accepted(self) -> None:
        self._workflow(
            self.HEADER + f"      - uses: docker://alpine@sha256:{'d' * 64}\n"
        )
        self.assert_baseline_passes()

    def test_local_action_without_definition_is_rejected(self) -> None:
        self._workflow(self.HEADER + "      - uses: ./.github/actions/missing\n")
        self.assert_violation("no action.yml")

    def test_local_action_path_escape_is_rejected(self) -> None:
        self._workflow(self.HEADER + "      - uses: ./../evil\n")
        self.assert_violation("local action path")

    def test_composite_action_definition_is_scanned(self) -> None:
        self.write(
            ".github/actions/setup/action.yml",
            "name: setup\nruns:\n  using: composite\n  steps:\n"
            "    - uses: attacker/backdoor@v9\n",
        )
        self.write(self.HEADER and ".github/workflows/ci.yml",
                   self.HEADER + "      - uses: ./.github/actions/setup\n")
        self.commit()
        self.assert_violation("unpinned action", "action.yml")

    def test_workflow_in_subdirectory_is_scanned(self) -> None:
        self.write(
            ".github/workflows/sub/nested.yml",
            self.HEADER + "      - uses: actions/checkout@v4\n",
        )
        self.commit()
        self.assert_violation("unpinned action", "nested.yml")

    def test_uses_inside_a_run_script_is_not_a_false_positive(self) -> None:
        self._workflow(
            self.HEADER
            + f"      - uses: actions/checkout@{CHECKOUT_SHA}\n"
            + "      - run: |\n          echo 'uses: actions/checkout@v4'\n"
        )
        self.assert_baseline_passes()

    def test_quoted_valid_pin_is_not_a_false_positive(self) -> None:
        self._workflow(
            self.HEADER + f'      - uses: "actions/checkout@{CHECKOUT_SHA}"\n'
        )
        self.assert_baseline_passes()

    def test_flow_mapping_with_valid_pin_is_not_a_false_positive(self) -> None:
        self._workflow(
            self.HEADER + f"      - {{uses: actions/checkout@{CHECKOUT_SHA}}}\n"
        )
        self.assert_baseline_passes()

    def test_undecodable_workflow_fails_rather_than_crashing(self) -> None:
        (self.repo / ".github/workflows/ci.yml").write_bytes(
            b"\xff\xfe" + "name: x\n".encode("utf-16-le")
        )
        self.commit()
        self.assert_violation("cannot decode workflow file")

    def test_unparseable_workflow_yaml_is_a_violation(self) -> None:
        self._workflow("name: fixture\n  bad: [unclosed\n")
        self.assert_violation("cannot parse workflow YAML")

    def test_missing_workflows_directory_is_an_error_not_a_warning(self) -> None:
        shutil.rmtree(self.repo / ".github/workflows")
        self.commit()
        self.assert_violation("workflows directory not found")


class TestUsesTraversal(unittest.TestCase):
    """_iter_uses must find keys and ignore look-alike values."""

    def test_finds_nested_uses_keys(self) -> None:
        document = {
            "jobs": {
                "a": {"steps": [{"uses": "x/y@1"}, {"run": "uses: fake/one@2"}]},
                "b": {"uses": "p/q@3"},
            }
        }
        found = sorted(value for _, value in sc._iter_uses(document))
        self.assertEqual(found, ["p/q@3", "x/y@1"])

    def test_ignores_uses_text_inside_scalar_values(self) -> None:
        document = {"jobs": {"a": {"steps": [{"run": "uses: evil/act@v1"}]}}}
        self.assertEqual(list(sc._iter_uses(document)), [])


# ═══════════════════════════════════════════════════════════════════════
# Reconciliation across supply-chain.lock, dependencies.lock, .gitmodules
# ═══════════════════════════════════════════════════════════════════════

class TestReconciliation(FakeRepoCase):

    def _set_manifest(self, entry: str) -> None:
        self.write(
            "ThirdParty/dependencies.lock",
            "set(SPARK_THIRDPARTY_AUDIT_ENTRIES\n"
            f'    "{entry}"\n'
            ")\n",
        )
        self.commit()

    BASE_FIELDS = [
        "demo", "https://github.com/example/demo", "v1.2.3 (vendored snapshot)",
        "MIT", "ThirdParty/Utils/demo", "demo.h,demo_impl.cpp", "SPARK_HAS_DEMO",
        "SparkEngine/Source/DemoStub.cpp", "WARN", "ThirdParty/Utils/demo/LICENSE",
    ]

    def entry(self, **overrides: str) -> str:
        fields = list(self.BASE_FIELDS)
        names = [
            "name", "source", "version", "license", "local_path",
            "required", "macro", "fallback", "severity", "notices",
        ]
        for key, value in overrides.items():
            fields[names.index(key)] = value
        return "|".join(fields)

    def test_managed_container_without_a_manifest_entry_is_detected(self) -> None:
        data = self.lock()
        data["managed_vendored_dirs"].append("ThirdParty/Extra")
        self.set_lock(data)
        self.write("ThirdParty/Extra/thing.h", "/* payload */\n")
        self.commit()
        done = self.check("--json")
        self.assertEqual(done.returncode, 1)
        self.assertIn("has no dependencies.lock entry", done.stdout)

    def test_dependency_outside_thirdparty_is_an_error_not_a_skip(self) -> None:
        self._set_manifest(self.entry(local_path="vendor/evil"))
        self.assert_violation("must live under ThirdParty/")

    def test_manifest_path_with_dot_dot_is_rejected(self) -> None:
        self._set_manifest(self.entry(local_path="ThirdParty/Utils/demo/../../etc"))
        self.assert_violation("dot-segment")

    def test_manifest_prefix_subpath_is_not_accepted_as_exact(self) -> None:
        self._set_manifest(self.entry(local_path="ThirdParty/Utils/demo/nested"))
        self.assert_violation("not an exact container")

    def test_duplicate_dependency_name_is_detected(self) -> None:
        self.write(
            "ThirdParty/dependencies.lock",
            "set(SPARK_THIRDPARTY_AUDIT_ENTRIES\n"
            f'    "{self.entry()}"\n'
            f'    "{self.entry(local_path="ThirdParty/Local")}"\n'
            ")\n",
        )
        self.commit()
        self.assert_violation("duplicate dependency name")

    def test_duplicate_local_path_is_detected(self) -> None:
        self.write(
            "ThirdParty/dependencies.lock",
            "set(SPARK_THIRDPARTY_AUDIT_ENTRIES\n"
            f'    "{self.entry()}"\n'
            f'    "{self.entry(name="demo2")}"\n'
            ")\n",
        )
        self.commit()
        self.assert_violation("shares its path with")

    def test_case_variant_manifest_alias_is_detected(self) -> None:
        self.write(
            "ThirdParty/dependencies.lock",
            "set(SPARK_THIRDPARTY_AUDIT_ENTRIES\n"
            f'    "{self.entry()}"\n'
            f'    "{self.entry(name="demo2", local_path="ThirdParty/Utils/DEMO")}"\n'
            ")\n",
        )
        self.commit()
        self.assert_violation("case-variant alias")

    def test_manifest_entry_appended_after_the_closing_paren_is_seen(self) -> None:
        # A text scrape stops at the first `)`; CMake evaluates the whole file.
        self.write(
            "ThirdParty/dependencies.lock",
            "set(SPARK_THIRDPARTY_AUDIT_ENTRIES\n"
            f'    "{self.entry()}"\n'
            ")\n"
            "list(APPEND SPARK_THIRDPARTY_AUDIT_ENTRIES\n"
            f'    "{self.entry(name="smuggled", local_path="ThirdParty/Smuggled")}"\n'
            ")\n",
        )
        self.commit()
        self.assert_violation("not an exact container")

    def test_manifest_entry_built_from_a_variable_is_seen(self) -> None:
        self.write(
            "ThirdParty/dependencies.lock",
            f'set(_hidden "{self.entry(name="hidden", local_path="ThirdParty/Hidden")}")\n'
            "set(SPARK_THIRDPARTY_AUDIT_ENTRIES\n"
            f'    "{self.entry()}"\n'
            '    "${_hidden}"\n'
            ")\n",
        )
        self.commit()
        self.assert_violation("not an exact container")

    def test_notice_not_tracked_as_a_sentinel_is_detected(self) -> None:
        self.write("ThirdParty/Utils/demo/EXTRA-LICENSE", LICENSE_TEXT)
        self._set_manifest(
            self.entry(notices="ThirdParty/Utils/demo/EXTRA-LICENSE")
        )
        done = self.check("--json")
        self.assertEqual(done.returncode, 1)
        self.assertIn("not a sentinel", done.stdout)

    def test_notice_typed_as_source_is_detected(self) -> None:
        data = self.lock()
        data["sentinel_files"]["ThirdParty/Utils/demo/LICENSE"]["type"] = "source"
        self.set_lock(data)
        self.assert_violation("only 'license' sentinels get content validation")

    def test_missing_required_file_is_detected(self) -> None:
        self._set_manifest(self.entry(required="demo.h,absent.h"))
        self.assert_violation("required file does not exist")

    def test_invalid_severity_is_rejected_by_cmake(self) -> None:
        self._set_manifest(self.entry(severity="INFO"))
        self.assert_fatal("Invalid severity")

    def test_non_https_source_is_detected(self) -> None:
        self._set_manifest(self.entry(source="git://example.invalid/demo"))
        self.assert_violation("must be an https:// URL")

    def test_empty_license_field_is_detected(self) -> None:
        self._set_manifest(self.entry(license=""))
        self.assert_violation("has no license")

    def test_nonexistent_fallback_path_is_detected(self) -> None:
        self._set_manifest(
            self.entry(fallback="SparkEngine/Source/DoesNotExist.cpp")
        )
        self.assert_violation("fallback path that does not exist")

    def test_descriptive_fallback_prose_is_not_a_false_positive(self) -> None:
        self._set_manifest(
            self.entry(fallback="NullRHI/headless fallback when unavailable")
        )
        self.assert_baseline_passes()

    def test_project_owned_dir_claimed_as_a_dependency_is_detected(self) -> None:
        self.write("ThirdParty/Local/LICENSE", LICENSE_TEXT)
        self._set_manifest(
            self.entry(
                local_path="ThirdParty/Local",
                required="notes.h",
                notices="ThirdParty/Local/LICENSE",
            )
        )
        self.assert_violation("cannot also be a third-party dependency")


class TestGitmodulesReconciliation(FakeRepoCase):

    def test_submodule_in_gitmodules_but_not_lock_is_detected(self) -> None:
        self.write(
            ".gitmodules",
            '[submodule "ThirdParty/New/dep"]\n'
            "\tpath = ThirdParty/New/dep\n"
            "\turl = https://example.invalid/dep.git\n",
        )
        self.commit()
        self.assert_violation("not in supply-chain.lock")

    def test_submodule_in_lock_but_not_gitmodules_is_detected(self) -> None:
        data = self.lock()
        data["submodule_gitlinks"]["ThirdParty/Ghost"] = "e" * 40
        self.set_lock(data)
        self.assert_violation("not in .gitmodules")

    def test_non_https_submodule_url_is_detected(self) -> None:
        self.write(
            ".gitmodules",
            '[submodule "ThirdParty/New/dep"]\n'
            "\tpath = ThirdParty/New/dep\n"
            "\turl = git://example.invalid/dep.git\n",
        )
        self.commit()
        self.assert_violation("url must be https://")

    def test_submodule_outside_thirdparty_is_detected(self) -> None:
        self.write(
            ".gitmodules",
            '[submodule "vendor/dep"]\n'
            "\tpath = vendor/dep\n"
            "\turl = https://example.invalid/dep.git\n",
        )
        self.commit()
        self.assert_violation("not in allowed roots")

    def test_submodule_without_a_path_is_detected(self) -> None:
        self.write(
            ".gitmodules",
            '[submodule "nameless"]\n\turl = https://example.invalid/dep.git\n',
        )
        self.commit()
        self.assert_violation("declares no path")


# ═══════════════════════════════════════════════════════════════════════
# Sentinel integrity
# ═══════════════════════════════════════════════════════════════════════

class TestSentinelIntegrity(FakeRepoCase):

    def test_content_hash_drift_is_detected(self) -> None:
        data = self.lock()
        data["sentinel_files"]["ThirdParty/Utils/demo/demo.h"]["sha256"] = "f" * 64
        self.set_lock(data)
        self.assert_violation("content hash mismatch")

    def test_size_drift_is_detected(self) -> None:
        data = self.lock()
        data["sentinel_files"]["ThirdParty/Utils/demo/demo.h"]["size"] += 1
        self.set_lock(data)
        self.assert_violation("size mismatch")

    def test_git_blob_drift_is_detected(self) -> None:
        data = self.lock()
        data["sentinel_files"]["ThirdParty/Utils/demo/demo.h"]["git_blob"] = "1" * 40
        self.set_lock(data)
        self.assert_violation("git blob drift")

    def test_missing_sentinel_file_is_detected(self) -> None:
        (self.repo / "ThirdParty/Utils/demo/demo.h").unlink()
        self.assert_violation("cannot lstat")

    def test_sentinel_outside_every_container_is_detected(self) -> None:
        data = self.lock()
        data["sentinel_files"]["ThirdParty/Nowhere/x.h"] = {
            "sha256": "0" * 64, "git_blob": "0" * 40, "size": 1, "type": "source",
        }
        self.set_lock(data)
        self.assert_violation("in no declared container")

    def test_empty_sentinel_set_fails_closed(self) -> None:
        data = self.lock()
        data["sentinel_files"] = {}
        self.set_lock(data)
        self.assert_violation("no sentinel files in lockfile")

    def test_license_without_copyright_is_rejected(self) -> None:
        # A notice named by the manifest is validated by the CMake audit before
        # the Python checks run, so this surfaces as a checker-level failure.
        self.write(
            "ThirdParty/Utils/demo/LICENSE",
            "Permission is hereby granted to do anything at all, forever, "
            "without restriction of any kind whatsoever, to any person or "
            "entity obtaining a copy of this software package, including "
            "the rights to use, copy, modify, merge, publish, distribute, "
            "sublicense, and sell copies of it without attribution.\n",
        )
        self.assert_fatal("lacks a copyright statement")

    def test_short_license_is_rejected(self) -> None:
        self.write("ThirdParty/Utils/demo/LICENSE", "Copyright (c) 2026. MIT.\n")
        self.assert_fatal("implausibly short")


class TestLicenseContent(unittest.TestCase):
    """_check_license_content guards license sentinels that are not notices."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="spark-sc-license-")
        self.addCleanup(self._tmp.cleanup)
        self.path = Path(self._tmp.name) / "LICENSE"

    def _messages(self, text: str) -> str:
        self.path.write_text(text, encoding="utf-8", newline="\n")
        result = sc.CheckResult()
        sc._check_license_content(self.path, "ThirdParty/x/LICENSE", result)
        return " | ".join(v.message for v in result.violations)

    def test_valid_license_passes(self) -> None:
        self.assertEqual(self._messages(LICENSE_TEXT), "")

    def test_short_license_flagged(self) -> None:
        self.assertIn("implausibly short", self._messages("Copyright 2026. MIT.\n"))

    def test_missing_copyright_flagged(self) -> None:
        text = ("Permission is hereby granted to use this software freely "
                "and without restriction of any kind. " * 4)
        self.assertIn("lacks copyright statement", self._messages(text))

    def test_missing_operative_terms_flagged(self) -> None:
        text = "Copyright (c) 2026 Example. " + ("All rights reserved. " * 20)
        self.assertIn("lacks operative terms", self._messages(text))


# ═══════════════════════════════════════════════════════════════════════
# --update must produce a complete, verified lockfile
# ═══════════════════════════════════════════════════════════════════════

class TestUpdateMode(FakeRepoCase):

    def test_update_verifies_what_it_wrote(self) -> None:
        # Break the tree so the freshly written lockfile cannot verify; the
        # exit code must be the verification's, not an unconditional 0.
        self.write("ThirdParty/Rogue/payload.h", "/* payload */\n")
        self.commit()
        done = self.check("--update")
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn("no declared container", done.stdout)

    def test_update_refreshes_tree_digests(self) -> None:
        self.write("ThirdParty/Utils/demo/nested/deep/extra.h", "/* changed */\n")
        self.commit()
        before = self.lock()["tree_digests"]["ThirdParty/Utils/demo"]["digest"]
        done = self.check("--update")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        after = self.lock()["tree_digests"]["ThirdParty/Utils/demo"]["digest"]
        self.assertNotEqual(before, after)

    def test_update_discovers_new_required_files_as_sentinels(self) -> None:
        self.write("ThirdParty/Utils/demo/demo_extra.h", "/* new entry point */\n")
        self.write(
            "ThirdParty/dependencies.lock",
            MANIFEST_TEXT.replace(
                "demo.h,demo_impl.cpp", "demo.h,demo_impl.cpp,demo_extra.h"
            ),
        )
        self.commit()
        done = self.check("--update")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn(
            "ThirdParty/Utils/demo/demo_extra.h", self.lock()["sentinel_files"]
        )

    def test_update_refuses_a_missing_lockfile(self) -> None:
        (self.repo / sc.LOCKFILE_REL).unlink()
        done = self.check("--update")
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        self.assertIn("cannot invent the container declarations", done.stderr)

    def test_update_rejects_an_unjustified_project_owned_dir(self) -> None:
        data = self.lock()
        data["project_owned_dirs"]["ThirdParty/Laundered"] = "x"
        self.set_lock(data)
        done = self.check("--update")
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        self.assertIn("recorded justification", done.stderr)

    def test_update_cannot_launder_an_unpopulated_container(self) -> None:
        data = self.lock()
        data["project_owned_dirs"]["ThirdParty/Laundered"] = (
            "an unjustifiable pre-authorisation of a path that does not exist"
        )
        self.set_lock(data)
        done = self.check("--update", "--json")
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn("holds no tracked files", done.stdout)

    def test_update_rejects_a_version_one_lockfile(self) -> None:
        data = self.lock()
        data["version"] = 1
        self.set_lock(data)
        done = self.check("--update")
        self.assertEqual(done.returncode, 2)
        self.assertIn("version", done.stderr)

    def test_update_json_reports_what_it_wrote(self) -> None:
        done = self.check("--update", "--json")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        payload = json.loads(done.stdout)
        self.assertEqual(payload["updated"], sc.LOCKFILE_REL)
        self.assertGreater(payload["sentinels_written"], 0)
        self.assertTrue(payload["passed"])

    def test_update_writes_a_readable_lockfile(self) -> None:
        self.check("--update")
        mode = stat.S_IMODE((self.repo / sc.LOCKFILE_REL).stat().st_mode)
        self.assertTrue(mode & stat.S_IRUSR)
        if os.name == "posix":
            self.assertTrue(mode & stat.S_IRGRP, "regenerated lockfile lost group read")


# ═══════════════════════════════════════════════════════════════════════
# Schema, bounds, and fail-closed behaviour
# ═══════════════════════════════════════════════════════════════════════

class TestSchemaValidation(unittest.TestCase):

    @staticmethod
    def _lock(**overrides) -> dict:
        base = {
            "version": 2,
            "description": "test",
            "submodule_gitlinks": {},
            "managed_vendored_dirs": [],
            "project_owned_dirs": {},
            "allowed_root_files": [],
            "tree_digests": {},
            "sentinel_files": {},
            "action_pins": {},
        }
        base.update(overrides)
        return base

    def _expect_fatal(self, data: dict) -> None:
        with self.assertRaises(SystemExit) as ctx:
            sc.validate_lockfile_schema(data)
        self.assertEqual(ctx.exception.code, 2)

    def test_minimal_valid_schema_is_accepted(self) -> None:
        sc.validate_lockfile_schema(self._lock())

    def test_truncated_sentinel_sha256_rejected(self) -> None:
        self._expect_fatal(self._lock(sentinel_files={
            "ThirdParty/x": {"sha256": "ab", "git_blob": "0" * 40,
                             "size": 1, "type": "source"}}))

    def test_missing_git_blob_rejected(self) -> None:
        self._expect_fatal(self._lock(sentinel_files={
            "ThirdParty/x": {"sha256": "0" * 64, "size": 1, "type": "source"}}))

    def test_negative_sentinel_size_rejected(self) -> None:
        self._expect_fatal(self._lock(sentinel_files={
            "ThirdParty/x": {"sha256": "0" * 64, "git_blob": "0" * 40,
                             "size": -1, "type": "source"}}))

    def test_boolean_size_rejected(self) -> None:
        self._expect_fatal(self._lock(sentinel_files={
            "ThirdParty/x": {"sha256": "0" * 64, "git_blob": "0" * 40,
                             "size": True, "type": "source"}}))

    def test_invalid_sentinel_type_rejected(self) -> None:
        self._expect_fatal(self._lock(sentinel_files={
            "ThirdParty/x": {"sha256": "0" * 64, "git_blob": "0" * 40,
                             "size": 1, "type": "binary"}}))

    def test_sentinel_outside_thirdparty_rejected(self) -> None:
        self._expect_fatal(self._lock(sentinel_files={
            "SparkEngine/x": {"sha256": "0" * 64, "git_blob": "0" * 40,
                              "size": 1, "type": "source"}}))

    def test_invalid_gitlink_sha_rejected(self) -> None:
        self._expect_fatal(self._lock(submodule_gitlinks={"ThirdParty/x": "nope"}))

    def test_uppercase_gitlink_sha_rejected(self) -> None:
        self._expect_fatal(self._lock(submodule_gitlinks={"ThirdParty/x": "A" * 40}))

    def test_duplicate_managed_dirs_rejected(self) -> None:
        self._expect_fatal(self._lock(
            managed_vendored_dirs=["ThirdParty/a", "ThirdParty/a"]))

    def test_invalid_tree_digest_rejected(self) -> None:
        self._expect_fatal(self._lock(
            tree_digests={"ThirdParty/a": {"digest": "z" * 64, "file_count": 0}}))

    def test_negative_file_count_rejected(self) -> None:
        self._expect_fatal(self._lock(
            tree_digests={"ThirdParty/a": {"digest": "0" * 64, "file_count": -1}}))

    def test_allowed_root_file_must_be_at_the_root(self) -> None:
        self._expect_fatal(self._lock(
            allowed_root_files=["ThirdParty/Utils/deep.md"]))

    def test_action_pin_key_must_be_owner_repo(self) -> None:
        self._expect_fatal(self._lock(action_pins={"not-a-slug": ["0" * 40]}))

    def test_action_pin_sha_must_be_lowercase_hex(self) -> None:
        self._expect_fatal(self._lock(action_pins={"a/b": ["Z" * 40]}))

    def test_empty_action_pin_list_rejected(self) -> None:
        self._expect_fatal(self._lock(action_pins={"a/b": []}))

    def test_project_owned_must_be_a_mapping(self) -> None:
        self._expect_fatal(self._lock(project_owned_dirs=["ThirdParty/a"]))

    def test_missing_field_rejected(self) -> None:
        data = self._lock()
        del data["tree_digests"]
        self._expect_fatal(data)


class TestPathValidation(unittest.TestCase):

    def test_valid_path_accepted(self) -> None:
        self.assertIsNone(sc.validate_repo_relative_path(
            "ThirdParty/Utils/stb/stb_image.h",
            allowed_roots=sc.AUTHORITATIVE_ROOTS))

    def test_absolute_rejected(self) -> None:
        self.assertIsNotNone(sc.validate_repo_relative_path("/etc/passwd"))

    def test_parent_traversal_rejected(self) -> None:
        self.assertIsNotNone(
            sc.validate_repo_relative_path("ThirdParty/../../etc/passwd"))

    def test_current_dir_segment_rejected(self) -> None:
        self.assertIsNotNone(sc.validate_repo_relative_path("ThirdParty/./x"))

    def test_backslash_rejected(self) -> None:
        self.assertIsNotNone(sc.validate_repo_relative_path("ThirdParty\\x"))

    def test_trailing_slash_rejected(self) -> None:
        self.assertIsNotNone(sc.validate_repo_relative_path("ThirdParty/x/"))

    def test_empty_segment_rejected(self) -> None:
        self.assertIsNotNone(sc.validate_repo_relative_path("ThirdParty//x"))

    def test_null_byte_rejected(self) -> None:
        self.assertIsNotNone(sc.validate_repo_relative_path("ThirdParty/x\x00y"))

    def test_newline_rejected(self) -> None:
        self.assertIsNotNone(sc.validate_repo_relative_path("ThirdParty/x\ny"))

    def test_empty_rejected(self) -> None:
        self.assertIsNotNone(sc.validate_repo_relative_path(""))

    def test_wrong_root_rejected(self) -> None:
        self.assertIsNotNone(sc.validate_repo_relative_path(
            "SparkEngine/x", allowed_roots=sc.AUTHORITATIVE_ROOTS))


class TestBounds(FakeRepoCase):
    """Every input the checker reads must be bounded before it is read."""

    def test_bounds_constants_are_declared(self) -> None:
        for name in (
            "MAX_LOCKFILE_BYTES", "MAX_MANIFEST_BYTES", "MAX_WORKFLOW_BYTES",
            "MAX_SENTINEL_BYTES", "MAX_LICENSE_BYTES", "MAX_JSON_DEPTH",
            "MAX_SENTINELS", "MAX_CONTAINERS", "MAX_SUBMODULES",
            "MAX_MANIFEST_ENTRIES", "MAX_TRACKED_PATHS", "MAX_WALK_ENTRIES",
            "MAX_WALK_DEPTH", "MAX_WORKFLOW_FILES", "MAX_VIOLATIONS",
        ):
            self.assertIsInstance(getattr(sc, name), int, name)
            self.assertGreater(getattr(sc, name), 0, name)

    def test_oversized_lockfile_is_rejected(self) -> None:
        original = (self.repo / sc.LOCKFILE_REL).read_text(encoding="utf-8")
        padding = " " * (sc.MAX_LOCKFILE_BYTES + 1)
        (self.repo / sc.LOCKFILE_REL).write_text(
            original.rstrip() + "\n" + padding, encoding="utf-8", newline="\n"
        )
        self.assert_fatal("exceeding the")

    def test_deeply_nested_lockfile_json_is_rejected(self) -> None:
        depth = sc.MAX_JSON_DEPTH + 5
        payload = "[" * depth + "]" * depth
        self.write(sc.LOCKFILE_REL, '{"version": 2, "nest": ' + payload + "}\n")
        self.assert_fatal("")

    def test_undecodable_lockfile_is_rejected(self) -> None:
        (self.repo / sc.LOCKFILE_REL).write_bytes(b"\xff\xfe\x00\x01 not utf-8")
        self.assert_fatal("not valid UTF-8")

    def test_duplicate_json_keys_are_rejected(self) -> None:
        self.write(
            sc.LOCKFILE_REL,
            '{"version": 2, "sentinel_files": {}, "sentinel_files": {}}\n',
        )
        self.assert_fatal("duplicate JSON key")

    def test_license_size_ceiling_is_below_the_sentinel_ceiling(self) -> None:
        self.assertLess(sc.MAX_LICENSE_BYTES, sc.MAX_SENTINEL_BYTES)

    def test_violation_list_is_capped(self) -> None:
        result = sc.CheckResult()
        for index in range(sc.MAX_VIOLATIONS + 50):
            result.error("test", f"p{index}", "boom")
        self.assertLessEqual(len(result.violations), sc.MAX_VIOLATIONS + 1)
        self.assertTrue(result.truncated)
        self.assertFalse(result.passed)


class TestFailClosed(FakeRepoCase):

    def test_missing_lockfile_exits_two(self) -> None:
        (self.repo / sc.LOCKFILE_REL).unlink()
        self.assert_fatal("cannot lstat")

    def test_corrupt_lockfile_exits_two(self) -> None:
        self.write(sc.LOCKFILE_REL, "{ not json\n")
        self.assert_fatal("cannot parse lockfile")

    def test_wrong_lockfile_version_exits_two(self) -> None:
        data = self.lock()
        data["version"] = 99
        self.set_lock(data)
        self.assert_fatal("unsupported lockfile version")

    def test_missing_manifest_exits_two(self) -> None:
        (self.repo / "ThirdParty/dependencies.lock").unlink()
        self.assert_fatal("")

    def test_missing_gitmodules_exits_two(self) -> None:
        (self.repo / ".gitmodules").unlink()
        self.assert_fatal("cannot lstat")

    def test_not_in_a_git_repo_exits_two(self) -> None:
        outside = Path(self._tmp.name) / "not-a-repo"
        (outside / "tools").mkdir(parents=True)
        shutil.copy2(PROJECT_ROOT / CHECKER_REL, outside / CHECKER_REL)
        done = subprocess.run(
            [sys.executable, CHECKER_REL],
            cwd=str(outside), capture_output=True, text=True, timeout=120,
        )
        self.assertIn(done.returncode, (2,), done.stdout + done.stderr)

    def test_git_command_failure_is_fatal_not_silent(self) -> None:
        with self.assertRaises(RuntimeError):
            sc.git_cmd(["cat-file", "-e", "0" * 40], self.repo)


# ═══════════════════════════════════════════════════════════════════════
# Real-repository assertions
# ═══════════════════════════════════════════════════════════════════════

class TestRealRepository(unittest.TestCase):
    """Read-only assertions about this repository's own lockfile."""

    @classmethod
    def setUpClass(cls) -> None:
        _require("git")
        cls.lock = json.loads(
            (PROJECT_ROOT / sc.LOCKFILE_REL).read_text(encoding="utf-8")
        )

    def test_lockfile_is_current_schema_version(self) -> None:
        self.assertEqual(self.lock["version"], sc.LOCKFILE_VERSION)

    def test_every_non_submodule_container_has_a_tree_digest(self) -> None:
        containers = set(self.lock["managed_vendored_dirs"]) | set(
            self.lock["project_owned_dirs"]
        )
        self.assertEqual(containers - set(self.lock["tree_digests"]), set())

    def test_every_tracked_thirdparty_path_is_accounted_for(self) -> None:
        """The coverage claim is a total, not a floor."""
        tracked = sc.git_tracked_thirdparty(PROJECT_ROOT)
        containers = set(self.lock["managed_vendored_dirs"]) | set(
            self.lock["project_owned_dirs"]
        ) | set(self.lock["submodule_gitlinks"])
        allowed = set(self.lock["allowed_root_files"])
        unaccounted = [
            entry.path for entry in tracked
            if entry.path not in allowed
            and not any(
                entry.path == c or entry.path.startswith(c + "/") for c in containers
            )
        ]
        self.assertEqual(unaccounted, [], "unaccounted tracked ThirdParty paths")

    def test_digest_file_counts_sum_to_the_tracked_payload(self) -> None:
        tracked = sc.git_tracked_thirdparty(PROJECT_ROOT)
        payload = [e for e in tracked if e.mode in sc.GIT_FILE_MODES]
        allowed = set(self.lock["allowed_root_files"])
        expected = len([e for e in payload if e.path not in allowed])
        counted = sum(v["file_count"] for v in self.lock["tree_digests"].values())
        self.assertEqual(counted, expected)

    def test_gitlink_count_matches_gitmodules(self) -> None:
        modules = sc._parse_gitmodules(PROJECT_ROOT, PROJECT_ROOT.resolve())
        paths = {f["path"] for f in modules.values() if f.get("path")}
        self.assertEqual(paths, set(self.lock["submodule_gitlinks"]))

    def test_every_action_pin_sha_is_lowercase_forty_hex(self) -> None:
        for repo, shas in self.lock["action_pins"].items():
            for sha in shas:
                self.assertRegex(sha, r"^[0-9a-f]{40}$", repo)

    def test_git_blob_identity_is_platform_stable(self) -> None:
        """Catches a .gitattributes or autocrlf change that shifts blob ids."""
        paths = sorted(self.lock["sentinel_files"])
        actual = sc.git_blob_hashes(paths, PROJECT_ROOT)
        for path in paths:
            self.assertEqual(
                actual[path], self.lock["sentinel_files"][path]["git_blob"], path
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
