#!/usr/bin/env python3
"""SEC-110: tools/generate-sbom.py SPDX generation and package reconciliation.

Generation runs against a fake repository that tools/check-supply-chain.py
passes (the fixture from Tests/test_check_supply_chain.py) plus the real
repository lock. Reconciliation runs against synthetic packages classified by
a small rule set with the real GOV-400 rule schema, and each negative case
changes exactly one thing from a package that reconciles cleanly: a forged
third-party payload, a component the lock does not know, a locked dependency
missing from the package, a stale ``--not-configured`` declaration, or a
notice inventory built from a different lock.

Run:  python3 -m unittest Tests.Tools.test_generate_sbom -v
      python3 Tests/Tools/test_generate_sbom.py
"""

from __future__ import annotations

import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

PROJECT_ROOT = Path(__file__).resolve().parents[2]


def _load(name: str, rel: str):
    spec = importlib.util.spec_from_file_location(name, str(PROJECT_ROOT / rel))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


sbom = _load("spark_generate_sbom_under_test", "tools/generate-sbom.py")
base = _load("spark_sbom_supply_chain_fixture", "Tests/test_check_supply_chain.py")

RULES_TEXT = json.dumps(
    {
        "schema": 1,
        "fontSuffixes": [".ttf"],
        "licenseText": {
            "minimumBytes": 10,
            "copyrightPattern": "[Cc]opyright",
            "operativeTermsPattern": "Permission",
        },
        "thirdPartyRoots": ["^include/Vendor/"],
        "payloadRules": [
            {"pattern": "^include/Vendor/glue\\.h$", "firstParty": "repository-authored integration header"},
            {"pattern": "^include/Vendor/alpha/", "component": "Alpha"},
            {"pattern": "^lib/(lib)?alpha\\.(a|lib)$", "component": "Alpha"},
            {"pattern": "^(bin|lib)/(lib)?Beta[^/]*$", "component": "Beta"},
        ],
    }
)

GOOD_FILES = [
    "bin/SparkEngine",
    "bin/libBeta-2.0.so",
    "include/Vendor/alpha/alpha.h",
    "include/Vendor/glue.h",
    "lib/libalpha.a",
    "share/fonts/Editor.ttf",
    "THIRD_PARTY_NOTICES.txt",
]


def dependency(name: str, version: str):
    return sbom.Dependency(
        name=name,
        source=f"https://github.com/example/{name.lower()}",
        version=version,
        declared_license="MIT",
        spdx_license="MIT",
        local_path=f"ThirdParty/{name}",
        kind="vendored",
        pin="0" * 64,
        file_count=1,
    )


INVENTORY = [dependency("Alpha", "v1.0.0"), dependency("Beta", "b" * 40), dependency("Gamma", "snapshot")]


def notice_text(entries: dict[str, str]) -> str:
    body = "".join(
        f"{name}\n  Source: x\n  Version: {version}\n  License: MIT\n\n" for name, version in entries.items()
    )
    return (
        "SparkEngine Third-Party Notices\n================================\n\n"
        "Dependency inventory\n--------------------\n\n"
        f"{body}"
        "Complete license and notice texts\n=================================\n\n"
    )


GOOD_NOTICE = {"Alpha": "v1.0.0", "Beta": "b" * 40, "Gamma": "snapshot"}


