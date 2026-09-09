#!/usr/bin/env python3
"""Adversarial tests for the RDY-010 module evidence validator.

Every case identified `B01`..`B36` is a bypass shape that an independent
hostile probe demonstrated the previous validator accepted — 36 of 36 hostile
manifests validated clean while 60 unit tests passed.  Each is pinned here as
a rejection test, so the false-green cannot return silently.

The suite deliberately opens with `TestGoldenPath`: a validator that rejects
everything would satisfy every rejection test in this file while being just as
useless as one that accepts everything.  The golden tests prove the validator
still says yes to a fully evidenced, fully truthful manifest.

No repository file is modified.  Fixtures are built in temporary directories.
"""

from __future__ import annotations

from contextlib import ExitStack
import copy
from dataclasses import dataclass
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "module-evidence"))

import artifacts  # noqa: E402
import lifecycle as lifecycle_mod  # noqa: E402
import paths as paths_mod  # noqa: E402
import provenance  # noqa: E402
import strict_json  # noqa: E402
import targets as targets_mod  # noqa: E402
from schema import EVIDENCE_PRODUCERS, expected_library_names  # noqa: E402
from validate_manifest import ManifestValidator, load_manifest  # noqa: E402

INCLUDED = "SparkGameFPS"
DECOY = "SparkGameDecoy"
PROFILE = "stable-v1"


# --------------------------------------------------------------------------
# Fixture construction
# --------------------------------------------------------------------------
def _git(repo: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True, text=True, check=True, timeout=60,
    )


def build_fake_repo(root: Path) -> str:
    """A minimal repository with the shape the validator reads.

    Returns the HEAD commit SHA.  A real git repository is used because the
    revision and source-tree bindings are verified through git; stubbing them
    would test the stub rather than the binding.
    """
    for module in (INCLUDED, DECOY):
        src = root / "GameModules" / module / "Source"
        src.mkdir(parents=True, exist_ok=True)
        (src / "Main.cpp").write_text(
            f"// {module} entry point\nextern \"C\" void CreateModule() {{}}\n",
            encoding="utf-8",
        )
        # add_library appears only inside a comment: text that looks like a
        # target declaration but declares nothing.
        (root / "GameModules" / module / "CMakeLists.txt").write_text(
            f"# add_library({module} SHARED Source/Main.cpp)\n"
            f"message(STATUS \"nothing is built here\")\n",
            encoding="utf-8",
        )

    site = root / "docs" / "site"
    site.mkdir(parents=True, exist_ok=True)
    (site / "readiness.json").write_text(
        json.dumps({"releaseProfiles": [{"id": PROFILE, "name": "Stable v1"}]}),
        encoding="utf-8",
    )
    items = root / "docs" / "readiness" / "work-items"
    items.mkdir(parents=True, exist_ok=True)
    (items / "00-truth.json").write_text(
        json.dumps({"workItems": [
            {"id": "MOD-310"}, {"id": "RDY-015"}, {"id": "RDY-010"},
        ]}),
        encoding="utf-8",
    )

    # Artifacts a producer would have written.  They are build outputs, so they
    # live outside version control; the validator requires them to exist and
    # carry semantically valid content.
    junit_path = root / EVIDENCE_PRODUCERS["junit-xml"]["artifact"]
    junit_path.parent.mkdir(parents=True, exist_ok=True)
    junit_path.write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<testsuites tests="5" failures="0" errors="0">\n'
        f'  <testsuite name="{INCLUDED}" tests="5" failures="0" errors="0">\n'
        f'    <testcase name="test_load" classname="{INCLUDED}.Module"/>\n'
        f'    <testcase name="test_init" classname="{INCLUDED}.Module"/>\n'
        f'    <testcase name="test_update" classname="{INCLUDED}.Module"/>\n'
        f'    <testcase name="test_unload" classname="{INCLUDED}.Module"/>\n'
        f'    <testcase name="test_shutdown" classname="{INCLUDED}.Module"/>\n'
        '  </testsuite>\n'
        '</testsuites>\n',
        encoding="utf-8",
    )
    smoke_path = root / EVIDENCE_PRODUCERS["package-smoke-log"]["artifact"]
    smoke_path.parent.mkdir(parents=True, exist_ok=True)
    smoke_path.write_text(
        f"[package-smoke] {INCLUDED}\n"
        f"[package-smoke] product=SparkEngine\n"
        f"[package-smoke] module={INCLUDED}\n"
        f"[package-smoke] library={INCLUDED}.dll\n"
        f"[package-smoke] exit_code=0\n"
        f"[package-smoke] PASS\n",
        encoding="utf-8",
    )

    _git(root, "init", "-q", "-b", "main")
    (root / ".gitignore").write_text("build/\n", encoding="utf-8")
    _git(root, "config", "user.email", "test@example.invalid")
    _git(root, "config", "user.name", "Test")
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", "fixture")
    return _git(root, "rev-parse", "HEAD").stdout.strip()


def module_entry(name: str, *, included: bool) -> dict[str, Any]:
    libs = expected_library_names(name)
    entry: dict[str, Any] = {
        "name": name,
        "cmakeTarget": name,
        "sharedLibrary": {"windows": libs["windows"], "linux": libs["linux"]},
        "sourceDirectory": f"GameModules/{name}/Source",
        "moduleKind": "Game",
        "profileApplicability": {PROFILE: "required" if included else "outside"},
        "dependencies": [],
    }
    if included:
        entry["packageSmokeOwner"] = "MOD-310"
        entry["evidenceBindings"] = [
            {"type": t, "artifactPattern": EVIDENCE_PRODUCERS[t]["artifact"]}
            for t in ("cmake-target-index", "lifecycle-log", "junit-xml",
                      "package-smoke-log")
        ]
    else:
        entry["evidenceBindings"] = []
        entry["experimentalSeparation"] = {"trackedUnder": "RDY-015"}
    return entry


def base_manifest(*, with_decoy: bool = False) -> dict[str, Any]:
    modules = [module_entry(INCLUDED, included=True)]
    excluded: list[str] = []
    if with_decoy:
        modules.append(module_entry(DECOY, included=False))
        excluded.append(DECOY)
    return {
        "schemaVersion": "stable-v2",
        "profiles": [{
            "id": PROFILE,
            "includedModules": [INCLUDED],
            "excludedModules": excluded,
        }],
        "modules": modules,
    }


def target_index(*, name: str = INCLUDED, ttype: str = "SHARED_LIBRARY",
                 sources: list[str] | None = None,
                 name_on_disk: str | None = None,
                 source_directory: str | None = None) -> dict[str, Any]:
    if sources is None:
        sources = [f"GameModules/{name}/Source/Main.cpp"]
    return {
        "schemaVersion": targets_mod.INDEX_SCHEMA_VERSION,
        "targets": {
            name: {
                "name": name,
                "type": ttype,
                "nameOnDisk": name_on_disk or f"lib{name}.so",
                "sourceDirectory": source_directory or f"GameModules/{name}",
                "sources": sources,
                "artifacts": [f"bin/lib{name}.so"],
            }
        },
    }


FAKE_ENGINE_SHA256 = "a" * 64
FAKE_ENGINE_PATH = "/usr/local/bin/SparkEngine"
FAKE_MODULE_SHA256 = "b" * 64
FAKE_MODULE_PATH = "/usr/local/lib/libSparkGameFPS.so"


def pe_image() -> bytes:
    """Minimal PE-shaped fixture; a bare MZ marker is not an executable."""
    image = bytearray(8192)
    image[:2] = b"MZ"
    image[0x3C:0x40] = (0x80).to_bytes(4, "little")
    image[0x80:0x84] = b"PE\0\0"
    return bytes(image)


@dataclass(frozen=True)
class FakeLeaseIdentity:
    """A complete, handle-style identity for collector lease tests."""

    volume_serial: int
    file_index: int
    size: int
    last_write_time: int = 1


class FakeImageLease:
    """Controlled lease double for exercising the collector's real boundaries."""

    def __init__(self, path: Path, *, identity: FakeLeaseIdentity,
                 digest: str, final_path: str | None = None,
                 attributes: int = 0, image: bytes | None = None) -> None:
        self.path = path
        self.identity = identity
        self.final_path = final_path or str(path)
        self.attributes = attributes
        self._digest = digest
        self._image = image or pe_image()
        self.closed = False

    def read_at(self, offset: int, size: int) -> bytes:
        if self.closed:
            raise OSError("attempted to read a closed fake image lease")
        return self._image[offset:offset + size]

    def sha256(self) -> str:
        if self.closed:
            raise OSError("attempted to hash a closed fake image lease")
        return self._digest

    def close(self) -> None:
        self.closed = True


def write_image_manifest(root: Path, engine: Path, module: Path, sha: str,
                          *, engine_digest: str | None = None) -> Path:
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    path = root / "module-lifecycle-images.json"
    path.write_text(json.dumps({
        "schemaVersion": "spark-image-manifest-v1", "commitSHA": sha,
        "images": [
            {"path": "SparkEngine.exe", "sha256": engine_digest or digest(engine)},
            {"path": "SparkGameFPS.dll", "sha256": digest(module)},
        ],
    }), encoding="utf-8")
    return path


def lifecycle_evidence(repo: Path, sha: str, *, module: str = INCLUDED,
                       phases: dict[str, int] | None = None,
                       tree_sha: str | None = None) -> dict[str, Any]:
    if tree_sha is None:
        tree_sha, err = lifecycle_mod.source_tree_sha(
            repo, sha, f"GameModules/{module}/Source"
        )
        assert tree_sha is not None, err
    if phases is None:
        phases = {p: 1 for p in lifecycle_mod.REQUIRED_RUNTIME_PHASES}
        phases["OnUpdate"] = 120
    return {
        "schemaVersion": lifecycle_mod.LIFECYCLE_SCHEMA_VERSION,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "commitSHA": sha,
        "records": [{
            "module": module,
            "sharedLibrary": expected_library_names(module)["linux"],
            "sourceDirectory": f"GameModules/{module}/Source",
            "sourceTreeSHA": tree_sha,
            "runner": "ctest",
            "phases": phases,
            "engineSHA256": FAKE_ENGINE_SHA256,
            "enginePath": FAKE_ENGINE_PATH,
            "moduleSHA256": FAKE_MODULE_SHA256,
            "modulePath": FAKE_MODULE_PATH,
        }],
    }


class FixtureCase(unittest.TestCase):
    """Base class owning one temporary repository for the whole class."""

    repo: Path
    sha: str
    _tmp: str

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.mkdtemp(prefix="spark-rdy010-")
        cls.repo = Path(cls._tmp) / "repo"
        cls.repo.mkdir()
        cls.sha = build_fake_repo(cls.repo)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls._tmp, ignore_errors=True)

    def validate(self, manifest: dict[str, Any], **over: Any) -> list[str]:
        kwargs: dict[str, Any] = {
            "target_index": target_index(),
            "lifecycle_evidence": lifecycle_evidence(self.repo, self.sha),
            "expected_sha": self.sha,
        }
        kwargs.update(over)
        return ManifestValidator(manifest, self.repo, **kwargs).validate()

    def assertRejected(self, manifest: dict[str, Any], case: str,
                       **over: Any) -> list[str]:
        errors = self.validate(manifest, **over)
        self.assertNotEqual(
            errors, [],
            f"[{case}] hostile manifest was ACCEPTED — this is a false-green",
        )
        return errors

    def assertAccepted(self, manifest: dict[str, Any], **over: Any) -> None:
        errors = self.validate(manifest, **over)
        self.assertEqual(errors, [], f"truthful manifest was rejected: {errors}")


# --------------------------------------------------------------------------
# Golden path — the validator must still say yes to the truth
# --------------------------------------------------------------------------
class TestGoldenPath(FixtureCase):
    """Guards against a validator that passes every rejection test by
    rejecting everything."""

    def test_fully_evidenced_manifest_is_accepted(self) -> None:
        self.assertAccepted(base_manifest())

    def test_manifest_with_experimental_module_is_accepted(self) -> None:
        self.assertAccepted(base_manifest(with_decoy=True))

    def test_shipped_manifest_passes_the_declarative_layer(self) -> None:
        """The checked-in manifest must at least be internally consistent."""
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json"
        )
        errors = ManifestValidator(manifest, REPO_ROOT, policy_only=True).validate()
        self.assertEqual(errors, [], f"shipped manifest has policy errors: {errors}")

    def test_shipped_manifest_declares_all_eleven_modules(self) -> None:
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json"
        )
        self.assertEqual(len(manifest["modules"]), 11)

    def test_shipped_manifest_blocks_without_runtime_evidence(self) -> None:
        """RDY-010's own manifest must not validate while evidence is absent."""
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json"
        )
        errors = ManifestValidator(manifest, REPO_ROOT).validate()
        self.assertTrue(
            any("unavailable" in e for e in errors),
            f"expected a blocking unavailable-evidence error, got {errors}",
        )


# --------------------------------------------------------------------------
# B01-B02 — target and source proof must be authoritative, not textual
# --------------------------------------------------------------------------
class TestTargetProof(FixtureCase):

    def test_B01_comment_only_add_library_is_not_a_target(self) -> None:
        """The fixture's CMakeLists declares the target only in a comment."""
        errors = self.assertRejected(base_manifest(), "B01", target_index=None,
                                     target_error="no codemodel")
        self.assertTrue(any("target evidence is unavailable" in e.lower()
                            or "unavailable" in e for e in errors))

    def test_B01b_target_absent_from_codemodel_is_rejected(self) -> None:
        self.assertRejected(base_manifest(), "B01b",
                            target_index=target_index(name="SomethingElse"))

    def test_B02_target_with_zero_sources_is_rejected(self) -> None:
        self.assertRejected(base_manifest(), "B02",
                            target_index=target_index(sources=[]))

    def test_B02b_sources_outside_declared_tree_are_rejected(self) -> None:
        self.assertRejected(
            base_manifest(), "B02b",
            target_index=target_index(sources=["SparkEngine/Source/Other.cpp"]),
        )

    def test_target_of_wrong_type_is_rejected(self) -> None:
        self.assertRejected(base_manifest(), "B01c",
                            target_index=target_index(ttype="STATIC_LIBRARY"))

    def test_target_index_naming_a_foreign_library_is_rejected(self) -> None:
        self.assertRejected(base_manifest(), "B01d",
                            target_index=target_index(name_on_disk="libOther.so"))

    def test_toolchain_variant_library_names_are_accepted(self) -> None:
        """MinGW emits libX.dll, MSVC X.dll, GCC libX.so — all legitimate."""
        for on_disk in (f"lib{INCLUDED}.so", f"lib{INCLUDED}.dll",
                        f"{INCLUDED}.dll", f"lib{INCLUDED}.dylib"):
            with self.subTest(nameOnDisk=on_disk):
                self.assertAccepted(
                    base_manifest(),
                    target_index=target_index(name_on_disk=on_disk),
                )

    def test_unconfigured_tree_raises_rather_than_returning_empty(self) -> None:
        with self.assertRaises(targets_mod.TargetEvidenceUnavailable):
            targets_mod.load_target_index(self.repo / "nope" / "targets.json")

    def test_codemodel_with_no_targets_raises(self) -> None:
        reply = self.repo / "emptyreply"
        reply.mkdir(exist_ok=True)
        with self.assertRaises(targets_mod.TargetEvidenceUnavailable):
            targets_mod.extract_from_reply(reply)