class ReconcileCase(unittest.TestCase):
    """A synthetic package that reconciles; each test changes one thing."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="spark-sbom-rec-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.rules = sbom.notices.parse_package_rules(RULES_TEXT, "fixture rules")
        self.notice = self.tmp / "THIRD_PARTY_NOTICES.txt"
        self.write_notice(GOOD_NOTICE)

    def write_notice(self, entries: dict[str, str]) -> None:
        self.notice.write_text(notice_text(entries), encoding="utf-8")

    def run_reconcile(self, files=None, not_configured=(), inventory=None) -> dict:
        return sbom.reconcile(
            INVENTORY if inventory is None else inventory,
            self.rules,
            sorted(GOOD_FILES if files is None else files),
            self.notice,
            list(not_configured),
        )

    def assert_error(self, report: dict, *needles: str) -> None:
        blob = "\n".join(report["errors"])
        self.assertTrue(report["errors"], "expected a reconciliation error")
        for needle in needles:
            self.assertIn(needle, blob)


class TestReconcileBaseline(ReconcileCase):
    def test_clean_package_reconciles(self) -> None:
        report = self.run_reconcile()
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["components"], {"Alpha": 2, "Beta": 1})
        self.assertEqual(report["firstPartyExemptFiles"], 1)
        # Gamma has no install payload rule: reported, never claimed as verified.
        self.assertEqual(report["compiledInOnly"], ["Gamma"])
        self.assertEqual(report["schema"], sbom.RECONCILE_SCHEMA)


class TestReconcileForgedPayload(ReconcileCase):
    def test_unmapped_file_under_third_party_root_fails(self) -> None:
        report = self.run_reconcile(GOOD_FILES + ["include/Vendor/evil/backdoor.h"])
        self.assert_error(report, "include/Vendor/evil/backdoor.h", "no package rule maps")

    def test_file_of_an_unlocked_component_fails(self) -> None:
        inventory = [dep for dep in INVENTORY if dep.name != "Beta"]
        self.write_notice({k: v for k, v in GOOD_NOTICE.items() if k != "Beta"})
        report = self.run_reconcile(inventory=inventory)
        self.assert_error(report, "bin/libBeta-2.0.so: ships component 'Beta'", "does not lock")

    def test_rule_for_an_unlocked_component_fails_even_without_files(self) -> None:
        inventory = [dep for dep in INVENTORY if dep.name != "Beta"]
        self.write_notice({k: v for k, v in GOOD_NOTICE.items() if k != "Beta"})
        files = [f for f in GOOD_FILES if "Beta" not in f]
        report = self.run_reconcile(files, inventory=inventory)
        self.assert_error(report, "package rule names component 'Beta'")

    def test_first_party_exemption_is_exact(self) -> None:
        report = self.run_reconcile(GOOD_FILES + ["include/Vendor/glue2.h"])
        self.assert_error(report, "include/Vendor/glue2.h")


class TestReconcileMissingDependency(ReconcileCase):
    def test_locked_shipped_dependency_absent_fails(self) -> None:
        files = [f for f in GOOD_FILES if "Beta" not in f]
        report = self.run_reconcile(files)
        self.assert_error(report, "locked dependency 'Beta'", "ships no file")

    def test_not_configured_declaration_accepts_a_real_absence(self) -> None:
        files = [f for f in GOOD_FILES if "Beta" not in f]
        report = self.run_reconcile(files, not_configured=["Beta"])
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["notConfigured"], ["Beta"])

    def test_stale_not_configured_declaration_fails(self) -> None:
        report = self.run_reconcile(not_configured=["Beta"])
        self.assert_error(report, "--not-configured Beta", "ships 1 file")

    def test_not_configured_cannot_name_a_compiled_in_or_unknown_dependency(self) -> None:
        for name in ("Gamma", "Nonexistent"):
            with self.subTest(name=name):
                report = self.run_reconcile(not_configured=[name])
                self.assert_error(report, f"--not-configured {name}", "not a locked dependency with install payload")


class TestReconcileNoticeInventory(ReconcileCase):
    def test_notice_version_from_a_different_lock_fails(self) -> None:
        self.write_notice({**GOOD_NOTICE, "Alpha": "v0.9.0"})
        self.assert_error(self.run_reconcile(), "lists 'Alpha' at version 'v0.9.0'", "'v1.0.0'")

    def test_notice_missing_a_locked_dependency_fails(self) -> None:
        self.write_notice({k: v for k, v in GOOD_NOTICE.items() if k != "Gamma"})
        self.assert_error(self.run_reconcile(), "does not list locked dependency 'Gamma'")

    def test_notice_listing_an_unlocked_dependency_fails(self) -> None:
        self.write_notice({**GOOD_NOTICE, "Delta": "1.0"})
        self.assert_error(self.run_reconcile(), "lists 'Delta', which dependencies.lock does not lock")

    def test_missing_notice_is_a_failure(self) -> None:
        self.notice.unlink()
        with self.assertRaises(sbom.SbomError):
            self.run_reconcile()

    def test_notice_without_generated_inventory_is_an_input_error(self) -> None:
        self.notice.write_text("hand-written notices\n", encoding="utf-8")
        with self.assertRaises(sbom.InputError):
            self.run_reconcile()


class TestInstallManifestAndTree(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="spark-sbom-inv-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def manifest(self, lines: list[str]) -> Path:
        path = self.tmp / "install_manifest.txt"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    def test_prefix_is_the_shortest_notice_directory(self) -> None:
        prefix = "/opt/stage"
        path = self.manifest(
            [
                f"{prefix}/bin/SparkEngine",
                f"{prefix}/share/sdk/THIRD_PARTY_NOTICES.txt",
                f"{prefix}/THIRD_PARTY_NOTICES.txt",
            ]
        )
        files, found = sbom._read_install_manifest(path, None)
        self.assertEqual(found, Path(prefix))
        self.assertEqual(files, ["THIRD_PARTY_NOTICES.txt", "bin/SparkEngine", "share/sdk/THIRD_PARTY_NOTICES.txt"])

    def test_windows_manifest_paths_are_normalized(self) -> None:
        path = self.manifest(["C:/stage/bin/SDL2.dll", "C:\\stage\\THIRD_PARTY_NOTICES.txt"])
        files, found = sbom._read_install_manifest(path, None)
        self.assertEqual(found.as_posix(), "C:/stage")
        self.assertIn("bin/SDL2.dll", files)

    def test_file_outside_the_prefix_is_rejected(self) -> None:
        path = self.manifest(["/opt/stage/THIRD_PARTY_NOTICES.txt", "/etc/passwd"])
        with self.assertRaisesRegex(sbom.InputError, "outside install prefix"):
            sbom._read_install_manifest(path, None)

    def test_dot_segments_are_rejected(self) -> None:
        path = self.manifest(["/opt/stage/THIRD_PARTY_NOTICES.txt", "/opt/stage/lib/../../evil"])
        with self.assertRaisesRegex(sbom.InputError, "not a normalized path"):
            sbom._read_install_manifest(path, None)

    def test_manifest_without_notice_requires_an_explicit_prefix(self) -> None:
        path = self.manifest(["/opt/stage/bin/SparkEngine"])
        with self.assertRaisesRegex(sbom.InputError, "--install-prefix"):
            sbom._read_install_manifest(path, None)
        files, _ = sbom._read_install_manifest(path, "/opt/stage")
        self.assertEqual(files, ["bin/SparkEngine"])

    def test_empty_manifest_is_rejected(self) -> None:
        with self.assertRaisesRegex(sbom.InputError, "lists no installed file"):
            sbom._read_install_manifest(self.manifest([]), "/opt/stage")

    @unittest.skipIf(os.name == "nt", "symlink creation needs privileges on Windows")
    def test_tree_walk_reports_a_symlinked_directory_without_descending(self) -> None:
        root = self.tmp / "pkg"
        (root / "include" / "Vendor").mkdir(parents=True)
        outside = self.tmp / "outside"
        (outside / "deep").mkdir(parents=True)
        (outside / "deep" / "hidden.h").write_text("x", encoding="utf-8")
        os.symlink(outside, root / "include" / "Vendor" / "linked")
        self.assertEqual(sbom._walk_package(root), ["include/Vendor/linked"])


class TestReconcileCli(unittest.TestCase):
    """End to end through main(): exit codes and the JSON report."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="spark-sbom-cli-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.prefix = self.tmp / "stage"
        self.prefix.mkdir()
        (self.prefix / "THIRD_PARTY_NOTICES.txt").write_text(notice_text(GOOD_NOTICE), encoding="utf-8")
        rules = sbom.notices.parse_package_rules(RULES_TEXT, "fixture rules")
        patches = [
            mock.patch.object(sbom, "load_inventory", return_value=INVENTORY),
            mock.patch.object(sbom, "load_rules", return_value=rules),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def run_main(self, files: list[str], *extra: str) -> tuple[int, str]:
        manifest = self.tmp / "install_manifest.txt"
        manifest.write_text("".join(f"{self.prefix.as_posix()}/{rel}\n" for rel in files), encoding="utf-8")
        err = io.StringIO()
        with redirect_stdout(io.StringIO()), redirect_stderr(err):
            code = sbom.main(["reconcile", "--install-manifest", str(manifest), *extra])
        return code, err.getvalue()

    def test_clean_manifest_exits_zero_and_writes_report(self) -> None:
        report_path = self.tmp / "report.json"
        code, err = self.run_main(GOOD_FILES, "--json-report", str(report_path))
        self.assertEqual(code, 0, err)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["packageFiles"], len(GOOD_FILES))

    def test_forged_payload_exits_one(self) -> None:
        code, err = self.run_main(GOOD_FILES + ["include/Vendor/evil.h"])
        self.assertEqual(code, 1)
        self.assertIn("include/Vendor/evil.h", err)

    def test_missing_dependency_exits_one(self) -> None:
        code, err = self.run_main([f for f in GOOD_FILES if "alpha" not in f])
        self.assertEqual(code, 1)
        self.assertIn("locked dependency 'Alpha'", err)

    def test_malformed_manifest_exits_two(self) -> None:
        code, err = self.run_main(GOOD_FILES, "--install-prefix", "/somewhere/else")
        self.assertEqual(code, 2)
        self.assertIn("outside install prefix", err)


class TestRealRepositoryLock(unittest.TestCase):
    """The committed lock, license policy and GOV-400 rules agree today."""

    @classmethod
    def setUpClass(cls) -> None:
        base._require("cmake")
        cls.inventory = sbom.load_inventory(PROJECT_ROOT)

    def test_every_package_rule_component_is_locked(self) -> None:
        names = {dep.name for dep in self.inventory}
        rules = sbom.load_rules()
        unlocked = sorted({r.component for r in rules.payload if r.component} - names)
        self.assertEqual(unlocked, [])

    def test_submodule_versions_are_the_locked_gitlinks(self) -> None:
        lock = json.loads((PROJECT_ROOT / "ThirdParty/supply-chain.lock").read_text(encoding="utf-8"))
        submodules = {dep.local_path: dep for dep in self.inventory if dep.kind == "submodule"}
        self.assertEqual(set(submodules), set(lock["submodule_gitlinks"]))
        for path, dep in submodules.items():
            self.assertEqual(dep.version, lock["submodule_gitlinks"][path])
            self.assertEqual(dep.pin, dep.version)

    def test_generated_document_describes_the_whole_lock(self) -> None:
        dirty = subprocess.run(
            ["git", "-C", str(PROJECT_ROOT), "diff", "--quiet", "HEAD", "--", "ThirdParty/dependencies.lock",
             "ThirdParty/supply-chain.lock"],
            capture_output=True,
            check=False,
        )
        if dirty.returncode != 0:
            self.skipTest("lockfiles have uncommitted edits; generation correctly refuses them")
        first = sbom.render(sbom.generate(PROJECT_ROOT))
        self.assertEqual(first, sbom.render(sbom.generate(PROJECT_ROOT)), "generation must be deterministic")
        document = json.loads(first)
        self.assertEqual(document["spdxVersion"], "SPDX-2.3")
        packages = {p["name"]: p for p in document["packages"] if p["SPDXID"] != sbom.ROOT_SPDX_ID}
        self.assertEqual(set(packages), {dep.name for dep in self.inventory})
        for dep in self.inventory:
            self.assertEqual(packages[dep.name]["licenseDeclared"], dep.spdx_license)
            self.assertEqual(packages[dep.name]["versionInfo"], dep.version)
        ids = [p["SPDXID"] for p in document["packages"]]
        self.assertEqual(len(ids), len(set(ids)))
        head = subprocess.run(["git", "-C", str(PROJECT_ROOT), "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
        lock_digest = sbom.provenance._committed_lock_digest(PROJECT_ROOT, head)
        self.assertEqual(document["documentNamespace"], f"urn:spark-engine:spdx:{head}:{lock_digest}")


class TestGenerateFakeRepository(base.FakeRepoCase):
    """Generation against a fake repository the real supply-chain checker passes."""

    def setUp(self) -> None:
        super().setUp()
        self.write("CMakeLists.txt", 'set(SPARK_ENGINE_VERSION "1.2.3" CACHE STRING "version")\n')
        self.write("LICENSE", "Fixture Open License 1.0\n\nCopyright (c) 2026 fixture\n")
        self.commit("sbom inputs")

    def run_main(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            try:
                code = sbom.main(["--source-root", str(self.repo), *args])
            except SystemExit as exit_:
                code = int(exit_.code)
        return code, out.getvalue(), err.getvalue()

    def head(self) -> str:
        return subprocess.run(["git", "-C", str(self.repo), "rev-parse", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()

    def test_document_binds_to_commit_and_committed_lock_digest(self) -> None:
        code, out, err = self.run_main()
        self.assertEqual(code, 0, err)
        document = json.loads(out)
        head = self.head()
        lock_digest = sbom.provenance._committed_lock_digest(self.repo, head)
        self.assertEqual(document["documentNamespace"], f"urn:spark-engine:spdx:{head}:{lock_digest}")
        self.assertIn(lock_digest, document["creationInfo"]["comment"])
        root, demo = document["packages"]
        self.assertEqual((root["name"], root["versionInfo"]), ("SparkEngine", "1.2.3"))
        self.assertEqual(root["licenseDeclared"], "LicenseRef-Fixture-Open-License-1.0")
        self.assertEqual(document["hasExtractedLicensingInfos"][0]["licenseId"], root["licenseDeclared"])
        self.assertEqual(demo["name"], "demo")
        self.assertEqual(demo["licenseDeclared"], "MIT")
        self.assertEqual(demo["downloadLocation"], "https://github.com/example/demo")
        self.assertNotIn("externalRefs", demo, "a vendored snapshot asserts no purl version")
        digest = self.lock()["tree_digests"]["ThirdParty/Utils/demo"]["digest"]
        self.assertIn(f"sha256:{digest}", demo["sourceInfo"])
        self.assertEqual(
            document["relationships"],
            [
                {"relatedSpdxElement": "SPDXRef-SparkEngine", "relationshipType": "DESCRIBES",
                 "spdxElementId": "SPDXRef-DOCUMENT"},
                {"relatedSpdxElement": "SPDXRef-Package-demo", "relationshipType": "DEPENDS_ON",
                 "spdxElementId": "SPDXRef-SparkEngine"},
            ],
        )

    def test_check_reproduces_and_detects_tampering(self) -> None:
        target = Path(self._tmp.name) / "sbom.spdx.json"
        self.assertEqual(self.run_main("--out", str(target))[0], 0)
        self.assertEqual(self.run_main("--check", str(target))[0], 0)
        document = json.loads(target.read_text(encoding="utf-8"))
        document["packages"][1]["licenseDeclared"] = "Unlicense"
        target.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        code, _, err = self.run_main("--check", str(target))
        self.assertEqual(code, 1)
        self.assertIn("does not equal", err)

    def test_check_rejects_newline_translated_copy(self) -> None:
        target = Path(self._tmp.name) / "sbom.spdx.json"
        self.assertEqual(self.run_main("--out", str(target))[0], 0)
        target.write_bytes(target.read_bytes().replace(b"\n", b"\r\n"))
        code, _, err = self.run_main("--check", str(target))
        self.assertEqual(code, 1)
        self.assertIn("does not equal", err)

    def test_uncommitted_lock_edit_is_refused(self) -> None:
        manifest = self.repo / "ThirdParty/dependencies.lock"
        manifest.write_text(manifest.read_text(encoding="utf-8").replace("v1.2.3", "v9.9.9"), encoding="utf-8")
        code, _, err = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("differs from the committed blob", err)

    def test_source_sha_other_than_head_is_refused(self) -> None:
        code, _, err = self.run_main("--source-sha", "0" * 40)
        self.assertEqual(code, 1)
        self.assertIn("is not the declared source SHA", err)

    def test_locked_container_without_manifest_entry_is_refused(self) -> None:
        data = self.lock()
        data["managed_vendored_dirs"].append("ThirdParty/Extra")
        self.set_lock(data)
        self.write("ThirdParty/Extra/thing.h", "/* payload */\n")
        self.commit()
        code, _, err = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("ThirdParty/Extra is locked in supply-chain.lock but has no dependencies.lock entry", err)

    def test_unresolvable_license_is_refused(self) -> None:
        manifest = self.repo / "ThirdParty/dependencies.lock"
        manifest.write_text(manifest.read_text(encoding="utf-8").replace("|MIT|", "|Proprietary|"), encoding="utf-8")
        self.commit()
        code, _, err = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("allow-list", err)

    def test_empty_license_field_is_refused(self) -> None:
        manifest = self.repo / "ThirdParty/dependencies.lock"
        manifest.write_text(manifest.read_text(encoding="utf-8").replace("|MIT|", "||"), encoding="utf-8")
        self.commit()
        code, _, err = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("empty name or license", err)


class TestInventoryConsistency(unittest.TestCase):
    """Lock disagreements that the fake repository cannot express with a real submodule."""

    LOCK = {
        "submodule_gitlinks": {"ThirdParty/Sub": "a" * 40},
        "managed_vendored_dirs": [],
        "tree_digests": {},
        "license_policy": {"dependencies": {}},
    }

    def inventory(self, fields: list[str]):
        with mock.patch.object(sbom.supply_chain, "load_lockfile", return_value=self.LOCK), mock.patch.object(
            sbom.supply_chain, "export_manifest_entries", return_value=[fields]
        ):
            return sbom.load_inventory(PROJECT_ROOT)

    def fields(self, version: str, path: str = "ThirdParty/Sub") -> list[str]:
        return ["Sub", "https://github.com/example/sub.git", version, "MIT", path, "x.h", "M", "F", "WARN", "L"]

    def test_matching_gitlink_yields_a_purl_pinned_package(self) -> None:
        (dep,) = self.inventory(self.fields("a" * 40))
        package = sbom._package(dep)
        self.assertEqual(package["downloadLocation"], f"git+https://github.com/example/sub.git@{'a' * 40}")
        self.assertEqual(package["externalRefs"][0]["referenceLocator"], f"pkg:github/example/sub@{'a' * 40}")

    def test_manifest_revision_disagreeing_with_locked_gitlink_is_refused(self) -> None:
        with self.assertRaisesRegex(sbom.SbomError, "pins gitlink"):
            self.inventory(self.fields("b" * 40))

    def test_manifest_path_outside_every_locked_container_is_refused(self) -> None:
        with self.assertRaisesRegex(sbom.SbomError, "neither a locked submodule gitlink nor a managed vendored"):
            self.inventory(self.fields("a" * 40, path="ThirdParty/Elsewhere"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