# --------------------------------------------------------------------------
# B03-B09 — exact lexical and canonical source paths
# --------------------------------------------------------------------------
class TestSourceDirectoryExactness(FixtureCase):

    def _with_source(self, value: str) -> dict[str, Any]:
        m = base_manifest()
        m["modules"][0]["sourceDirectory"] = value
        return m

    def test_B03_module_root_is_not_the_source_tree(self) -> None:
        self.assertRejected(self._with_source(f"GameModules/{INCLUDED}"), "B03")

    def test_B04_source_subdirectory_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/Source/Core"), "B04")

    def test_B05_dot_segment_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/./Source"), "B05")

    def test_B06_empty_segment_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules//{INCLUDED}/Source"), "B06")

    def test_B07_case_aliased_source_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/source"), "B07")

    def test_B07b_case_aliased_root_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"gamemodules/{INCLUDED}/Source"), "B07b")

    def test_B08_trailing_slash_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/Source/"), "B08")

    def test_B09_reparse_point_source_is_rejected(self) -> None:
        link = self.repo / "GameModules" / INCLUDED / "Linked"
        target = self.repo / "GameModules" / DECOY / "Source"
        made = False
        if not link.exists():
            try:
                os.symlink(str(target), str(link), target_is_directory=True)
                made = True
            except (OSError, NotImplementedError):
                if sys.platform == "win32":
                    rc = subprocess.run(
                        ["cmd", "/c", "mklink", "/J", str(link), str(target)],
                        capture_output=True, text=True, check=False,
                    )
                    made = rc.returncode == 0
        if not (made or link.exists()):
            self.skipTest("this OS/account cannot create symlinks or junctions")
        self.assertTrue(
            paths_mod._is_reparse_point(link),
            "fixture link was not detected as a reparse point",
        )
        # Even under its exact expected name, a reparse point is not source.
        errors = paths_mod.check_on_disk(
            f"GameModules/{INCLUDED}/Linked", self.repo)
        self.assertNotEqual(errors, [], "[B09] reparse point was ACCEPTED")

    def test_traversal_escape_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/Source/../../{DECOY}/Source"),
            "B03b")

    def test_sibling_module_source_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{DECOY}/Source"), "B03c")

    def test_absolute_and_home_paths_are_rejected(self) -> None:
        for value in ("/etc/passwd", "C:/Windows", "~/evil", "$HOME/evil",
                      "%USERPROFILE%/evil", "\\\\server\\share"):
            with self.subTest(path=value):
                self.assertRejected(self._with_source(value), "B03d")

    def test_alternate_data_stream_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/Source:hidden"), "B03e")

    def test_short_name_alias_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GAMEMO~1/{INCLUDED}/Source"), "B03f")

    def test_reserved_device_name_is_rejected(self) -> None:
        self.assertRejected(self._with_source(f"GameModules/{INCLUDED}/NUL"),
                            "B03g")

    def test_nonexistent_directory_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["name"] = "SparkGameGhost"
        m["modules"][0]["cmakeTarget"] = "SparkGameGhost"
        m["modules"][0]["sharedLibrary"] = {
            "windows": "SparkGameGhost.dll", "linux": "libSparkGameGhost.so"}
        m["modules"][0]["sourceDirectory"] = "GameModules/SparkGameGhost/Source"
        m["profiles"][0]["includedModules"] = ["SparkGameGhost"]
        self.assertRejected(m, "B03h")


# --------------------------------------------------------------------------
# B10 — declared phases are not runtime proof
# --------------------------------------------------------------------------
class TestLifecycleIsRuntimeProof(FixtureCase):

    def test_B10_declared_phase_list_is_not_accepted_as_evidence(self) -> None:
        """The schema no longer even has a lifecyclePhases key to declare."""
        m = base_manifest()
        m["modules"][0]["lifecyclePhases"] = [
            "SparkGetModuleCompatibility", "CreateModule", "GetModuleInfo",
            "OnLoad", "OnUpdate", "OnUnload", "DestroyModule",
        ]
        errors = self.assertRejected(m, "B10")
        self.assertTrue(any("unknown keys" in e and "lifecyclePhases" in e
                            for e in errors), errors)

    def test_missing_lifecycle_evidence_blocks(self) -> None:
        errors = self.assertRejected(
            base_manifest(), "B10b",
            lifecycle_evidence=None, lifecycle_error="not found")
        self.assertTrue(any("lifecycle evidence is unavailable" in e for e in errors))

    def test_zero_count_phase_did_not_run(self) -> None:
        phases = {p: 1 for p in lifecycle_mod.REQUIRED_RUNTIME_PHASES}
        phases["OnUpdate"] = 0
        self.assertRejected(
            base_manifest(), "B10c",
            lifecycle_evidence=lifecycle_evidence(self.repo, self.sha, phases=phases))

    def test_each_required_phase_is_individually_required(self) -> None:
        for phase in lifecycle_mod.REQUIRED_RUNTIME_PHASES:
            with self.subTest(phase=phase):
                phases = {p: 1 for p in lifecycle_mod.REQUIRED_RUNTIME_PHASES}
                del phases[phase]
                self.assertRejected(
                    base_manifest(), f"B10-{phase}",
                    lifecycle_evidence=lifecycle_evidence(
                        self.repo, self.sha, phases=phases))

    def test_evidence_for_a_different_source_tree_is_rejected(self) -> None:
        self.assertRejected(
            base_manifest(), "B10d",
            lifecycle_evidence=lifecycle_evidence(
                self.repo, self.sha, tree_sha="0" * 40))

    def test_evidence_for_a_different_module_is_rejected(self) -> None:
        self.assertRejected(
            base_manifest(), "B10e",
            lifecycle_evidence=lifecycle_evidence(self.repo, self.sha, module=DECOY))

    def test_unknown_runner_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["runner"] = "trust-me"
        self.assertRejected(base_manifest(), "B10f", lifecycle_evidence=ev)

    def test_unknown_phase_name_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["phases"]["OnMagic"] = 1
        self.assertRejected(base_manifest(), "B10g", lifecycle_evidence=ev)

    def test_lifecycle_record_with_unknown_keys_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["backdoor"] = "accepted"
        self.assertRejected(base_manifest(), "B10h", lifecycle_evidence=ev)

    def test_boolean_phase_count_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["phases"]["OnUpdate"] = True
        self.assertRejected(base_manifest(), "B10i", lifecycle_evidence=ev)

    def test_empty_records_raises_rather_than_passing(self) -> None:
        path = self.repo / "lc.json"
        path.write_text(json.dumps({
            "schemaVersion": lifecycle_mod.LIFECYCLE_SCHEMA_VERSION,
            "records": [],
        }), encoding="utf-8")
        with self.assertRaises(lifecycle_mod.LifecycleEvidenceUnavailable):
            lifecycle_mod.load_lifecycle_evidence(path)

    def test_source_tree_sha_changes_when_source_changes(self) -> None:
        """The binding must actually discriminate."""
        first, err = lifecycle_mod.source_tree_sha(
            self.repo, self.sha, f"GameModules/{INCLUDED}/Source")
        self.assertIsNone(err)
        (self.repo / "GameModules" / INCLUDED / "Source" / "Extra.cpp").write_text(
            "// changed\n", encoding="utf-8")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", "change source")
        new_sha = _git(self.repo, "rev-parse", "HEAD").stdout.strip()
        second, err = lifecycle_mod.source_tree_sha(
            self.repo, new_sha, f"GameModules/{INCLUDED}/Source")
        self.assertIsNone(err)
        self.assertNotEqual(first, second)
        # Restore so later tests in this class see the original tree.
        _git(self.repo, "reset", "-q", "--hard", self.sha)


# --------------------------------------------------------------------------
# Engine binary authenticity — arbitrary marker printers are not engines
# --------------------------------------------------------------------------
class TestLifecycleAuthenticity(FixtureCase):

    def test_script_cmd_rejected_as_engine(self) -> None:
        from collect_lifecycle import validate_engine_binary
        script = self.repo / "fake.cmd"
        script.write_text("@echo off\necho hello\n", encoding="utf-8")
        err = validate_engine_binary(script)
        self.assertIsNotNone(err, "a .cmd script was accepted as an engine")
        self.assertIn("script", err)

    def test_script_bat_rejected_as_engine(self) -> None:
        from collect_lifecycle import validate_engine_binary
        script = self.repo / "fake.bat"
        script.write_text("@echo off\n", encoding="utf-8")
        err = validate_engine_binary(script)
        self.assertIsNotNone(err)

    def test_script_sh_rejected_as_engine(self) -> None:
        from collect_lifecycle import validate_engine_binary
        script = self.repo / "fake.sh"
        script.write_text("#!/bin/sh\necho hello\n", encoding="utf-8")
        err = validate_engine_binary(script)
        self.assertIsNotNone(err)

    def test_wrong_name_rejected_as_engine(self) -> None:
        from collect_lifecycle import validate_engine_binary
        binary = self.repo / "NotAnEngine.exe"
        binary.write_bytes(b"\x00" * 8192)
        err = validate_engine_binary(binary)
        self.assertIsNotNone(err, "a binary with wrong name was accepted")
        self.assertIn("stable-v1", err)

    def test_tiny_file_rejected_as_engine(self) -> None:
        from collect_lifecycle import validate_engine_binary
        binary = self.repo / "SparkEngine.exe"
        binary.write_bytes(b"\x00" * 100)
        err = validate_engine_binary(binary)
        self.assertIsNotNone(err, "a tiny binary was accepted as an engine")
        self.assertIn("bytes", err)

    def test_script_content_in_exe_rejected(self) -> None:
        from collect_lifecycle import validate_engine_binary
        binary = self.repo / "SparkEngine.exe"
        binary.write_bytes(b"@echo off\r\n" + b"\x00" * 8192)
        err = validate_engine_binary(binary)
        self.assertIsNotNone(err, "a script disguised as .exe was accepted")
        self.assertIn("script signature", err)

    def test_shebang_content_in_binary_rejected(self) -> None:
        from collect_lifecycle import validate_engine_binary
        binary = self.repo / "SparkEngine"
        binary.write_bytes(b"#!/bin/sh\necho lifecycle\n" + b"\x00" * 8192)
        err = validate_engine_binary(binary)
        self.assertIsNotNone(err)

    def test_symlink_engine_rejected(self) -> None:
        from collect_lifecycle import validate_engine_binary
        real = self.repo / "RealEngine"
        real.write_bytes(b"\x00" * 8192)
        link = self.repo / "SparkEngine"
        made = False
        try:
            os.symlink(str(real), str(link))
            made = True
        except (OSError, NotImplementedError):
            pass
        if not made:
            self.skipTest("cannot create symlinks on this platform")
        err = validate_engine_binary(link)
        self.assertIsNotNone(err, "a symlinked engine was accepted")
        self.assertIn("reparse point", err.lower() if "reparse" in err.lower() else err)

    def test_nonexistent_engine_rejected(self) -> None:
        from collect_lifecycle import validate_engine_binary
        err = validate_engine_binary(self.repo / "no-such-binary")
        self.assertIsNotNone(err)

    def test_plausible_engine_binary_accepted(self) -> None:
        from collect_lifecycle import validate_engine_binary
        binary = self.repo / "SparkEngine.exe"
        binary.write_bytes(pe_image())
        err = validate_engine_binary(binary)
        self.assertIsNone(err, f"valid engine binary rejected: {err}")

    def test_missing_engine_sha256_in_record_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        del ev["records"][0]["engineSHA256"]
        errors = self.assertRejected(base_manifest(), "AUTH1",
                                     lifecycle_evidence=ev)
        self.assertTrue(any("engineSHA256" in e or "missing" in e for e in errors))

    def test_missing_engine_path_in_record_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        del ev["records"][0]["enginePath"]
        errors = self.assertRejected(base_manifest(), "AUTH2",
                                     lifecycle_evidence=ev)
        self.assertTrue(any("enginePath" in e or "missing" in e for e in errors))

    def test_invalid_engine_sha256_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["engineSHA256"] = "not-a-hash"
        errors = self.assertRejected(base_manifest(), "AUTH3",
                                     lifecycle_evidence=ev)
        self.assertTrue(any("engineSHA256" in e for e in errors))

    def test_empty_engine_path_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["enginePath"] = ""
        errors = self.assertRejected(base_manifest(), "AUTH4",
                                     lifecycle_evidence=ev)
        self.assertTrue(any("enginePath" in e for e in errors))

# --------------------------------------------------------------------------
# Collector contract — one host-owned terminal record, no trace reconstruction
# --------------------------------------------------------------------------
class TestLifecycleCollector(FixtureCase):

    VALID_RECORD = (
        "SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 "
        "update=4 fixed=2 render=4 unload=1 destroy=1 faults=0"
    )

    def _collector_main_fixture(self, name: str) -> tuple[Path, list[str]]:
        """Build a truthful image pair for main() transaction tests."""
        root = self.repo / name
        root.mkdir()
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        # The release collector has one non-negotiable publication namespace.
        # Fixtures deliberately use that same path so transaction tests cannot
        # quietly exercise an arbitrary caller-controlled output directory.
        out = self.repo / "build" / "module-evidence" / "module-lifecycle.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        for artifact in (out, log):
            if artifact.exists():
                artifact.unlink()
        image_manifest = write_image_manifest(root, engine, module, self.sha)
        return out, [
            "collect_lifecycle.py", "--engine", str(engine), "--module", INCLUDED,
            "--module-image", str(module), "--working-directory", str(root),
            "--rhi-backend", "d3d11", "--image-manifest", str(image_manifest),
            "--out", str(out), "--commit-sha", self.sha,
        ]

    def _run_truthful_collector_main(
        self, argv: list[str], *extra_patches: object
    ) -> int:
        """Run main's real publication path while isolating the engine process."""
        import collect_lifecycle

        engine = Path(argv[argv.index("--engine") + 1])
        module = Path(argv[argv.index("--module-image") + 1])
        image_manifest = Path(argv[argv.index("--image-manifest") + 1])
        engine_digest = hashlib.sha256(engine.read_bytes()).hexdigest()
        module_digest = hashlib.sha256(module.read_bytes()).hexdigest()
        engine_image = collect_lifecycle.ImageVerification(
            digest=engine_digest,
            final_path=str(engine),
            identity=collect_lifecycle.ImageIdentity(1, 1, engine.stat().st_size, 1),
        )
        module_image = collect_lifecycle.ImageVerification(
            digest=module_digest,
            final_path=str(module),
            identity=collect_lifecycle.ImageIdentity(1, 2, module.stat().st_size, 1),
        )
        manifest_bytes = image_manifest.read_bytes()

        def manifest_lease_factory(path: Path) -> FakeImageLease:
            self.assertEqual(path, image_manifest)
            return FakeImageLease(
                path,
                identity=FakeLeaseIdentity(1, 3, len(manifest_bytes)),
                digest=hashlib.sha256(manifest_bytes).hexdigest(),
                image=manifest_bytes,
            )

        with ExitStack() as stack:
            stack.enter_context(mock.patch.object(sys, "argv", argv))
            stack.enter_context(mock.patch("collect_lifecycle.os.name", "nt"))
            stack.enter_context(mock.patch("collect_lifecycle.REPO_ROOT", self.repo))
            stack.enter_context(mock.patch(
                "collect_lifecycle.resolve_head_sha", return_value=(self.sha, None),
            ))
            stack.enter_context(mock.patch.dict(
                "collect_lifecycle.os.environ", {"GITHUB_SHA": self.sha}, clear=False,
            ))
            stack.enter_context(mock.patch(
                "collect_lifecycle.lifecycle_mod.source_tree_sha",
                return_value=("c" * 40, None),
            ))
            stack.enter_context(mock.patch(
                "collect_lifecycle.run_engine",
                return_value=(
                    collect_lifecycle.EngineOutput(
                        self.VALID_RECORD, "", engine_image, module_image,
                    ),
                    None,
                ),
            ))
            stack.enter_context(mock.patch(
                "collect_lifecycle.open_image_lease",
                side_effect=manifest_lease_factory,
            ))
            for patch in extra_patches:
                stack.enter_context(patch)
            return collect_lifecycle.main()

    def _assert_no_transaction_artifacts(self, out: Path) -> None:
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        self.assertFalse(out.exists(), f"partial JSON publication survived at {out}")
        self.assertFalse(log.exists(), f"partial log publication survived at {log}")
        for final_path in (out, log):
            self.assertEqual(
                list(final_path.parent.glob(f".{final_path.name}.*.tmp")), [],
                f"temporary publication sibling survived for {final_path.name}",
            )

    @staticmethod
    def _fdopen_that_fails(phase: str):
        """Fail one real temporary stream operation but still close its descriptor."""
        real_fdopen = os.fdopen

        class FailingTempStream:
            def __init__(self, stream) -> None:
                self._stream = stream

            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc_value, traceback) -> bool:
                try:
                    self._stream.close()
                finally:
                    if phase == "close":
                        raise OSError("injected temporary close failure")
                return False

            def write(self, content: str) -> int:
                if phase == "write":
                    raise OSError("injected temporary write failure")
                return self._stream.write(content)

            def flush(self) -> None:
                if phase == "flush":
                    raise OSError("injected temporary flush failure")
                self._stream.flush()

        def failing_fdopen(descriptor: int, *args, **kwargs):
            return FailingTempStream(real_fdopen(descriptor, *args, **kwargs))

        return failing_fdopen

    @staticmethod
    def _replace_that_fails(ordinal: int):
        """Return a replacement operation that fails only at the requested call."""
        real_replace = os.replace

        def fail_replace(source, destination):
            fail_replace.call_count += 1
            if fail_replace.call_count == ordinal:
                raise OSError(f"injected replace {ordinal} failure")
            return real_replace(source, destination)

        fail_replace.call_count = 0
        return fail_replace

    def test_main_cleans_all_transaction_artifacts_after_transient_stale_unlink_failure(
        self,
    ) -> None:
        """A stale-clear failure must still run the shared cleanup transaction."""
        out, argv = self._collector_main_fixture("stale-unlink")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        out.write_text("stale-json", encoding="utf-8")
        log.write_text("stale-log", encoding="utf-8")
        real_unlink = Path.unlink
        failed_once = False

        def fail_only_the_initial_json_unlink(path: Path, *args, **kwargs):
            nonlocal failed_once
            if path == out and not failed_once:
                failed_once = True
                raise OSError("injected stale JSON unlink failure")
            return real_unlink(path, *args, **kwargs)

        with mock.patch.object(Path, "unlink", new=fail_only_the_initial_json_unlink):
            self.assertEqual(self._run_truthful_collector_main(argv), 1)
        self.assertTrue(failed_once, "the stale-clear failure injection was not exercised")
        self._assert_no_transaction_artifacts(out)

    def test_main_cleans_all_transaction_artifacts_after_each_publication_failure(
        self,
    ) -> None:
        """A status or either atomic replacement failure cannot leave a partial pair."""
        real_print = print

        def fail_final_status(*args, **kwargs):
            if args and str(args[0]).startswith("OK: wrote "):
                raise OSError("injected final status failure")
            return real_print(*args, **kwargs)

        out, argv = self._collector_main_fixture("publish-status")
        self.assertEqual(self._run_truthful_collector_main(
            argv, mock.patch("builtins.print", side_effect=fail_final_status),
        ), 1)
        self._assert_no_transaction_artifacts(out)

        for ordinal in (1, 2):
            with self.subTest(failure=f"replace-{ordinal}"):
                out, argv = self._collector_main_fixture(f"publish-replace-{ordinal}")
                replace_failure = self._replace_that_fails(ordinal)
                self.assertEqual(self._run_truthful_collector_main(
                    argv, mock.patch("collect_lifecycle.os.replace",
                                    side_effect=replace_failure),
                ), 1)
                self.assertEqual(replace_failure.call_count, ordinal)
                self._assert_no_transaction_artifacts(out)

    def test_main_cleans_all_transaction_artifacts_after_temp_stream_failures(
        self,
    ) -> None:
        """Write, flush, and close failures clean the temp created by _write_temp."""
        for phase in ("write", "flush", "close"):
            with self.subTest(phase=phase):
                out, argv = self._collector_main_fixture(f"temp-{phase}")
                self.assertEqual(self._run_truthful_collector_main(
                    argv,
                    mock.patch("collect_lifecycle.os.fdopen",
                               side_effect=self._fdopen_that_fails(phase)),
                ), 1)
                self._assert_no_transaction_artifacts(out)

    def test_write_temp_closes_descriptor_when_fdopen_never_takes_ownership(self) -> None:
        """A failed fdopen construction cannot leak the mkstemp descriptor."""
        import collect_lifecycle

        root = self.repo / "fdopen-construction-failure"
        root.mkdir()
        final_path = root / "module-lifecycle.json"
        tracked: set[Path] = set()
        descriptors: list[int] = []

        def fail_before_ownership(descriptor: int, *args, **kwargs):
            descriptors.append(descriptor)
            raise OSError("injected fdopen construction failure")

        with mock.patch("collect_lifecycle.os.fdopen", side_effect=fail_before_ownership):
            with self.assertRaisesRegex(OSError, "fdopen construction failure"):
                collect_lifecycle._write_temp(final_path, "payload", tracked_temps=tracked)

        self.assertEqual(len(descriptors), 1)
        with self.assertRaises(OSError):
            os.fstat(descriptors[0])
        self.assertFalse(final_path.exists())
        self.assertEqual(tracked, set())
        self.assertEqual(list(root.glob(".module-lifecycle.json.*.tmp")), [])

    def test_engine_output_audit_log_always_delimits_stdout_and_stderr(self) -> None:
        """The audit trail remains line-oriented when stdout lacks a newline."""
        from collect_lifecycle import EngineOutput

        self.assertEqual(
            EngineOutput("stdout-without-newline", "stderr").audit_log,
            "--- stdout ---\nstdout-without-newline\n--- stderr ---\nstderr",
        )

    def test_main_rejects_unsafe_output_and_module_inputs_before_any_clear(self) -> None:
        """No caller-controlled alias or collision can select a deletion target."""
        import collect_lifecycle

        cases = (
            ("module-traversal", "--module", r"\..\..\victim"),
            ("output-is-log", "--out", "log"),
            ("output-is-engine", "--out", "engine"),
            ("output-is-module", "--out", "module"),
            ("output-is-manifest", "--out", "manifest"),
            ("engine-trailing-dot", "--engine", "engine-trailing-dot"),
            ("module-image-ads", "--module-image", "module-image-ads"),
            ("output-dot-alias", "--out", "dot-alias"),
            ("output-trailing-dot", "--out", "trailing-dot"),
            ("output-trailing-space", "--out", "trailing-space"),
            ("output-ads", "--out", "ads"),
        )
        for name, option, replacement in cases:
            with self.subTest(case=name):
                out, argv = self._collector_main_fixture(f"namespace-{name}")
                log = out.parent / "module-lifecycle-SparkGameFPS.log"
                engine = Path(argv[argv.index("--engine") + 1])
                module = Path(argv[argv.index("--module-image") + 1])
                manifest = Path(argv[argv.index("--image-manifest") + 1])
                out.write_text("protected-json", encoding="utf-8")
                log.write_text("protected-log", encoding="utf-8")
                values = {
                    "log": str(log),
                    "engine": str(engine),
                    "module": str(module),
                    "manifest": str(manifest),
                    "engine-trailing-dot": str(engine) + ".",
                    "module-image-ads": str(module) + ":alternate",
                    "dot-alias": str(self.repo / "build") +
                    r"\.\module-evidence\module-lifecycle.json",
                    "trailing-dot": str(out) + ".",
                    "trailing-space": str(out) + " ",
                    "ads": str(out) + ":audit",
                }
                argv = list(argv)
                argv[argv.index(option) + 1] = values.get(replacement, replacement)
                protected = {
                    engine: engine.read_bytes(),
                    module: module.read_bytes(),
                    manifest: manifest.read_bytes(),
                }
                with mock.patch.object(sys, "argv", argv), \
                     mock.patch("collect_lifecycle.os.name", "nt"), \
                     mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
                     mock.patch("collect_lifecycle.resolve_head_sha",
                                return_value=(self.sha, None)), \
                     mock.patch("collect_lifecycle._clear_artifacts",
                                wraps=collect_lifecycle._clear_artifacts) as clear, \
                     mock.patch("collect_lifecycle.run_engine") as child:
                    self.assertEqual(collect_lifecycle.main(), 1)
                clear.assert_not_called()
                child.assert_not_called()
                self.assertEqual(out.read_text(encoding="utf-8"), "protected-json")
                self.assertEqual(log.read_text(encoding="utf-8"), "protected-log")
                for path, content in protected.items():
                    self.assertEqual(path.read_bytes(), content, path)

    def test_main_rejects_reparse_output_namespace_before_any_clear(self) -> None:
        """Every fixed output ancestor is checked before stale evidence is touched."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("namespace-reparse")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        out.write_text("protected-json", encoding="utf-8")
        log.write_text("protected-log", encoding="utf-8")

        def synthetic_reparse(path: Path) -> bool:
            return path == out.parent

        with mock.patch.object(sys, "argv", argv), \
             mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
             mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch("collect_lifecycle.paths_mod._is_reparse_point",
                        side_effect=synthetic_reparse), \
             mock.patch("collect_lifecycle._clear_artifacts",
                        wraps=collect_lifecycle._clear_artifacts) as clear, \
             mock.patch("collect_lifecycle.run_engine") as child:
            self.assertEqual(collect_lifecycle.main(), 1)

        clear.assert_not_called()
        child.assert_not_called()
        self.assertEqual(out.read_text(encoding="utf-8"), "protected-json")
        self.assertEqual(log.read_text(encoding="utf-8"), "protected-log")

    def test_manifest_lease_reads_held_bytes_not_replaced_path(self) -> None:
        """A manifest lease, not a later pathname read, supplies authority."""
        import collect_lifecycle

        root = self.repo / "manifest-held-bytes"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        manifest = write_image_manifest(root, engine, module, self.sha)
        held_bytes = manifest.read_bytes()
        manifest.write_text('{"schemaVersion":"attacker-replacement"}', encoding="utf-8")
        lease = FakeImageLease(
            manifest,
            identity=FakeLeaseIdentity(31, 901, len(held_bytes)),
            digest=hashlib.sha256(held_bytes).hexdigest(),
            image=held_bytes,
        )

        trusted, error = collect_lifecycle.load_image_manifest_from_lease(
            lease, manifest, root, self.sha,
        )

        self.assertIsNone(error)
        self.assertEqual(
            trusted,
            {
                "SparkEngine.exe": hashlib.sha256(pe_image()).hexdigest(),
                "SparkGameFPS.dll": hashlib.sha256(pe_image()).hexdigest(),
            },
        )
        lease.close()

    def test_main_holds_manifest_lease_across_run_and_publishes_distinct_audit_log(self) -> None:
        """Replacing the pathname after its lease opens cannot swap manifest authority."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("manifest-held-through-run")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        engine = Path(argv[argv.index("--engine") + 1])
        module = Path(argv[argv.index("--module-image") + 1])
        manifest = Path(argv[argv.index("--image-manifest") + 1])
        held_bytes = manifest.read_bytes()
        lease = FakeImageLease(
            manifest,
            identity=FakeLeaseIdentity(32, 902, len(held_bytes)),
            digest=hashlib.sha256(held_bytes).hexdigest(),
            image=held_bytes,
        )
        engine_digest = hashlib.sha256(engine.read_bytes()).hexdigest()
        module_digest = hashlib.sha256(module.read_bytes()).hexdigest()
        engine_image = collect_lifecycle.ImageVerification(
            engine_digest, str(engine),
            collect_lifecycle.ImageIdentity(32, 903, engine.stat().st_size, 1),
        )
        module_image = collect_lifecycle.ImageVerification(
            module_digest, str(module),
            collect_lifecycle.ImageIdentity(32, 904, module.stat().st_size, 1),
        )
        opened: list[FakeImageLease] = []

        def manifest_factory(path: Path) -> FakeImageLease:
            self.assertEqual(path, manifest)
            opened.append(lease)
            # A path-based parse after open would now authorize this replacement.
            manifest.write_text('{"schemaVersion":"attacker-replacement"}',
                                encoding="utf-8")
            return lease

        def run_with_live_manifest(*args, **kwargs):
            self.assertFalse(lease.closed, "manifest lease was released before image leases/run")
            return collect_lifecycle.EngineOutput(
                self.VALID_RECORD, "stderr-without-newline", engine_image, module_image,
            ), None

        with mock.patch.object(sys, "argv", argv), \
             mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
             mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch.dict("collect_lifecycle.os.environ", {"GITHUB_SHA": self.sha}, clear=False), \
             mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                        return_value=("c" * 40, None)), \
             mock.patch("collect_lifecycle.open_image_lease", side_effect=manifest_factory), \
             mock.patch("collect_lifecycle.run_engine", side_effect=run_with_live_manifest):
            self.assertEqual(collect_lifecycle.main(), 0)

        self.assertEqual(opened, [lease])
        self.assertTrue(lease.closed)
        self.assertTrue(out.is_file())
        self.assertTrue(log.is_file())
        self.assertNotEqual(out, log)
        self.assertIn("\n--- stderr ---\n", log.read_text(encoding="utf-8"))

    def test_main_closes_manifest_lease_after_engine_failure(self) -> None:
        """Manifest lease ownership is released even when launch/collection fails."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("manifest-close-on-failure")
        manifest = Path(argv[argv.index("--image-manifest") + 1])
        manifest_bytes = manifest.read_bytes()
        lease = FakeImageLease(
            manifest,
            identity=FakeLeaseIdentity(33, 905, len(manifest_bytes)),
            digest=hashlib.sha256(manifest_bytes).hexdigest(),
            image=manifest_bytes,
        )
        with mock.patch.object(sys, "argv", argv), \
             mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
             mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch.dict("collect_lifecycle.os.environ", {"GITHUB_SHA": self.sha}, clear=False), \
             mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                        return_value=("c" * 40, None)), \
             mock.patch("collect_lifecycle.open_image_lease", return_value=lease), \
             mock.patch("collect_lifecycle.run_engine", return_value=(None, "injected launch failure")):
            self.assertEqual(collect_lifecycle.main(), 1)

        self.assertTrue(lease.closed)
        self._assert_no_transaction_artifacts(out)

    def test_main_fails_closed_when_native_manifest_lease_is_unavailable(self) -> None:
        """A Windows collector never falls back to reading its manifest by path."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("manifest-native-lease-required")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        out.write_text("preexisting-json", encoding="utf-8")
        log.write_text("preexisting-log", encoding="utf-8")
        with mock.patch.object(sys, "argv", argv), \
             mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
             mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch.dict("collect_lifecycle.os.environ", {"GITHUB_SHA": self.sha}, clear=False), \
             mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                        return_value=("c" * 40, None)), \
             mock.patch("collect_lifecycle.WINDOWS_IMAGE_LEASE_AVAILABLE", False), \
             mock.patch("collect_lifecycle.run_engine") as child:
            self.assertEqual(collect_lifecycle.main(), 1)

        child.assert_not_called()
        self.assertEqual(out.read_text(encoding="utf-8"), "preexisting-json")
        self.assertEqual(log.read_text(encoding="utf-8"), "preexisting-log")

    def test_main_records_handle_derived_paths_and_digests(self) -> None:
        """Published evidence must use lease metadata, never pre-launch Path.resolve values."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("handle-derived-record")
        engine = Path(argv[argv.index("--engine") + 1])
        module = Path(argv[argv.index("--module-image") + 1])
        engine_digest = hashlib.sha256(engine.read_bytes()).hexdigest()
        module_digest = hashlib.sha256(module.read_bytes()).hexdigest()
        engine_image = collect_lifecycle.ImageVerification(
            digest=engine_digest,
            final_path=r"\\?\C:\verified-artifact\SparkEngine.exe",
            identity=collect_lifecycle.ImageIdentity(21, 701, engine.stat().st_size, 1),
        )
        module_image = collect_lifecycle.ImageVerification(
            digest=module_digest,
            final_path=r"\\?\C:\verified-artifact\SparkGameFPS.dll",
            identity=collect_lifecycle.ImageIdentity(21, 702, module.stat().st_size, 1),
        )

        self.assertEqual(self._run_truthful_collector_main(
            argv,
            mock.patch(
                "collect_lifecycle.run_engine",
                return_value=(
                    collect_lifecycle.EngineOutput(
                        self.VALID_RECORD, "", engine_image, module_image,
                    ),
                    None,
                ),
            ),
        ), 0)

        record = json.loads(out.read_text(encoding="utf-8"))["records"][0]
        self.assertEqual(record["engineSHA256"], engine_digest)
        self.assertEqual(record["enginePath"], engine_image.final_path)
        self.assertEqual(record["moduleSHA256"], module_digest)
        self.assertEqual(record["modulePath"], module_image.final_path)

    def test_parses_exact_standalone_terminal_record(self) -> None:
        from collect_lifecycle import parse_terminal_record
        self.assertEqual(parse_terminal_record(self.VALID_RECORD, INCLUDED), {
            "CreateModule": 1, "OnLoad": 1, "OnUpdate": 4,
            "OnFixedUpdate": 2, "OnRender": 4, "OnUnload": 1,
            "DestroyModule": 1,
        })

    def test_rejects_duplicate_prefixed_wrong_or_malformed_terminal_records(self) -> None:
        from collect_lifecycle import parse_terminal_record
        bad_inputs = (
            self.VALID_RECORD + "\n" + self.VALID_RECORD,
            "[info] " + self.VALID_RECORD,
            self.VALID_RECORD.replace("module=SparkGameFPS", "module=Other"),
            self.VALID_RECORD.replace("faults=0", "faults=0 extra=1"),
            self.VALID_RECORD.replace("create=1", "create=0"),
            self.VALID_RECORD.replace("faults=0", "faults=1"),
        )
        for text in bad_inputs:
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_terminal_record(text, INCLUDED)

    def test_rejects_marker_in_stderr_or_embedded_stream_copy(self) -> None:
        from collect_lifecycle import parse_terminal_streams
        self.assertEqual(parse_terminal_streams(self.VALID_RECORD, "", INCLUDED)["OnLoad"], 1)
        for stdout, stderr in ((self.VALID_RECORD, self.VALID_RECORD),
                               ("[info] " + self.VALID_RECORD, ""),
                               (self.VALID_RECORD + "\nlog " + self.VALID_RECORD, ""),
                               ("", self.VALID_RECORD)):
            with self.subTest(stdout=stdout, stderr=stderr), self.assertRaises(ValueError):
                parse_terminal_streams(stdout, stderr, INCLUDED)

    def test_run_command_and_environment_are_the_stable_v1_contract(self) -> None:
        from collect_lifecycle import run_engine
        root = self.repo / "package"
        root.mkdir(exist_ok=True)
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        completed = subprocess.CompletedProcess([], 0, self.VALID_RECORD, "")
        digest = hashlib.sha256(pe_image()).hexdigest()
        with mock.patch("collect_lifecycle.subprocess.run", return_value=completed) as run:
            captured, err = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest),
            )
        self.assertIsNone(err)
        self.assertEqual(captured.stdout, self.VALID_RECORD)
        cmd = run.call_args.args[0]
        self.assertEqual(cmd, [
            str(engine), "-game", str(module), "-require-game",
            "-test-seconds", "1.0", "-threads", "2", "-window-size", "640x360",
            "-no-subprocess",
        ])
        env = run.call_args.kwargs["env"]
        self.assertEqual(env["SPARK_RHI_BACKEND"], "d3d11")
        self.assertEqual(env["SPARK_D3D11_DRIVER"], "warp")
        self.assertEqual(run.call_args.kwargs["cwd"], str(root))

    def test_run_engine_keeps_image_leases_live_and_records_handle_metadata(self) -> None:
        """A child may start only while both authenticated image leases are held."""
        from collect_lifecycle import run_engine

        root = self.repo / "leased-package"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        engine_identity = FakeLeaseIdentity(11, 101, len(pe_image()))
        module_identity = FakeLeaseIdentity(11, 102, len(pe_image()))
        leases = [
            FakeImageLease(engine, identity=engine_identity, digest=digest),
            FakeImageLease(module, identity=module_identity, digest=digest),
            FakeImageLease(engine, identity=engine_identity, digest=digest),
            FakeImageLease(module, identity=module_identity, digest=digest),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        def child(*args, **kwargs):
            self.assertEqual(len(opened), 2)
            self.assertFalse(opened[0].closed)
            self.assertFalse(opened[1].closed)
            return subprocess.CompletedProcess([], 0, self.VALID_RECORD, "")

        with mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.subprocess.run", side_effect=child):
            captured, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIsNone(error)
        self.assertEqual([lease.path for lease in opened],
                         [engine, module, engine, module])
        self.assertTrue(all(lease.closed for lease in opened))
        self.assertEqual(captured.engine_image.digest, digest)
        self.assertEqual(captured.engine_image.final_path, str(engine))
        self.assertEqual(captured.module_image.digest, digest)
        self.assertEqual(captured.module_image.final_path, str(module))

    def test_run_engine_closes_held_leases_when_child_launch_fails(self) -> None:
        """A launch error cannot leak the write/delete-excluding image handles."""
        from collect_lifecycle import run_engine

        root = self.repo / "lease-launch-failure"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        leases = [
            FakeImageLease(engine, identity=FakeLeaseIdentity(12, 201, len(pe_image())), digest=digest),
            FakeImageLease(module, identity=FakeLeaseIdentity(12, 202, len(pe_image())), digest=digest),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        with mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.subprocess.run", side_effect=OSError("launch denied")):
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIn("cannot launch engine", error or "")
        self.assertEqual(len(opened), 2)
        self.assertTrue(all(lease.closed for lease in opened))

    def test_run_engine_closes_first_lease_when_second_lease_acquisition_fails(self) -> None:
        """A partially acquired image pair never leaves the first Windows handle open."""
        from collect_lifecycle import run_engine

        root = self.repo / "lease-open-failure"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        first = FakeImageLease(
            engine, identity=FakeLeaseIdentity(12, 203, len(pe_image())), digest=digest,
        )
        calls = 0

        def factory(path: Path) -> FakeImageLease:
            nonlocal calls
            calls += 1
            if calls == 1:
                self.assertEqual(path, engine)
                return first
            raise OSError("second image lease denied")

        with mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.subprocess.run") as child:
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIn("second image lease denied", error or "")
        child.assert_not_called()
        self.assertTrue(first.closed)

    def test_run_engine_rejects_post_run_lease_identity_change_and_closes_every_lease(self) -> None:
        """A pathname that changes identity after launch cannot produce evidence."""
        from collect_lifecycle import run_engine

        root = self.repo / "lease-identity-mutation"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        engine_identity = FakeLeaseIdentity(13, 301, len(pe_image()))
        module_identity = FakeLeaseIdentity(13, 302, len(pe_image()))
        leases = [
            FakeImageLease(engine, identity=engine_identity, digest=digest),
            FakeImageLease(module, identity=module_identity, digest=digest),
            FakeImageLease(engine, identity=FakeLeaseIdentity(13, 399, len(pe_image())), digest=digest),
            FakeImageLease(module, identity=module_identity, digest=digest),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        with mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.subprocess.run", return_value=subprocess.CompletedProcess(
                 [], 0, self.VALID_RECORD, "",
             )):
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIn("changed", error or "")
        self.assertEqual(len(opened), 4)
        self.assertTrue(all(lease.closed for lease in opened))

    def test_run_engine_rejects_engine_module_identity_alias_before_launch(self) -> None:
        """The executable and DLL cannot be two names for one hard-linked image."""
        from collect_lifecycle import run_engine

        root = self.repo / "lease-identity-alias"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        shared = FakeLeaseIdentity(14, 401, len(pe_image()))
        leases = [
            FakeImageLease(engine, identity=shared, digest=digest),
            FakeImageLease(module, identity=shared, digest=digest),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        with mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.subprocess.run") as child:
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIn("same file identity", error or "")
        child.assert_not_called()
        self.assertEqual(len(opened), 2)
        self.assertTrue(all(lease.closed for lease in opened))

    def test_run_engine_rejects_handle_final_path_alias_before_launch(self) -> None:
        """A path resolved from a handle must still equal the requested image path."""
        from collect_lifecycle import run_engine

        root = self.repo / "lease-final-path-alias"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        leases = [
            FakeImageLease(
                engine, identity=FakeLeaseIdentity(16, 601, len(pe_image())),
                digest=digest, final_path=str(root / "other" / "SparkEngine.exe"),
            ),
            FakeImageLease(
                module, identity=FakeLeaseIdentity(16, 602, len(pe_image())), digest=digest,
            ),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        with mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.subprocess.run") as child:
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIn("handle path", error or "")
        child.assert_not_called()
        self.assertTrue(all(lease.closed for lease in opened))

    def test_run_engine_fails_closed_without_native_lease_support(self) -> None:
        """Windows collection never falls back to path-only hashing."""
        from collect_lifecycle import run_engine

        root = self.repo / "no-native-lease"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()

        with mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.WINDOWS_IMAGE_LEASE_AVAILABLE", False), \
             mock.patch("collect_lifecycle.subprocess.run") as child:
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest),
            )

        self.assertIn("native Windows image leases", error or "")
        child.assert_not_called()

    def test_resolve_collection_sha_rejects_environment_commit_disagreement(self) -> None:
        """A CI environment SHA cannot silently overrule the checked-out revision."""
        from collect_lifecycle import resolve_collection_sha

        with mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch.dict("collect_lifecycle.os.environ", {"GITHUB_SHA": "f" * 40}, clear=False):
            sha, error = resolve_collection_sha(self.sha)

        self.assertIsNone(sha)
        self.assertIn("GITHUB_SHA", error or "")

    def test_main_rejects_noncanonical_manifest_or_commit_before_launch(self) -> None:
        """Only the fixed artifact-root manifest for checkout HEAD can authorize a run."""
        import collect_lifecycle

        root = self.repo / "fixed-manifest-package"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        manifest = write_image_manifest(root, engine, module, self.sha)
        wrong_name = root / "image-manifest.json"
        wrong_name.write_text(manifest.read_text(encoding="utf-8"), encoding="utf-8")
        outside = self.repo / "outside-image-manifest"
        outside.mkdir()
        outside_manifest = outside / "module-lifecycle-images.json"
        outside_manifest.write_text(manifest.read_text(encoding="utf-8"), encoding="utf-8")
        cases = (
            ("wrong-name", wrong_name, self.sha, self.sha),
            ("lexical-traversal", root / "nested" / ".." / manifest.name,
             self.sha, self.sha),
            ("case-alias", str(root / "MODULE-LIFECYCLE-IMAGES.JSON"),
             self.sha, self.sha),
            ("ads-alias", str(manifest) + ":alternate", self.sha, self.sha),
            ("outside-root", outside_manifest, self.sha, self.sha),
            ("commit-mismatch", manifest, self.sha, "f" * 40),
        )

        for name, image_manifest, requested_sha, head_sha in cases:
            with self.subTest(case=name):
                out = self.repo / "build" / "module-evidence" / "module-lifecycle.json"
                out.parent.mkdir(parents=True, exist_ok=True)
                for artifact in (out, out.parent / "module-lifecycle-SparkGameFPS.log"):
                    if artifact.exists():
                        artifact.unlink()
                argv = [
                    "collect_lifecycle.py", "--engine", str(engine), "--module", INCLUDED,
                    "--module-image", str(module), "--working-directory", str(root),
                    "--rhi-backend", "d3d11", "--image-manifest", str(image_manifest),
                    "--out", str(out), "--commit-sha", requested_sha,
                ]
                with mock.patch.object(sys, "argv", argv), \
                     mock.patch("collect_lifecycle.os.name", "nt"), \
                     mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
                     mock.patch("collect_lifecycle.resolve_head_sha", return_value=(head_sha, None)), \
                     mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                                return_value=("c" * 40, None)), \
                     mock.patch("collect_lifecycle.run_engine") as child:
                    self.assertEqual(collect_lifecycle.main(), 1)
                child.assert_not_called()
                self.assertFalse(out.exists())

    @unittest.skipUnless(os.name == "nt", "native Windows image leases are unavailable")
    def test_native_image_lease_requests_read_only_nondelete_share_mode(self) -> None:
        """The native lease denies new write and delete opens for the image epoch."""
        import collect_lifecycle

        if not collect_lifecycle.WINDOWS_IMAGE_LEASE_AVAILABLE:
            self.skipTest("native Windows image lease binding is unavailable")
        root = self.repo / "native-lease-intent"
        root.mkdir()
        engine = root / "SparkEngine.exe"
        engine.write_bytes(pe_image())
        native_create = collect_lifecycle._CreateFileW
        native_set_information = collect_lifecycle._SetHandleInformation
        with mock.patch("collect_lifecycle._CreateFileW", wraps=native_create) as create, \
             mock.patch("collect_lifecycle._SetHandleInformation",
                        wraps=native_set_information) as set_information:
            lease = collect_lifecycle.open_image_lease(engine)
        try:
            args = create.call_args.args
            self.assertEqual(args[1], collect_lifecycle._GENERIC_READ |
                             collect_lifecycle._FILE_READ_ATTRIBUTES)
            self.assertEqual(args[2], collect_lifecycle._FILE_SHARE_READ)
            self.assertEqual(args[4], collect_lifecycle._OPEN_EXISTING)
            required_flags = (collect_lifecycle._FILE_ATTRIBUTE_NORMAL |
                              collect_lifecycle._FILE_FLAG_OPEN_REPARSE_POINT)
            self.assertEqual(args[5] & required_flags, required_flags)
            self.assertEqual(
                set_information.call_args.args[1:],
                (collect_lifecycle._HANDLE_FLAG_INHERIT, 0),
            )
            self.assertEqual(lease.sha256(), hashlib.sha256(pe_image()).hexdigest())
            with self.assertRaises(OSError):
                engine.write_bytes(pe_image())
        finally:
            lease.close()

    def test_rejects_outside_script_and_reparse_images(self) -> None:
        from collect_lifecycle import validate_image_pair
        root = self.repo / "package"
        root.mkdir(exist_ok=True)
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        outside = self.repo / "SparkGameFPS.dll"
        outside.write_bytes(pe_image())
        self.assertIsNotNone(validate_image_pair(engine, outside, INCLUDED, root))
        script = root / "SparkGameFPS.cmd"
        script.write_text("@echo off\n", encoding="utf-8")
        self.assertIsNotNone(validate_image_pair(engine, script, INCLUDED, root))
        link = root / "linked.dll"
        try:
            os.symlink(str(module), str(link))
        except (OSError, NotImplementedError):
            self.skipTest("cannot create symlinks on this platform")
        self.assertIsNotNone(validate_image_pair(engine, link, INCLUDED, root))

    def test_main_writes_no_json_or_log_after_failed_or_invalid_run(self) -> None:
        import collect_lifecycle

        out, argv = self._collector_main_fixture("failed-or-invalid-run")
        engine = Path(argv[argv.index("--engine") + 1])
        module = Path(argv[argv.index("--module-image") + 1])
        engine_image = collect_lifecycle.ImageVerification(
            hashlib.sha256(engine.read_bytes()).hexdigest(), str(engine),
            collect_lifecycle.ImageIdentity(41, 1001, engine.stat().st_size, 1),
        )
        module_image = collect_lifecycle.ImageVerification(
            hashlib.sha256(module.read_bytes()).hexdigest(), str(module),
            collect_lifecycle.ImageIdentity(41, 1002, module.stat().st_size, 1),
        )
        cases = (
            ("failed-engine", collect_lifecycle.EngineOutput(
                "bad output", "failure", engine_image, module_image,
            ), "injected non-zero engine exit"),
            ("malformed-stream", collect_lifecycle.EngineOutput(
                "malformed output", "", engine_image, module_image,
            ), None),
        )
        for name, captured, error in cases:
            with self.subTest(case=name):
                self.assertEqual(self._run_truthful_collector_main(
                    argv,
                    mock.patch("collect_lifecycle.run_engine",
                               return_value=(captured, error)),
                ), 1)
            self.assertFalse(out.exists())
            self.assertFalse((out.parent / "module-lifecycle-SparkGameFPS.log").exists())

    def test_module_digest_and_path_are_required_and_valid(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        for key, value in (("moduleSHA256", None), ("modulePath", ""),
                           ("moduleSHA256", "not-a-digest")):
            candidate = copy.deepcopy(ev)
            if value is None:
                del candidate["records"][0][key]
            else:
                candidate["records"][0][key] = value
            errors = self.assertRejected(base_manifest(), f"collector-{key}",
                                         lifecycle_evidence=candidate)
            self.assertTrue(any(key in error or "missing" in error for error in errors))

    def test_run_engine_rejects_marker_printer_and_nonstable_engine_identity(self) -> None:
        from collect_lifecycle import run_engine
        root = self.repo / "authenticated-package"
        root.mkdir()
        module = root / "SparkGameFPS.dll"
        module.write_bytes(pe_image())
        for name, content in (("SparkEngine.exe", b"MZ" + b"\0" * 8192),
                              ("SparkConsole.exe", pe_image())):
            engine = root / name
            engine.write_bytes(content)
            expected = (
                hashlib.sha256(content).hexdigest(),
                hashlib.sha256(pe_image()).hexdigest(),
            )
            with self.subTest(engine=name), \
                  mock.patch("collect_lifecycle.subprocess.run") as run:
                _, error = run_engine(
                    engine, module, INCLUDED, root, "d3d11", 30,
                    expected_digests=expected,
                )
            self.assertIsNotNone(error)
            run.assert_not_called()

    def test_main_requires_trusted_manifest_and_rejects_digest_mismatch_before_launch(self) -> None:
        import collect_lifecycle
        root = self.repo / "manifest-package"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        out = self.repo / "build" / "module-evidence" / "module-lifecycle.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        bad_manifest = write_image_manifest(root, engine, module, self.sha,
                                            engine_digest="0" * 64)
        manifest_bytes = bad_manifest.read_bytes()
        lease = FakeImageLease(
            bad_manifest,
            identity=FakeLeaseIdentity(42, 1101, len(manifest_bytes)),
            digest=hashlib.sha256(manifest_bytes).hexdigest(),
            image=manifest_bytes,
        )
        image_digest = hashlib.sha256(pe_image()).hexdigest()
        engine_lease = FakeImageLease(
            engine,
            identity=FakeLeaseIdentity(42, 1102, len(pe_image())),
            digest=image_digest,
        )
        module_lease = FakeImageLease(
            module,
            identity=FakeLeaseIdentity(42, 1103, len(pe_image())),
            digest=image_digest,
        )
        opened: list[FakeImageLease] = []

        def lease_factory(path: Path) -> FakeImageLease:
            choices = {
                bad_manifest: lease,
                engine: engine_lease,
                module: module_lease,
            }
            selected = choices[path]
            opened.append(selected)
            return selected

        argv = ["collect_lifecycle.py", "--engine", str(engine), "--module", INCLUDED,
                "--module-image", str(module), "--working-directory", str(root),
                "--rhi-backend", "d3d11", "--image-manifest", str(bad_manifest),
                "--out", str(out), "--commit-sha", self.sha]
        with mock.patch.object(sys, "argv", argv), \
             mock.patch("collect_lifecycle.os.name", "nt"), \
             mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
             mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch.dict("collect_lifecycle.os.environ", {"GITHUB_SHA": self.sha}, clear=False), \
             mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                        return_value=("c" * 40, None)), \
             mock.patch("collect_lifecycle.open_image_lease", side_effect=lease_factory), \
             mock.patch("collect_lifecycle.subprocess.run") as child:
             self.assertEqual(collect_lifecycle.main(), 1)
        child.assert_not_called()
        self.assertEqual(opened, [lease, engine_lease, module_lease])
        self.assertTrue(all(item.closed for item in opened))
        self.assertFalse(out.exists())

    def test_run_engine_uses_canonical_paths_for_relative_arguments(self) -> None:
        from collect_lifecycle import run_engine
        root = self.repo / "relative-package"
        root.mkdir()
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        completed = subprocess.CompletedProcess([], 0, self.VALID_RECORD, "")
        previous = Path.cwd()
        try:
            os.chdir(self.repo)
            digest = hashlib.sha256(pe_image()).hexdigest()
            with mock.patch("collect_lifecycle.subprocess.run", return_value=completed) as run:
                _, error = run_engine(Path("relative-package/SparkEngine.exe"),
                                       Path("relative-package/SparkGameFPS.dll"),
                                       INCLUDED, Path("relative-package"), "d3d11", 30,
                                       expected_digests=(digest, digest))
        finally:
            os.chdir(previous)
        self.assertIsNone(error)
        self.assertEqual(run.call_args.args[0][0], str(engine.resolve()))
        self.assertEqual(run.call_args.args[0][2], str(module.resolve()))
        self.assertEqual(run.call_args.kwargs["cwd"], str(root.resolve()))

    def test_rejects_raw_root_and_intermediate_reparse_paths(self) -> None:
        from collect_lifecycle import validate_image_pair
        root = self.repo / "reparse-package"
        root.mkdir()
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        root_link = self.repo / "reparse-root-link"
        inner = root / "real-inner"
        inner.mkdir()
        (inner / "SparkEngine.exe").write_bytes(pe_image())
        (inner / "SparkGameFPS.dll").write_bytes(pe_image())
        inner_link = root / "inner-link"
        try:
            os.symlink(str(root), str(root_link), target_is_directory=True)
            os.symlink(str(inner), str(inner_link), target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("cannot create symlinks on this platform")
        self.assertIsNotNone(validate_image_pair(root_link / "SparkEngine.exe",
                                                  root_link / "SparkGameFPS.dll",
                                                  INCLUDED, root_link))
        self.assertIsNotNone(validate_image_pair(inner_link / "SparkEngine.exe",
                                                  inner_link / "SparkGameFPS.dll",
                                                  INCLUDED, root))

    def test_run_engine_rejects_image_mutation_after_launch(self) -> None:
        """A manifest digest that changes after launch is rejected through the leases."""
        from collect_lifecycle import run_engine
        root = self.repo / "mutation-package"
        root.mkdir()
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        engine_digest = "a" * 64
        module_digest = "b" * 64
        engine_identity = FakeLeaseIdentity(15, 501, len(pe_image()))
        module_identity = FakeLeaseIdentity(15, 502, len(pe_image()))
        leases = [
            FakeImageLease(engine, identity=engine_identity, digest=engine_digest),
            FakeImageLease(module, identity=module_identity, digest=module_digest),
            FakeImageLease(engine, identity=engine_identity, digest="c" * 64),
            FakeImageLease(module, identity=module_identity, digest=module_digest),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        completed = subprocess.CompletedProcess([], 0, self.VALID_RECORD, "")
        with mock.patch("collect_lifecycle.subprocess.run", return_value=completed), \
             mock.patch("collect_lifecycle.os.name", "nt"):
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(engine_digest, module_digest), lease_factory=factory,
            )
        self.assertIn("changed", error or "")
        self.assertTrue(all(lease.closed for lease in opened))

    def test_main_clears_stale_output_after_post_validation_run_or_write_failure(self) -> None:
        """Once all paths/manifests validate, later failures remove stale evidence."""
        out, argv = self._collector_main_fixture("stale-post-validation")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        failures = (
            ("run-engine", mock.patch(
                "collect_lifecycle.run_engine",
                return_value=(None, "injected post-validation engine failure"),
            )),
            ("write-temp", mock.patch(
                "collect_lifecycle._write_temp",
                side_effect=OSError("injected write failure"),
            )),
        )
        for name, failure in failures:
            out.write_text("stale-json", encoding="utf-8")
            log.write_text("stale-log", encoding="utf-8")
            with self.subTest(failure=name):
                self.assertEqual(self._run_truthful_collector_main(argv, failure), 1)
            self.assertFalse(out.exists())
            self.assertFalse(log.exists())

    def test_digest_rejects_trailing_newline(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["moduleSHA256"] = "b" * 64 + "\n"
        errors = self.assertRejected(base_manifest(), "digest-newline",
                                     lifecycle_evidence=ev)
        self.assertTrue(any("moduleSHA256" in error for error in errors))


# --------------------------------------------------------------------------
# B11-B17 — evidence bindings must name real producers
# --------------------------------------------------------------------------
class TestEvidenceBindings(FixtureCase):

    def test_B11_binding_to_a_never_produced_artifact_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][2]["artifactPattern"] = \
            "build/never-produced.xml"
        errors = self.assertRejected(m, "B11")
        self.assertTrue(any("producer" in e for e in errors), errors)

    def test_B12_included_module_with_no_bindings_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"] = []
        self.assertRejected(m, "B12")

    def test_B12b_each_required_evidence_type_is_individually_required(self) -> None:
        for i in range(4):
            with self.subTest(dropped=i):
                m = base_manifest()
                del m["modules"][0]["evidenceBindings"][i]
                self.assertRejected(m, f"B12b-{i}")

    def test_B13_included_module_without_package_smoke_owner_is_rejected(self) -> None:
        m = base_manifest()
        del m["modules"][0]["packageSmokeOwner"]
        self.assertRejected(m, "B13")

    def test_B14_invented_package_smoke_owner_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["packageSmokeOwner"] = "TOTALLY-FAKE-999"
        self.assertRejected(m, "B14")

    def test_B14b_well_formed_but_unknown_owner_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["packageSmokeOwner"] = "ZZZ-999"
        errors = self.assertRejected(m, "B14b")
        self.assertTrue(any("not a readiness work item" in e for e in errors), errors)

    def test_B15_currently_unproduced_lifecycle_log_path_is_rejected(self) -> None:
        """The historical binding to build/module-lifecycle-<mod>.log."""
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][1]["artifactPattern"] = \
            f"build/module-lifecycle-{INCLUDED}.log"
        self.assertRejected(m, "B15")

    def test_B16_binding_with_unknown_keys_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][0]["backdoor"] = "accepted"
        self.assertRejected(m, "B16")

    def test_B17_duplicate_binding_type_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"].append(
            copy.deepcopy(m["modules"][0]["evidenceBindings"][0]))
        self.assertRejected(m, "B17")

    def test_unknown_evidence_type_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][0]["type"] = "vibes"
        self.assertRejected(m, "B11b")

    def test_glob_artifact_pattern_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][0]["artifactPattern"] = "build/*.json"
        self.assertRejected(m, "B11c")

    def test_traversal_artifact_pattern_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][0]["artifactPattern"] = \
            "build/../../etc/shadow"
        self.assertRejected(m, "B11d")

    def test_every_evidence_type_names_a_producer(self) -> None:
        for etype, producer in EVIDENCE_PRODUCERS.items():
            with self.subTest(type=etype):
                for key in ("producer", "definedIn", "artifact", "ciJob"):
                    self.assertTrue(producer.get(key),
                                    f"{etype} declares no {key}")


# --------------------------------------------------------------------------
# B18-B21 — strict, duplicate-aware, bounded JSON
# --------------------------------------------------------------------------
class TestStrictJSON(unittest.TestCase):

    def test_B18_duplicate_property_is_rejected(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"schemaVersion":"evil","schemaVersion":"stable-v2"}')

    def test_B18b_duplicate_property_in_nested_object_is_rejected(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"a":{"b":1,"b":2}}')

    def test_B18c_duplicate_property_inside_array_element_is_rejected(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"a":[{"b":1,"b":2}]}')

    def test_B19_nan_and_infinity_literals_are_rejected(self) -> None:
        for literal in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(literal=literal):
                with self.assertRaises(strict_json.StrictJSONError):
                    strict_json.loads('{"a":%s}' % literal)

    def test_B19b_overflowing_float_is_rejected(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"a":1e400}')

    def test_depth_limit_is_enforced(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads("[" * 40 + "]" * 40)

    def test_document_size_limit_is_enforced(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"a":"' + "x" * (600 * 1024) + '"}')

    def test_container_item_limit_is_enforced(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads(
                json.dumps(
                    {"a": list(range(strict_json.DEFAULT_LIMITS.container_items + 1))}
                )
            )

    def test_module_target_container_item_limit_is_enforced(self) -> None:
        """The File API exception remains bounded for hostile artifacts."""
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads(
                json.dumps(
                    {
                        "sources": list(
                            range(strict_json.MODULE_TARGET_LIMITS.container_items + 1)
                        )
                    }
                ),
                limits=strict_json.MODULE_TARGET_LIMITS,
            )

    def test_string_length_limit_is_enforced(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads(json.dumps({"a": "x" * 600}))

    def test_node_count_limit_is_enforced(self) -> None:
        limits = strict_json.Limits(total_nodes=10)
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads(json.dumps({"a": list(range(50))}), limits=limits)

    def test_valid_document_is_accepted(self) -> None:
        self.assertEqual(strict_json.loads('{"a":[1,2,{"b":"x"}]}'),
                         {"a": [1, 2, {"b": "x"}]})

    def test_contract_limits_still_reject_duplicates(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"a":1,"a":2}', limits=strict_json.CONTRACT_LIMITS)

    def test_readiness_contract_parses_under_contract_limits(self) -> None:
        """The relaxed limits must actually admit the real contract."""
        document = strict_json.load_file(
            REPO_ROOT / "docs" / "site" / "readiness.json",
            limits=strict_json.CONTRACT_LIMITS,
        )
        self.assertIn("releaseProfiles", document)


class TestNestedUnknownKeys(FixtureCase):

    def test_B20_profile_with_unknown_keys_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["backdoor"] = "accepted"
        self.assertRejected(m, "B20")

    def test_B21_experimental_separation_unknown_keys_are_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][1]["experimentalSeparation"]["backdoor"] = "accepted"
        self.assertRejected(m, "B21")

    def test_top_level_unknown_keys_are_rejected(self) -> None:
        m = base_manifest()
        m["backdoor"] = "accepted"
        self.assertRejected(m, "B20b")

    def test_module_unknown_keys_are_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["backdoor"] = "accepted"
        self.assertRejected(m, "B20c")

    def test_self_attested_revision_keys_are_rejected(self) -> None:
        """A committed manifest may not claim its own commit or time."""
        for key, value in (("commitSHA", "a" * 40),
                           ("generatedAt", "2026-08-28T00:00:00Z")):
            with self.subTest(key=key):
                m = base_manifest()
                m[key] = value
                self.assertRejected(m, f"B33-{key}")

    def test_unknown_shared_library_platform_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["sharedLibrary"]["haiku"] = "libX.so"
        self.assertRejected(m, "B20d")


# --------------------------------------------------------------------------
# B22-B27 — the profile partition
# --------------------------------------------------------------------------
class TestProfilePartition(FixtureCase):

    def test_B22_unclassified_module_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["profiles"][0]["excludedModules"] = []
        errors = self.assertRejected(m, "B22")
        self.assertTrue(any("neither included nor excluded" in e for e in errors))

    def test_B23_duplicate_in_included_list_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["includedModules"] = [INCLUDED, INCLUDED]
        self.assertRejected(m, "B23")

    def test_B23b_duplicate_in_excluded_list_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["profiles"][0]["excludedModules"] = [DECOY, DECOY]
        self.assertRejected(m, "B23b")

    def test_B24_empty_included_list_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["includedModules"] = []
        m["profiles"][0]["excludedModules"] = [INCLUDED]
        m["modules"][0]["profileApplicability"] = {PROFILE: "outside"}
        m["modules"][0]["experimentalSeparation"] = {"trackedUnder": "RDY-015"}
        errors = self.assertRejected(m, "B24")
        self.assertTrue(any("empty" in e for e in errors), errors)

    def test_B25_empty_profile_id_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["id"] = ""
        m["modules"][0]["profileApplicability"] = {"": "required"}
        self.assertRejected(m, "B25")

    def test_B25b_unknown_profile_id_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["id"] = "invented-profile"
        m["modules"][0]["profileApplicability"] = {"invented-profile": "required"}
        errors = self.assertRejected(m, "B25b")
        self.assertTrue(any("readiness.json" in e for e in errors), errors)

    def test_B26_extra_profile_applicability_key_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["profileApplicability"]["invented-profile"] = "required"
        self.assertRejected(m, "B26")

    def test_B27_missing_profile_applicability_key_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["profileApplicability"] = {"invented-profile": "required"}
        self.assertRejected(m, "B27")

    def test_empty_profile_applicability_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["profileApplicability"] = {}
        self.assertRejected(m, "B27b")

    def test_module_in_both_lists_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["excludedModules"] = [INCLUDED]
        self.assertRejected(m, "B22b")

    def test_reference_to_undeclared_module_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["excludedModules"] = ["SparkGamePhantom"]
        self.assertRejected(m, "B22c")

    def test_duplicate_profile_ids_are_rejected(self) -> None:
        m = base_manifest()
        m["profiles"].append(copy.deepcopy(m["profiles"][0]))
        self.assertRejected(m, "B25c")

    def test_no_profiles_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"] = []
        self.assertRejected(m, "B24b")

    def test_included_module_declaring_itself_outside_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["profileApplicability"] = {PROFILE: "outside"}
        self.assertRejected(m, "B22d")

    def test_excluded_module_declaring_itself_required_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][1]["profileApplicability"] = {PROFILE: "required"}
        self.assertRejected(m, "B22e")


# --------------------------------------------------------------------------
# B28-B32 — cross-referenced identities
# --------------------------------------------------------------------------
class TestCrossReferences(FixtureCase):

    def test_B28_dependency_on_unknown_module_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["dependencies"] = ["NoSuchModule"]
        self.assertRejected(m, "B28")

    def test_B29_duplicate_dependencies_are_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][0]["dependencies"] = [DECOY, DECOY]
        self.assertRejected(m, "B29")

    def test_B29b_self_dependency_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["dependencies"] = [INCLUDED]
        self.assertRejected(m, "B29b")

    def test_B30_unrelated_dll_identity_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["sharedLibrary"]["windows"] = "Completely-Unrelated.dll"
        errors = self.assertRejected(m, "B30")
        self.assertTrue(any("may not claim a library identity" in e for e in errors))

    def test_B30b_wrong_platform_convention_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["sharedLibrary"]["linux"] = f"{INCLUDED}.so"
        self.assertRejected(m, "B30b")

    def test_B31_windows_case_colliding_libraries_are_rejected(self) -> None:
        """SparkGameFPS.dll and SparkGameFps.dll are one file on Windows.

        Both modules here are individually well formed — each name matches the
        naming rules and each library follows the platform convention — so
        only the collision check can catch them.  The assertion names that
        specific error rather than accepting any failure, because the
        case-variant directory does not exist on a case-insensitive host and
        would otherwise mask the collision behind a path error.
        """
        variant = "SparkGameFps"
        m = base_manifest(with_decoy=True)
        m["modules"][1]["name"] = variant
        m["modules"][1]["cmakeTarget"] = variant
        m["modules"][1]["sharedLibrary"] = {
            "windows": expected_library_names(variant)["windows"],
            "linux": expected_library_names(variant)["linux"],
        }
        m["modules"][1]["sourceDirectory"] = f"GameModules/{variant}/Source"
        m["profiles"][0]["excludedModules"] = [variant]
        errors = self.assertRejected(m, "B31")
        self.assertTrue(
            any("same file" in e for e in errors),
            f"[B31] case-colliding library identities were not detected: {errors}",
        )

    def test_B31b_case_colliding_module_names_are_rejected(self) -> None:
        variant = "SparkGameFps"
        m = base_manifest(with_decoy=True)
        m["modules"][1]["name"] = variant
        m["modules"][1]["cmakeTarget"] = variant
        m["modules"][1]["sharedLibrary"] = {
            "windows": expected_library_names(variant)["windows"],
            "linux": expected_library_names(variant)["linux"],
        }
        m["modules"][1]["sourceDirectory"] = f"GameModules/{variant}/Source"
        m["profiles"][0]["excludedModules"] = [variant]
        errors = self.assertRejected(m, "B31b")
        self.assertTrue(
            any("module name" in e and "collides" in e for e in errors),
            f"[B31b] case-colliding module names were not detected: {errors}",
        )

    def test_B32_invented_tracker_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][1]["experimentalSeparation"]["trackedUnder"] = "NOT-A-TRACKER-42"
        self.assertRejected(m, "B32")

    def test_B32b_well_formed_unknown_tracker_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][1]["experimentalSeparation"]["trackedUnder"] = "ZZZ-999"
        self.assertRejected(m, "B32b")

    def test_B32c_outside_module_without_tracker_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        del m["modules"][1]["experimentalSeparation"]
        self.assertRejected(m, "B32c")

    def test_cmake_target_differing_from_name_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["cmakeTarget"] = "SomethingElse"
        self.assertRejected(m, "B30c")

    def test_duplicate_module_names_are_rejected(self) -> None:
        m = base_manifest()
        m["modules"].append(copy.deepcopy(m["modules"][0]))
        self.assertRejected(m, "B31c")

    def test_experimental_module_required_in_profile_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][1]["profileApplicability"] = {PROFILE: "shared"}
        m["profiles"][0]["includedModules"] = [INCLUDED, DECOY]
        m["profiles"][0]["excludedModules"] = []
        self.assertRejected(m, "B32d")


# --------------------------------------------------------------------------
# B33-B36 — revision and time provenance
# --------------------------------------------------------------------------
class TestProvenance(FixtureCase):

    def test_B33_arbitrary_hex_sha_is_not_a_revision(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["commitSHA"] = "a" * 40
        errors = self.assertRejected(base_manifest(), "B33",
                                     lifecycle_evidence=ev, expected_sha="a" * 40)
        self.assertTrue(any("not an object in this repository" in e for e in errors),
                        errors)

    def test_B34_real_but_unrelated_sha_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["commitSHA"] = "0" * 39 + "1"
        self.assertRejected(base_manifest(), "B34", lifecycle_evidence=ev)

    def test_B34b_evidence_from_a_different_commit_is_rejected(self) -> None:
        """Shape-valid, object-valid, but not the revision under test."""
        (self.repo / "unrelated.txt").write_text("x", encoding="utf-8")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", "second")
        other = _git(self.repo, "rev-parse", "HEAD").stdout.strip()
        try:
            ev = lifecycle_evidence(self.repo, self.sha)
            ev["commitSHA"] = other
            errors = self.assertRejected(base_manifest(), "B34b",
                                         lifecycle_evidence=ev)
            self.assertTrue(any("stale evidence" in e for e in errors), errors)
        finally:
            _git(self.repo, "reset", "-q", "--hard", self.sha)

    def test_B35_null_generated_at_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["generatedAt"] = None
        errors = self.assertRejected(base_manifest(), "B35", lifecycle_evidence=ev)
        self.assertTrue(any("null" in e for e in errors), errors)

    def test_B36_non_timestamp_generated_at_is_rejected(self) -> None:
        for value in ("whenever", "2026-08-28", "2026-08-28T00:00:00",
                      "not-a-date", "", "2026-13-45T99:99:99Z"):
            with self.subTest(value=value):
                ev = lifecycle_evidence(self.repo, self.sha)
                ev["generatedAt"] = value
                self.assertRejected(base_manifest(), "B36",
                                    lifecycle_evidence=ev)

    def test_naive_timestamp_without_offset_is_rejected(self) -> None:
        self.assertNotEqual(
            provenance.check_rfc3339("2026-08-28T00:00:00", "t"), [])

    def test_far_future_timestamp_is_rejected(self) -> None:
        self.assertNotEqual(
            provenance.check_rfc3339("2099-01-01T00:00:00Z", "t"), [])

    def test_prehistoric_timestamp_is_rejected(self) -> None:
        self.assertNotEqual(
            provenance.check_rfc3339("1999-01-01T00:00:00Z", "t"), [])

    def test_valid_rfc3339_forms_are_accepted(self) -> None:
        for value in ("2026-08-28T07:01:00Z", "2026-08-28T07:01:00+00:00",
                      "2026-08-28T07:01:00.123Z", "2026-08-28T07:01:00-05:00"):
            with self.subTest(value=value):
                self.assertEqual(provenance.check_rfc3339(value, "t"), [])

    def test_missing_expected_sha_blocks(self) -> None:
        errors = self.assertRejected(base_manifest(), "B33b", expected_sha=None)
        self.assertTrue(any("revision under test" in e for e in errors), errors)

    def test_head_sha_resolves_in_a_real_checkout(self) -> None:
        sha, err = provenance.resolve_head_sha(self.repo)
        self.assertIsNone(err)
        self.assertEqual(sha, self.sha)

    def test_sha_shape_rejects_non_hex_and_short_values(self) -> None:
        for value in ("", "abc", "A" * 40, "g" * 40, None, 12345, "a" * 41):
            with self.subTest(value=value):
                self.assertNotEqual(provenance.check_sha_shape(value, "s"), [])


# --------------------------------------------------------------------------
# Registry integrity and the shipped contract
# --------------------------------------------------------------------------
class TestRegistryIntegrity(FixtureCase):

    def test_unreadable_profile_registry_blocks(self) -> None:
        (self.repo / "docs" / "site" / "readiness.json").unlink()
        try:
            errors = self.assertRejected(base_manifest(), "REG1")
            self.assertTrue(any("release profile registry unavailable" in e
                                for e in errors), errors)
        finally:
            (self.repo / "docs" / "site" / "readiness.json").write_text(
                json.dumps({"releaseProfiles": [{"id": PROFILE}]}), encoding="utf-8")

    def test_unreadable_work_item_registry_blocks(self) -> None:
        path = self.repo / "docs" / "readiness" / "work-items" / "00-truth.json"
        saved = path.read_text(encoding="utf-8")
        path.unlink()
        try:
            errors = self.assertRejected(base_manifest(), "REG2")
            self.assertTrue(any("work item registry unavailable" in e
                                for e in errors), errors)
        finally:
            path.write_text(saved, encoding="utf-8")

    def test_shipped_owner_and_tracker_exist_in_the_real_contract(self) -> None:
        from schema import load_known_work_item_ids
        ids, err = load_known_work_item_ids(REPO_ROOT)
        self.assertIsNone(err, f"real work item registry unreadable: {err}")
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json")
        for module in manifest["modules"]:
            owner = module.get("packageSmokeOwner")
            if owner is not None:
                self.assertIn(owner, ids, f"{module['name']} owner {owner}")
            sep = module.get("experimentalSeparation")
            if sep:
                self.assertIn(sep["trackedUnder"], ids)

    def test_shipped_profile_exists_in_the_real_contract(self) -> None:
        from schema import load_known_profile_ids
        ids, err = load_known_profile_ids(REPO_ROOT)
        self.assertIsNone(err)
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json")
        for profile in manifest["profiles"]:
            self.assertIn(profile["id"], ids)

    def test_shipped_source_directories_exist_with_exact_case(self) -> None:
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json")
        for module in manifest["modules"]:
            with self.subTest(module=module["name"]):
                self.assertEqual(
                    paths_mod.check_source_directory(
                        module["sourceDirectory"], module["name"], REPO_ROOT),
                    [])

    def test_manifest_is_not_an_object_is_rejected(self) -> None:
        self.assertNotEqual(ManifestValidator([], self.repo).validate(), [])

    def test_wrong_schema_version_is_rejected(self) -> None:
        m = base_manifest()
        m["schemaVersion"] = "stable-v1"
        self.assertRejected(m, "SV1")

    def test_module_count_and_collection_bounds(self) -> None:
        m = base_manifest()
        m["modules"] = [module_entry(INCLUDED, included=True)] * 200
        self.assertRejected(m, "BND1")


# --------------------------------------------------------------------------
# CMake File API — parsed from a real reply, and from a real configure
# --------------------------------------------------------------------------
class TestEvidenceGapLedger(FixtureCase):
    """The one softening in the validator, and the ratchet that bounds it."""

    def _ledger(self, gaps: list[dict[str, Any]]) -> Path:
        path = self.repo / "gaps.json"
        path.write_text(json.dumps(
            {"schemaVersion": "evidence-gaps-v1", "gaps": gaps}), encoding="utf-8")
        return path

    def _load(self, gaps: list[dict[str, Any]]) -> dict[str, str]:
        from validate_manifest import load_declared_gaps
        return load_declared_gaps(self._ledger(gaps),
                                  {"RDY-010", "RDY-015", "MOD-310"})

    def _valid_gap(self, etype: str = "lifecycle-log") -> dict[str, Any]:
        return {
            "evidenceType": etype,
            "trackedUnder": "RDY-010",
            "reason": "x" * 60,
        }

    def test_declared_gap_downgrades_absence_to_a_warning(self) -> None:
        v = ManifestValidator(
            base_manifest(), self.repo,
            target_index=target_index(),
            lifecycle_evidence=None, lifecycle_error="absent",
            expected_sha=self.sha,
            declared_gaps={"lifecycle-log": "RDY-010"},
        )
        self.assertEqual(v.validate(), [])
        self.assertTrue(any("lifecycle-log" in w for w in v.warnings), v.warnings)

    def test_undeclared_gap_is_still_a_hard_failure(self) -> None:
        v = ManifestValidator(
            base_manifest(), self.repo,
            target_index=None, target_error="absent",
            lifecycle_evidence=lifecycle_evidence(self.repo, self.sha),
            expected_sha=self.sha,
            declared_gaps={"lifecycle-log": "RDY-010"},
        )
        self.assertNotEqual(v.validate(), [])

    def test_ratchet_declared_gap_whose_evidence_appeared_fails(self) -> None:
        """The ledger may only shrink."""
        v = ManifestValidator(
            base_manifest(), self.repo,
            target_index=target_index(),
            lifecycle_evidence=lifecycle_evidence(self.repo, self.sha),
            expected_sha=self.sha,
            declared_gaps={"lifecycle-log": "RDY-010"},
        )
        errors = v.validate()
        self.assertTrue(any("evidence is now available" in e for e in errors),
                        errors)

    def test_missing_artifact_evidence_is_rejected(self) -> None:
        artifact = self.repo / EVIDENCE_PRODUCERS["junit-xml"]["artifact"]
        saved = artifact.read_text(encoding="utf-8")
        artifact.unlink()
        try:
            errors = self.assertRejected(base_manifest(), "ART1")
            self.assertTrue(any("was not produced" in e for e in errors), errors)
        finally:
            artifact.write_text(saved, encoding="utf-8")

    def test_ledger_rejects_unknown_evidence_type(self) -> None:
        from validate_manifest import ManifestError
        with self.assertRaises(ManifestError):
            self._load([{**self._valid_gap(), "evidenceType": "vibes"}])

    def test_ledger_rejects_untracked_gap(self) -> None:
        from validate_manifest import ManifestError
        with self.assertRaises(ManifestError):
            self._load([{**self._valid_gap(), "trackedUnder": "ZZZ-999"}])

    def test_ledger_rejects_malformed_tracker(self) -> None:
        from validate_manifest import ManifestError
        with self.assertRaises(ManifestError):
            self._load([{**self._valid_gap(), "trackedUnder": "whatever"}])

    def test_ledger_rejects_unexplained_gap(self) -> None:
        from validate_manifest import ManifestError
        with self.assertRaises(ManifestError):
            self._load([{**self._valid_gap(), "reason": "because"}])

    def test_ledger_rejects_duplicate_and_unknown_keys(self) -> None:
        from validate_manifest import ManifestError
        with self.assertRaises(ManifestError):
            self._load([self._valid_gap(), self._valid_gap()])
        with self.assertRaises(ManifestError):
            self._load([{**self._valid_gap(), "backdoor": "accepted"}])

    def test_ledger_rejects_wrong_schema_version(self) -> None:
        from validate_manifest import ManifestError, load_declared_gaps
        path = self.repo / "bad-gaps.json"
        path.write_text(json.dumps({"schemaVersion": "v9", "gaps": []}),
                        encoding="utf-8")
        with self.assertRaises(ManifestError):
            load_declared_gaps(path, {"RDY-010"})

    def test_valid_ledger_loads(self) -> None:
        self.assertEqual(self._load([self._valid_gap()]),
                         {"lifecycle-log": "RDY-010"})

    def test_shipped_ledger_is_valid_and_nonempty(self) -> None:
        from schema import load_known_work_item_ids
        from validate_manifest import load_declared_gaps
        ids, err = load_known_work_item_ids(REPO_ROOT)
        self.assertIsNone(err)
        gaps = load_declared_gaps(
            REPO_ROOT / "tools" / "module-evidence" / "evidence-gaps.json", ids)
        self.assertNotEqual(gaps, {},
                            "an empty ledger would mean RDY-010 is closeable")
        self.assertIn("lifecycle-log", gaps)


class TestCIWiring(unittest.TestCase):
    """The gate is only a gate if CI actually runs it, blockingly.

    Parsed as text rather than YAML so the check needs no dependency the CI
    runner does not already have.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(
            encoding="utf-8")

    def _job_block(self, name: str) -> str:
        start = self.workflow.index(f"\n  {name}:\n")
        rest = self.workflow[start + 1:]
        lines = rest.split("\n")
        out = [lines[0]]
        for line in lines[1:]:
            if line and not line.startswith("   ") and not line.startswith("  #"):
                if line.startswith("  ") and line.rstrip().endswith(":"):
                    break
            out.append(line)
        return "\n".join(out)

    def test_module_evidence_job_exists(self) -> None:
        self.assertIn("\n  module-evidence:\n", self.workflow)

    def test_module_evidence_is_a_required_gate(self) -> None:
        gate = self.workflow[self.workflow.index("\n  required-ci-gate:\n"):]
        needs = gate[gate.index("needs:"):gate.index("runs-on:")]
        self.assertIn("- module-evidence", needs,
                      "module-evidence is not in required-ci-gate needs")

    def test_module_evidence_is_not_continue_on_error(self) -> None:
        self.assertNotIn("continue-on-error", self._job_block("module-evidence"))

    def test_module_evidence_runs_a_real_configure(self) -> None:
        block = self._job_block("module-evidence")
        self.assertIn("collect_targets.py", block)
        self.assertIn("-DBUILD_GAME_MODULES=ON", block)

    def test_module_evidence_gate_is_not_policy_only(self) -> None:
        """A --policy-only run must never be the release gate."""
        block = self._job_block("module-evidence")
        self.assertIn("--allow-declared-gaps", block)
        self.assertNotIn("--policy-only", block)

    def test_module_evidence_binds_the_revision_under_test(self) -> None:
        self.assertIn("--expected-sha", self._job_block("module-evidence"))

    def test_gate_consumes_really_produced_junit_evidence(self) -> None:
        block = self._job_block("module-evidence")
        self.assertIn("download-artifact", block)
        self.assertIn("test-results-linux-gcc-Release", block)

    def test_every_gap_tracker_is_a_real_work_item(self) -> None:
        from schema import load_known_work_item_ids
        ids, err = load_known_work_item_ids(REPO_ROOT)
        self.assertIsNone(err)
        gaps = json.loads(
            (REPO_ROOT / "tools" / "module-evidence" / "evidence-gaps.json")
            .read_text(encoding="utf-8"))["gaps"]
        for gap in gaps:
            with self.subTest(gap=gap["evidenceType"]):
                self.assertIn(gap["trackedUnder"], ids)

    def test_action_pins_are_shared_with_the_rest_of_the_workflow(self) -> None:
        """A pin used once is a pin nobody reviewed."""
        import re
        block = self._job_block("module-evidence")
        for action, pin in re.findall(r"uses: (actions/[\w-]+)@([a-f0-9]{40})", block):
            with self.subTest(action=action):
                self.assertGreaterEqual(
                    self.workflow.count(f"{action}@{pin}"), 2,
                    f"{action}@{pin} appears only in the module-evidence job; "
                    f"reuse the pin the rest of the workflow already uses",
                )


class TestTargetContainment(FixtureCase):
    """Adversarial tests for target evidence containment, provenance, and
    strict JSON parsing — attacks exposed by rdy010_target_adversarial_probe.py."""

    def _write_reply(self, reply: Path, *, codemodel_json_file: str = "codemodel-v2-abc.json",
                     target_json_file: str = "target-X.json",
                     target_content: str | None = None,
                     codemodel_content: str | None = None,
                     include_shared_fallback: bool = False) -> None:
        """Build a minimal File API reply directory."""
        if target_content is None:
            target_content = json.dumps({
                "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                "nameOnDisk": "libSparkGameFPS.so",
                "paths": {"source": "GameModules/SparkGameFPS"},
                "sources": [{"path": "GameModules/SparkGameFPS/Source/Main.cpp"}],
                "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
            })
        if codemodel_content is None:
            codemodel_content = json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS", "jsonFile": target_json_file}],
                }]
            })
        reply.mkdir(parents=True, exist_ok=True)
        (reply / target_json_file).write_text(target_content, encoding="utf-8")
        (reply / codemodel_json_file).write_text(codemodel_content, encoding="utf-8")
        (reply / "index-1.json").write_text(json.dumps({
            "reply": {targets_mod.CLIENT_NAME: {"query.json": {
                "responses": [{"kind": "codemodel",
                               "jsonFile": codemodel_json_file}]
            }}}
        }), encoding="utf-8")
        if include_shared_fallback:
            (reply / "codemodel-v2-shared.json").write_text(
                codemodel_content, encoding="utf-8")

    def test_path_traversal_in_codemodel_jsonfile_is_rejected(self) -> None:
        """A jsonFile with ../ escaping the reply directory must be blocked."""
        with tempfile.TemporaryDirectory(prefix="spark-traversal-") as tmp:
            reply = Path(tmp) / "reply"
            reply.mkdir()
            forged = Path(tmp) / "forged-codemodel.json"
            forged.write_text(json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS", "jsonFile": "target-X.json"}],
                }]
            }), encoding="utf-8")
            (reply / "target-X.json").write_text(json.dumps({
                "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                "nameOnDisk": "libSparkGameFPS.so",
                "paths": {"source": "GameModules/SparkGameFPS"},
                "sources": [{"path": "GameModules/SparkGameFPS/Source/Main.cpp"}],
                "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
            }), encoding="utf-8")
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {targets_mod.CLIENT_NAME: {"query.json": {
                    "responses": [{"kind": "codemodel",
                                   "jsonFile": "../forged-codemodel.json"}]
                }}}
            }), encoding="utf-8")
            with self.assertRaises(targets_mod.TargetEvidenceUnavailable) as cm:
                targets_mod.extract_from_reply(reply)
            self.assertIn("path traversal", str(cm.exception).lower())

    def test_path_traversal_in_target_jsonfile_is_rejected(self) -> None:
        """A target jsonFile reference escaping the reply dir must be blocked."""
        with tempfile.TemporaryDirectory(prefix="spark-traversal-") as tmp:
            reply = Path(tmp) / "reply"
            reply.mkdir()
            forged_target = Path(tmp) / "forged-target.json"
            forged_target.write_text(json.dumps({
                "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                "nameOnDisk": "libSparkGameFPS.so",
                "paths": {"source": "GameModules/SparkGameFPS"},
                "sources": [{"path": "GameModules/SparkGameFPS/Source/Main.cpp"}],
                "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
            }), encoding="utf-8")
            codemodel = reply / "codemodel-v2-abc.json"
            codemodel.write_text(json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS",
                                 "jsonFile": "../forged-target.json"}],
                }]
            }), encoding="utf-8")
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {targets_mod.CLIENT_NAME: {"query.json": {
                    "responses": [{"kind": "codemodel",
                                   "jsonFile": "codemodel-v2-abc.json"}]
                }}}
            }), encoding="utf-8")
            with self.assertRaises(targets_mod.TargetEvidenceUnavailable) as cm:
                targets_mod.extract_from_reply(reply)
            self.assertIn("path traversal", str(cm.exception).lower())

    def test_duplicate_keys_in_target_json_are_rejected(self) -> None:
        """Duplicate JSON keys (type shadowing) must be caught by strict_json."""
        with tempfile.TemporaryDirectory(prefix="spark-dup-") as tmp:
            reply = Path(tmp)
            (reply / "target-X.json").write_text(
                '{"name":"SparkGameFPS","type":"EXECUTABLE",'
                '"type":"SHARED_LIBRARY","nameOnDisk":"libSparkGameFPS.so",'
                '"paths":{"source":"GameModules/SparkGameFPS"},'
                '"sources":[{"path":"GameModules/SparkGameFPS/Source/Main.cpp"}],'
                '"artifacts":[{"path":"bin/libSparkGameFPS.so"}]}',
                encoding="utf-8",
            )
            (reply / "codemodel-v2-abc.json").write_text(json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS", "jsonFile": "target-X.json"}],
                }]
            }), encoding="utf-8")
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {targets_mod.CLIENT_NAME: {"query.json": {
                    "responses": [{"kind": "codemodel",
                                   "jsonFile": "codemodel-v2-abc.json"}]
                }}}
            }), encoding="utf-8")
            with self.assertRaises((
                targets_mod.TargetEvidenceUnavailable,
                strict_json.StrictJSONError,
            )):
                targets_mod.extract_from_reply(reply)

    def test_cmake_target_with_more_than_512_sources_is_accepted(self) -> None:
        """Real CMake File API targets can legitimately exceed 512 sources."""
        source_count = 526
        target_content = json.dumps({
            "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
            "nameOnDisk": "libSparkGameFPS.so",
            "paths": {"source": "GameModules/SparkGameFPS"},
            "sources": [
                {"path": f"GameModules/SparkGameFPS/Source/Unit{index}.cpp"}
                for index in range(source_count)
            ],
            "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
        })
        with tempfile.TemporaryDirectory(prefix="spark-many-sources-") as tmp:
            reply = Path(tmp) / "reply"
            self._write_reply(reply, target_content=target_content)
            index = targets_mod.extract_from_reply(reply)
            target_index_path = Path(tmp) / "module-targets.json"
            targets_mod.write_index(
                index,
                target_index_path,
                commit_sha=self.sha,
                generated_at=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                source="test",
            )
            captured = targets_mod.load_target_index(target_index_path)
        self.assertEqual(len(captured["targets"]["SparkGameFPS"]["sources"]), source_count)

    def test_shared_reply_fallback_is_gone(self) -> None:
        """Without a client-specific reply, existence of a shared codemodel
        must not substitute — only our own client reply is authoritative."""
        with tempfile.TemporaryDirectory(prefix="spark-fallback-") as tmp:
            reply = Path(tmp)
            (reply / "codemodel-v2-shared.json").write_text(json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS", "jsonFile": "target-X.json"}],
                }]
            }), encoding="utf-8")
            (reply / "target-X.json").write_text(json.dumps({
                "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                "nameOnDisk": "libSparkGameFPS.so",
                "paths": {"source": "GameModules/SparkGameFPS"},
                "sources": [{"path": "GameModules/SparkGameFPS/Source/Main.cpp"}],
                "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
            }), encoding="utf-8")
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {"some-other-client": {"query.json": {
                    "responses": [{"kind": "codemodel",
                                   "jsonFile": "codemodel-v2-shared.json"}]
                }}}
            }), encoding="utf-8")
            with self.assertRaises(targets_mod.TargetEvidenceUnavailable) as cm:
                targets_mod.extract_from_reply(reply)
            self.assertIn("authoritative", str(cm.exception))

    def test_foreign_source_directory_in_codemodel_is_rejected(self) -> None:
        """A target claiming source in C:/not-the-repository must be caught."""
        foreign_index = target_index()
        foreign_index["targets"]["SparkGameFPS"]["sourceDirectory"] = "C:/not-the-repository/GameModules/SparkGameFPS"
        errors = targets_mod.check_target(
            foreign_index, "SparkGameFPS", "SparkGameFPS",
            {"windows": "SparkGameFPS.dll", "linux": "libSparkGameFPS.so"},
            "forged",
        )
        self.assertTrue(
            any("foreign source tree" in e for e in errors),
            f"foreign source directory was not rejected: {errors}",
        )

    def test_load_target_index_rejects_malformed_commit_sha(self) -> None:
        """Provenance with invalid commitSHA must be rejected."""
        from datetime import datetime, timezone
        with tempfile.TemporaryDirectory(prefix="spark-prov-") as tmp:
            path = Path(tmp) / "targets.json"
            targets_mod.write_index(
                {"SparkGameFPS": {"name": "SparkGameFPS", "type": "SHARED_LIBRARY"}},
                path,
                commit_sha="NOT-A-VALID-SHA",
                generated_at=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                source="test",
            )
            with self.assertRaises(targets_mod.TargetEvidenceUnavailable) as cm:
                targets_mod.load_target_index(path)
            self.assertIn("provenance", str(cm.exception).lower())

    def test_load_target_index_rejects_malformed_generated_at(self) -> None:
        """Provenance with invalid generatedAt must be rejected."""
        with tempfile.TemporaryDirectory(prefix="spark-prov-") as tmp:
            path = Path(tmp) / "targets.json"
            targets_mod.write_index(
                {"SparkGameFPS": {"name": "SparkGameFPS", "type": "SHARED_LIBRARY"}},
                path,
                commit_sha="a" * 40,
                generated_at="not-a-timestamp",
                source="test",
            )
            with self.assertRaises(targets_mod.TargetEvidenceUnavailable) as cm:
                targets_mod.load_target_index(path)
            self.assertIn("provenance", str(cm.exception).lower())

    def test_load_target_index_accepts_valid_provenance(self) -> None:
        """A well-formed document with valid provenance must load."""
        from datetime import datetime, timezone
        with tempfile.TemporaryDirectory(prefix="spark-prov-") as tmp:
            path = Path(tmp) / "targets.json"
            targets_mod.write_index(
                {"SparkGameFPS": {"name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                                  "sources": ["a.cpp"], "artifacts": ["a.so"]}},
                path,
                commit_sha=self.sha,
                generated_at=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                source="test",
            )
            doc = targets_mod.load_target_index(path)
            self.assertIn("SparkGameFPS", doc["targets"])


class TestCMakeFileAPI(unittest.TestCase):
    """The File API is the authority for target existence; prove it is read
    correctly, and that a configure actually discriminates."""

    def test_extract_from_reply_reads_a_recorded_codemodel(self) -> None:
        with tempfile.TemporaryDirectory(prefix="spark-reply-") as tmp:
            reply = Path(tmp)
            (reply / "target-X.json").write_text(json.dumps({
                "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                "nameOnDisk": "libSparkGameFPS.so",
                "paths": {"source": "GameModules/SparkGameFPS"},
                "sources": [{"path": "GameModules/SparkGameFPS/Source/Main.cpp"}],
                "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
            }), encoding="utf-8")
            (reply / "codemodel-v2-abc.json").write_text(json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS", "jsonFile": "target-X.json"}],
                }]
            }), encoding="utf-8")
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {targets_mod.CLIENT_NAME: {"query.json": {
                    "responses": [{"kind": "codemodel",
                                   "jsonFile": "codemodel-v2-abc.json"}]
                }}}
            }), encoding="utf-8")
            index = targets_mod.extract_from_reply(reply)
        self.assertEqual(index["SparkGameFPS"]["type"], "SHARED_LIBRARY")
        self.assertEqual(index["SparkGameFPS"]["sources"],
                         ["GameModules/SparkGameFPS/Source/Main.cpp"])

    def test_query_error_in_reply_raises(self) -> None:
        with tempfile.TemporaryDirectory(prefix="spark-reply-") as tmp:
            reply = Path(tmp)
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {targets_mod.CLIENT_NAME: {
                    "query.json": {"error": "unknown request kind"}}}
            }), encoding="utf-8")
            with self.assertRaises(targets_mod.TargetEvidenceUnavailable):
                targets_mod.extract_from_reply(reply)

    def test_undecorate_library_name_handles_every_toolchain(self) -> None:
        for filename, expected in (
            ("libSparkGameFPS.so", "SparkGameFPS"),
            ("libSparkGameFPS.dll", "SparkGameFPS"),
            ("SparkGameFPS.dll", "SparkGameFPS"),
            ("libSparkGameFPS.dylib", "SparkGameFPS"),
        ):
            with self.subTest(filename=filename):
                self.assertEqual(targets_mod.undecorate_library_name(filename),
                                 expected)

    @unittest.skipIf(shutil.which("cmake") is None, "cmake is not on PATH")
    def test_real_configure_discriminates_a_declared_target(self) -> None:
        """A configure must report the target only when it is really created.

        This is the property a regex over CMakeLists.txt cannot have: the
        control project declares its target through a helper function, so the
        string `add_library(` never appears in the file that declares it.
        """
        with tempfile.TemporaryDirectory(prefix="spark-cmake-") as tmp:
            src = Path(tmp) / "src"
            src.mkdir()
            (src / "a.cpp").write_text("int spark_probe() { return 0; }\n",
                                       encoding="utf-8")
            (src / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(SparkProbe CXX)\n"
                "function(spark_add_game_module name)\n"
                "  add_library(${name} SHARED ${ARGN})\n"
                "endfunction()\n"
                "option(MAKE_IT \"\" ON)\n"
                "if(MAKE_IT)\n"
                "  spark_add_game_module(ProbeModule a.cpp)\n"
                "endif()\n"
                "# add_library(GhostModule SHARED a.cpp)\n",
                encoding="utf-8",
            )
            text = (src / "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertNotIn("add_library(ProbeModule", text,
                             "fixture must not declare the target literally")
            self.assertIn("# add_library(GhostModule", text)

            for make_it, expect_target in ((True, True), (False, False)):
                build = Path(tmp) / f"build-{make_it}"
                build.mkdir()
                targets_mod.write_query(build)
                proc = subprocess.run(
                    ["cmake", "-S", str(src), "-B", str(build),
                     f"-DMAKE_IT={'ON' if make_it else 'OFF'}"],
                    capture_output=True, text=True, timeout=600, check=False,
                )
                if proc.returncode != 0:
                    self.skipTest(
                        f"cmake configure failed in this environment: "
                        f"{proc.stderr[-400:]}"
                    )
                reply = build / ".cmake" / "api" / "v1" / "reply"
                if expect_target:
                    index = targets_mod.extract_from_reply(reply)
                    self.assertIn("ProbeModule", index)
                    self.assertEqual(index["ProbeModule"]["type"], "SHARED_LIBRARY")
                    self.assertTrue(index["ProbeModule"]["sources"])
                    self.assertNotIn(
                        "GhostModule", index,
                        "a commented-out add_library must not yield a target",
                    )
                else:
                    index = targets_mod.extract_from_reply(reply)
                    self.assertNotIn(
                        "ProbeModule", index,
                        "target reported despite not being created",
                    )


# --------------------------------------------------------------------------
# Artifact semantic validation — the zero-byte bypass (rdy010_artifact_adversarial_probe)
# --------------------------------------------------------------------------
class TestArtifactSemanticValidation(FixtureCase):
    """Zero-byte and trivially fabricated artifacts must be rejected."""

    def _junit_xml(self, content: str) -> Path:
        path = self.repo / "build" / "test-junit.xml"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def _smoke_log(self, content: str) -> Path:
        path = self.repo / "build" / "test-smoke.log"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    VALID_JUNIT = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<testsuites tests="5" failures="0" errors="0">\n'
        '  <testsuite name="SparkGameFPS" tests="5" failures="0" errors="0">\n'
        '    <testcase name="test_load" classname="SparkGameFPS.Module"/>\n'
        '    <testcase name="test_init" classname="SparkGameFPS.Module"/>\n'
        '    <testcase name="test_update" classname="SparkGameFPS.Module"/>\n'
        '    <testcase name="test_unload" classname="SparkGameFPS.Module"/>\n'
        '    <testcase name="test_shutdown" classname="SparkGameFPS.Module"/>\n'
        '  </testsuite>\n'
        '</testsuites>\n'
    )
    VALID_SMOKE = (
        "[package-smoke] SparkGameFPS\n"
        "[package-smoke] product=SparkEngine\n"
        "[package-smoke] module=SparkGameFPS\n"
        "[package-smoke] library=SparkGameFPS.dll\n"
        "[package-smoke] exit_code=0\n"
        "[package-smoke] PASS\n"
    )

    def test_zero_byte_junit_xml_is_rejected(self) -> None:
        path = self._junit_xml("")
        path.write_bytes(b"")
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(any("zero bytes" in e for e in errors), errors)

    def test_valid_junit_xml_is_accepted(self) -> None:
        path = self._junit_xml(self.VALID_JUNIT)
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertEqual(errors, [], f"valid JUnit XML rejected: {errors}")

    def test_junit_xml_with_zero_tests_is_rejected(self) -> None:
        path = self._junit_xml(
            '<testsuites tests="0" failures="0" errors="0">'
            '</testsuites>'
        )
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(any("zero tests" in e for e in errors), errors)

    def test_junit_xml_with_no_testcases_is_rejected(self) -> None:
        path = self._junit_xml(
            '<testsuites tests="5" failures="0" errors="0">'
            '</testsuites>'
        )
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(any("no <testcase>" in e for e in errors), errors)

    def test_invalid_xml_is_rejected(self) -> None:
        path = self._junit_xml("this is not xml at all {{{")
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(
            any("not valid XML" in e or "cannot be parsed" in e for e in errors),
            errors,
        )

    def test_junit_xml_with_too_few_testcases_is_rejected(self) -> None:
        path = self._junit_xml(
            '<testsuites tests="1" failures="0" errors="0">'
            '  <testsuite name="X" tests="1">'
            '    <testcase name="t1" classname="X.Y"/>'
            '  </testsuite>'
            '</testsuites>'
        )
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(any("minimum" in e for e in errors), errors)

    def test_junit_xml_with_entity_declaration_is_rejected(self) -> None:
        path = self._junit_xml(
            '<?xml version="1.0"?>\n'
            '<!DOCTYPE foo [\n'
            '  <!ENTITY xxe "pwned">\n'
            ']>\n'
            '<testsuites tests="3" failures="0" errors="0">\n'
            '  <testsuite name="X" tests="3">\n'
            '    <testcase name="t1" classname="X.Y"/>\n'
            '    <testcase name="t2" classname="X.Y"/>\n'
            '    <testcase name="t3" classname="X.Y"/>\n'
            '  </testsuite>\n'
            '</testsuites>\n'
        )
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(
            any("entity" in e.lower() or "not valid XML" in e for e in errors),
            f"XXE entity declaration was accepted: {errors}",
        )

    def test_zero_byte_package_smoke_is_rejected(self) -> None:
        path = self._smoke_log("")
        path.write_bytes(b"")
        errors = artifacts.validate_package_smoke(path, INCLUDED)
        self.assertTrue(any("zero bytes" in e for e in errors), errors)

    def test_valid_package_smoke_is_accepted(self) -> None:
        path = self._smoke_log(self.VALID_SMOKE)
        errors = artifacts.validate_package_smoke(path, INCLUDED)
        self.assertEqual(errors, [], f"valid smoke log rejected: {errors}")

    def test_package_smoke_without_module_name_is_rejected(self) -> None:
        path = self._smoke_log(
            "[package-smoke] SomeOtherModule\n"
            "[package-smoke] product=SparkEngine\n"
            "[package-smoke] module=SomeOtherModule\n"
            "[package-smoke] exit_code=0\n"
            "[package-smoke] PASS\n"
        )
        errors = artifacts.validate_package_smoke(path, INCLUDED)
        self.assertTrue(any("does not mention" in e for e in errors), errors)

    def test_package_smoke_without_pass_indicator_is_rejected(self) -> None:
        path = self._smoke_log(
            f"[package-smoke] {INCLUDED}\n"
            f"[package-smoke] module={INCLUDED}\n"
            f"[package-smoke] library={INCLUDED}.dll\n"
            "[package-smoke] exit_code=1\n"
            "[package-smoke] FAIL\n"
        )
        errors = artifacts.validate_package_smoke(path, INCLUDED)
        self.assertTrue(any("pass indicator" in e for e in errors), errors)

    def test_whitespace_only_smoke_log_is_rejected(self) -> None:
        path = self._smoke_log("   \n  \n\n  \n")
        errors = artifacts.validate_package_smoke(path, INCLUDED)
        self.assertTrue(len(errors) > 0, "whitespace-only log accepted")

    def test_artifact_dispatcher_routes_correctly(self) -> None:
        path = self._junit_xml(self.VALID_JUNIT)
        self.assertEqual(artifacts.validate_artifact(path, "junit-xml", INCLUDED), [])
        self.assertEqual(
            artifacts.validate_artifact(path, "cmake-target-index", INCLUDED), [])

    def test_full_validator_rejects_zero_byte_artifacts(self) -> None:
        """The full ManifestValidator must reject zero-byte evidence files."""
        m = base_manifest()
        junit = self.repo / EVIDENCE_PRODUCERS["junit-xml"]["artifact"]
        junit.parent.mkdir(parents=True, exist_ok=True)
        junit.write_bytes(b"")
        smoke = self.repo / EVIDENCE_PRODUCERS["package-smoke-log"]["artifact"]
        smoke.parent.mkdir(parents=True, exist_ok=True)
        smoke.write_bytes(b"")
        errors = self.validate(m)
        self.assertTrue(
            any("zero bytes" in e for e in errors),
            f"zero-byte artifact was accepted by the full validator: {errors}",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
