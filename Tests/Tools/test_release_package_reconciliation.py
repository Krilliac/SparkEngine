#!/usr/bin/env python3
"""Fail-closed checks for final-package evidence and publication byte coverage."""
import copy
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("release_package_reconciliation",
                                            ROOT / "tools/release_package_reconciliation.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
SOURCE = {"sha": "a" * 40, "dependencyLockSha256": "b" * 64}
NAME = "SparkEngine-1.0.0-Windows-AMD64-MinSizeRel.zip"
DIGEST = "c" * 64


def report():
    return {"schema": tool.SCHEMA, "platform": "Windows", "configuration": "MinSizeRel",
            "source": SOURCE.copy(), "errors": [], "artifacts": [{
                "name": NAME, "sha256": DIGEST, "inventory": {
                    "schema": "spark-package-inventory-reconciliation-v1", "packageFiles": 12,
                    "errors": [], "source": SOURCE.copy(),
                    "artifact": {"name": NAME, "sha256": DIGEST, "format": "zip"}}}]}


class FinalPackageReconciliationTests(unittest.TestCase):
    def verify(self, documents=None, published=None):
        tool.verify_documents([report()] if documents is None else documents,
                              {NAME: DIGEST} if published is None else published, SOURCE)

    def test_accepts_exact_final_bytes_and_identical_alias(self):
        self.verify()
        self.verify(published={NAME: DIGEST, "SparkEngine-Windows-x64-Release.zip": DIGEST})

    def test_missing_extra_changed_and_alias_only_publication_are_rejected(self):
        for published in ({}, {NAME: "d" * 64}, {NAME: DIGEST, "foreign.exe": "d" * 64}, {"alias.zip": DIGEST}):
            with self.subTest(published=published), self.assertRaises(ValueError):
                self.verify(published=published)

    def test_empty_duplicate_and_unknown_report_schemas_are_rejected(self):
        for documents in ([], [report(), report()], [{**report(), "unverified": True}]):
            with self.subTest(documents=documents), self.assertRaises(ValueError):
                self.verify(documents)

    def test_source_and_lock_drift_are_rejected(self):
        for key in SOURCE:
            document = report()
            document["source"][key] = "f" * len(SOURCE[key])
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "source/lock"):
                self.verify([document])

    def test_missing_failed_and_wrong_byte_inventories_are_rejected(self):
        for kind in ("missing", "failed", "wrong-bytes", "wrong-name", "empty"):
            document = report()
            entry = document["artifacts"][0]
            if kind == "missing":
                entry["inventory"] = None
            elif kind == "failed":
                entry["inventory"]["errors"] = ["unmapped dependency"]
            elif kind == "empty":
                entry["inventory"]["packageFiles"] = 0
            elif kind == "wrong-name":
                entry["inventory"]["artifact"]["name"] = "other.zip"
            else:
                entry["inventory"]["artifact"]["sha256"] = "e" * 64
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                self.verify([document])

    def test_installer_outer_hash_cannot_stand_in_for_final_inventory(self):
        document = report()
        document["errors"] = ["final native/standalone payload inventory has not been verified"]
        with self.assertRaisesRegex(ValueError, "blocked or failed"):
            self.verify([document])

    def test_record_reports_unverified_native_payload_instead_of_accepting_stage(self):
        package = Path("packages/SparkEngine-1.0.0-Runtime.exe")
        fake_sbom = unittest.mock.Mock()
        fake_sbom.load_inventory.return_value = []
        fake_sbom.load_rules.return_value = object()
        fake_sbom.InputError = ValueError
        fake_sbom.SbomError = RuntimeError
        fake_sbom.reconcile_nsis.side_effect = ValueError("unsupported NSIS decoder")
        with patch.object(tool, "load_tool", return_value=fake_sbom), \
                patch.object(tool, "source_identity", return_value=SOURCE), \
                patch.object(Path, "iterdir", return_value=iter([package])), \
                patch.object(tool, "regular", side_effect=lambda path: path), \
                patch.object(tool.provenance, "_sha256_file", return_value=DIGEST):
            result = tool.create(Path("packages"), ROOT, SOURCE["sha"], "Windows", "MinSizeRel", [])
        self.assertTrue(result["errors"])
        self.assertIsNone(result["artifacts"][0]["inventory"])
        fake_sbom.reconcile_archive.assert_not_called()
        fake_sbom.reconcile_nsis.assert_called_once()

    def test_unsafe_artifact_names_and_duplicate_json_fields_are_rejected(self):
        for name in ("../payload.zip", "C:payload.zip", "a/b.zip", "file.zip\n"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                tool.safe_name(name)
        with self.assertRaises(ValueError):
            tool.unique_object([("errors", []), ("errors", ["failure"])])


native = tool.native_msi


class FixtureDatabase:
    def __init__(self):
        self.tables = {
            "_Tables": [(name,) for name in ("File", "Component", "Directory", "Media", "Feature", "FeatureComponents")],
            "_Storages": [],
            "Directory": [("INSTALL_ROOT", "TARGETDIR", "SparkEngine"),
                          ("BIN", "INSTALL_ROOT", "bin"), ("SAMPLES", "INSTALL_ROOT", "samples")],
            "Component": [("Runtime", "BIN"), ("OptionalSamples", "SAMPLES"), ("Notices", "INSTALL_ROOT")],
            "File": [("Engine", "Runtime", "engine.exe", "6", "512", "1"),
                     ("Notice", "Notices", "NOTICE~1.TXT|THIRD_PARTY_NOTICES.txt", "7", "512", "2"),
                     ("Optional", "OptionalSamples", "example.txt", "8", "512", "3")],
            "Media": [("1", "2", "#first.cab", ""), ("2", "3", "#optional.cab", "")],
            "Feature": [("ProductFeature", "", "1", "16"),
                        *((f"CM_C_{name}", "ProductFeature", "1", "0") for name in ("runtime", "redist", "tools", "samples"))],
            "FeatureComponents": [("CM_C_runtime", "Runtime"), ("CM_C_samples", "OptionalSamples"),
                                  ("CM_C_runtime", "Notices")],
        }
        self.source_type = 2

    def rows(self, table, _columns):
        return self.tables[table]

    def word_count(self):
        return self.source_type


def fixture_evidence():
    payload, media = native.payload_plan(FixtureDatabase())
    features, component_features = native.feature_layout(FixtureDatabase())
    for item in payload:
        item["sha256"] = "d" * 64
    return {"method": native.METHOD, "scope": "all-file-table-components", "root": "INSTALL_ROOT",
            "features": features, "componentFeatures": component_features,
            "files": payload, "manifestSha256": native.manifest_digest(payload),
            "cabinets": [{"stream": item["stream"], "sha256": "e" * 64, "size": 128,
                          "files": sum(entry["cabinet"] == item["stream"] for entry in payload)} for item in media]}


class NativeMsiPlanTests(unittest.TestCase):
    def test_profile_requires_exact_features_hierarchy_and_membership(self):
        policy = native.parse_profiles((ROOT / native.PROFILE_PATH).read_bytes())
        evidence = fixture_evidence()
        self.assertEqual(native.prove_profile(evidence, policy)["samples/example.txt"], "samples")
        for kind in ("missing", "unknown", "parent", "root-payload", "wrong-component"):
            changed = copy.deepcopy(evidence)
            if kind == "missing":
                changed["features"].pop()
            elif kind == "unknown":
                changed["features"].append({"id": "CM_C_sdk", "parent": "ProductFeature", "level": 1, "attributes": 0})
            elif kind == "parent":
                changed["features"][0]["parent"] = "Unknown"
            elif kind == "root-payload":
                changed["componentFeatures"]["Runtime"] = "ProductFeature"
            else:
                changed["componentFeatures"]["Runtime"] = "CM_C_sdk"
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                native.prove_profile(changed, policy)
        for pair in (("MissingFeature", "Runtime"), ("CM_C_tools", "MissingComponent"), ("CM_C_tools", "Runtime")):
            database = FixtureDatabase()
            database.tables["FeatureComponents"].append(pair)
            with self.subTest(pair=pair), self.assertRaises(ValueError):
                native.feature_layout(database)

    def test_profile_policy_rejects_unknown_duplicate_and_empty_component_sets(self):
        original = json.loads((ROOT / native.PROFILE_PATH).read_text(encoding="utf-8"))
        for components in ([], ["runtime", "runtime"], ["symbols"], [None]):
            policy = copy.deepcopy(original)
            policy["windowsRuntime"]["components"] = components
            with self.subTest(components=components), self.assertRaises(ValueError):
                native.parse_profiles(json.dumps(policy).encode())
        with self.assertRaisesRegex(ValueError, "duplicate"):
            native.parse_profiles(b'{"schema":1,"schema":1}')

    def test_cmake_producers_consume_the_shared_component_profile(self):
        policy = native.parse_profiles((ROOT / native.PROFILE_PATH).read_bytes())
        with tempfile.TemporaryDirectory(prefix="spark-profile-cmake-") as raw:
            temporary = Path(raw)
            output = temporary / "components.txt"
            script = temporary / "profile.cmake"
            root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
            block = root_cmake[root_cmake.index('# Shared with final-payload reconciliation;'):
                               root_cmake.index('set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME')]
            for msvc in (False, True):
                script.write_text('cmake_minimum_required(VERSION 3.25)\n'
                    f'set(CMAKE_SOURCE_DIR "{ROOT.as_posix()}")\nset(MSVC {"ON" if msvc else "OFF"})\n'
                    + block + f'file(WRITE "{output.as_posix()}" "${{CPACK_COMPONENTS_ALL}}")\n', encoding="utf-8")
                result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                expected = policy["archives"]["components"] + (policy["archives"]["msvcAdditionalComponents"] if msvc else [])
                self.assertEqual(output.read_text().split(';'), expected)
            script.write_text('cmake_minimum_required(VERSION 3.25)\nset(CPACK_GENERATOR WIX)\n'
                f'include("{(ROOT / "cmake/SparkCPackOptions.cmake").as_posix()}")\n'
                f'file(WRITE "{output.as_posix()}" "${{CPACK_COMPONENTS_ALL}}")\n', encoding="utf-8")
            result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_text().split(';'), policy["windowsRuntime"]["components"])

    def test_all_optional_components_and_cabinets_are_mapped(self):
        payload, cabinets = native.payload_plan(FixtureDatabase())
        self.assertEqual([item["path"] for item in payload],
                         ["THIRD_PARTY_NOTICES.txt", "bin/engine.exe", "samples/example.txt"])
        self.assertEqual(payload[-1]["component"], "OptionalSamples")
        self.assertEqual(payload[-1]["cabinet"], "optional.cab")
        self.assertEqual(len(cabinets), 2)

    def test_external_uncompressed_missing_duplicate_and_bad_media_are_rejected(self):
        for rows in ([], [("1", "3", "external.cab", "")], [("1", "3", "", "")],
                     [("1", "3", "#media.cab", "REMOTE")], [("1", "1", "#media.cab", "")],
                     [("1", "2", "#media.cab", ""), ("2", "3", "#MEDIA.cab", "")],
                     [("1", "2", "#media.cab", ""), ("1", "3", "#second.cab", "")]):
            database = FixtureDatabase()
            database.tables["Media"] = rows
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                native.payload_plan(database)

    def test_file_paths_collisions_sequences_sizes_and_components_fail_closed(self):
        original = FixtureDatabase().tables["File"][0]
        cases = [(2, "../outside"), (2, "C:outside"), (2, "NUL.txt"), (2, "escape. "),
                 (2, "a\\b.txt"), (2, "a:stream"), (3, "-1"), (3, str(native.MAX_BYTES + 1)),
                 (1, "Missing"), (4, "8192"), (4, "4096"), (5, "3"), (5, "0")]
        for index, value in cases:
            database = FixtureDatabase()
            changed = list(original)
            changed[index] = value
            database.tables["File"][0] = tuple(changed)
            with self.subTest(index=index, value=value), self.assertRaises(ValueError):
                native.payload_plan(database)
        database = FixtureDatabase()
        database.tables["File"].append(("Duplicate", "Runtime", "ENGINE.EXE", "3", "512", "4"))
        with self.assertRaisesRegex(ValueError, "colliding"):
            native.payload_plan(database)

    def test_directory_cycles_outside_root_and_file_directory_collisions_fail(self):
        for parent in ("BIN", "TARGETDIR", "Missing"):
            database = FixtureDatabase()
            database.tables["Directory"][1] = ("BIN", parent, "bin")
            with self.subTest(parent=parent), self.assertRaises(ValueError):
                native.payload_plan(database)
        database = FixtureDatabase()
        database.tables["File"][1] = ("Notice", "Notices", "bin", "7", "512", "2")
        with self.assertRaisesRegex(ValueError, "file/directory collision"):
            native.payload_plan(database)

    def test_admin_image_transforms_and_payload_rewriting_tables_fail(self):
        for source_type in (0, 4, 6, 18):
            database = FixtureDatabase()
            database.source_type = source_type
            with self.subTest(source_type=source_type), self.assertRaises(ValueError):
                native.payload_plan(database)
        database = FixtureDatabase()
        database.tables["_Storages"] = [("transform",)]
        with self.assertRaisesRegex(ValueError, "transforms"):
            native.payload_plan(database)
        for name in ("DuplicateFile", "MoveFile", "Patch", "PatchPackage", "MsiPatchMetadata"):
            database = FixtureDatabase()
            database.tables["_Tables"].append((name,))
            with self.subTest(table=name), self.assertRaisesRegex(ValueError, "unsupported"):
                native.payload_plan(database)

    def test_publication_requires_complete_msi_evidence(self):
        document = report()
        entry = document["artifacts"][0]
        name = "SparkEngine-Runtime.msi"
        entry["name"] = entry["inventory"]["artifact"]["name"] = name
        entry["inventory"]["artifact"]["format"] = "msi"
        entry["inventory"]["packageFiles"] = 3
        entry["inventory"]["extraction"] = fixture_evidence()
        entry["inventory"]["noticeCoverage"] = {"fonts": 0, "embeddedFonts": 0, "thirdPartyPayloadFiles": 0}
        authority = native.profile_authority(lambda path: (ROOT / path).read_bytes())
        entry["inventory"]["packageProfile"] = {**authority[2],
                    "installComponents": sorted(authority[0]["windowsRuntime"]["components"])}
        tool.verify_documents([document], {name: DIGEST}, SOURCE, authority)
        with self.assertRaisesRegex(ValueError, "committed package-profile authority"):
            tool.verify_documents([document], {name: DIGEST}, SOURCE)
        for kind in ("outer-only", "missing-file", "missing-cabinet", "bad-digest", "bad-path", "wrong-count", "stage", "policy", "rules", "notice-coverage"):
            changed = copy.deepcopy(document)
            evidence = changed["artifacts"][0]["inventory"]["extraction"]
            if kind == "outer-only":
                del changed["artifacts"][0]["inventory"]["extraction"]
            elif kind == "missing-file":
                evidence["files"].pop()
            elif kind == "missing-cabinet":
                evidence["cabinets"].pop()
            elif kind == "bad-digest":
                evidence["files"][0]["sha256"] = "not-a-digest"
            elif kind == "bad-path":
                evidence["files"][0]["path"] = "../outside"
            elif kind == "wrong-count":
                evidence["cabinets"][0]["files"] += 1
            elif kind in ("policy", "rules"):
                profile = changed["artifacts"][0]["inventory"]["packageProfile"]
                profile["profileSha256" if kind == "policy" else "rulesSha256"] = "f" * 64
            elif kind == "notice-coverage":
                del changed["artifacts"][0]["inventory"]["noticeCoverage"]
            else:
                evidence["method"] = "caller-supplied-stage"
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                tool.verify_documents([changed], {name: DIGEST}, SOURCE, authority)

    def test_fabricated_exe_inventory_remains_unsupported(self):
        document = report()
        entry = document["artifacts"][0]
        entry["name"] = entry["inventory"]["artifact"]["name"] = "SparkInstaller-Windows.exe"
        with self.assertRaisesRegex(ValueError, "format mismatch"):
            tool.verify_documents([document], {entry["name"]: DIGEST}, SOURCE)


class NativeNsisPublicationTests(unittest.TestCase):
    """The publication gate requires complete static NSIS evidence and source authority."""

    def setUp(self):
        fixture_spec = importlib.util.spec_from_file_location(
            "nsis_publication_fixture", ROOT / "Tests/Tools/test_native_nsis_payload.py")
        fixture = importlib.util.module_from_spec(fixture_spec)
        fixture_spec.loader.exec_module(fixture)
        self.document = report()
        self.name = "SparkEngine-Runtime.exe"
        entry = self.document["artifacts"][0]
        entry["name"] = entry["inventory"]["artifact"]["name"] = self.name
        entry["inventory"]["artifact"]["format"] = "nsis"
        entry["inventory"]["packageFiles"] = 4
        entry["inventory"]["extraction"] = fixture.evidence_fixture()
        self.authority = native.profile_authority(lambda path: (ROOT / path).read_bytes())
        entry["inventory"]["packageProfile"] = {
            **self.authority[2], "installComponents": sorted(self.authority[0]["windowsRuntime"]["components"])}
        entry["inventory"]["noticeCoverage"] = {"fonts": 0, "embeddedFonts": 0, "thirdPartyPayloadFiles": 0}

    def test_complete_nsis_inventory_and_identical_alias_are_accepted(self):
        tool.verify_documents([self.document], {self.name: DIGEST, "SparkInstaller.exe": DIGEST},
                              SOURCE, self.authority)

    def test_outer_only_stage_partial_and_unbound_reports_are_rejected(self):
        for kind in ("outer-only", "stage", "missing-support", "missing-uninstaller", "wrong-count",
                     "wrong-profile", "notice-coverage", "false-signature-claim"):
            changed = copy.deepcopy(self.document)
            inventory = changed["artifacts"][0]["inventory"]
            if kind == "outer-only":
                del inventory["extraction"]
            elif kind == "stage":
                inventory["extraction"]["method"] = "caller-supplied-stage"
            elif kind == "missing-support":
                inventory["extraction"]["support"].clear()
            elif kind == "missing-uninstaller":
                del inventory["extraction"]["uninstaller"]
            elif kind == "wrong-count":
                inventory["packageFiles"] += 1
            elif kind == "wrong-profile":
                inventory["packageProfile"]["profileSha256"] = "f" * 64
            elif kind == "notice-coverage":
                del inventory["noticeCoverage"]
            else:
                inventory["extraction"]["certificate"] = {
                    "offset": 1024, "size": 16, "sha256": "a" * 64, "authentication": "verified"}
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                tool.verify_documents([changed], {self.name: DIGEST}, SOURCE, self.authority)
        with self.assertRaisesRegex(ValueError, "committed package-profile authority"):
            tool.verify_documents([self.document], {self.name: DIGEST}, SOURCE)


def write_cabinet(path, members):
    """Independent, uncompressed CAB fixture; no external tool or package install."""
    content, entries, offset = b"", b"", 0
    for name, body in members:
        entries += struct.pack("<IIHHHH", len(body), offset, 0, 0, 0, 32) + name.encode("ascii") + b"\0"
        content += body
        offset += len(body)
    assert len(content) <= 32768
    data_offset = 44 + len(entries)
    data = struct.pack("<IHH", 0, len(content), len(content)) + content
    header = struct.pack("<4sIIIIIBBHHHHH", b"MSCF", 0, data_offset + len(data), 0, 44, 0,
                         3, 1, 1, len(members), 0, 1, 0)
    path.write_bytes(header + struct.pack("<IHH", data_offset, 1, 0) + entries + data)


def write_msi(path, cabinets, tables=None):
    """Create a disposable MSI database through the database API, never msiexec."""
    C, api = native.api.C, native.api
    dll = api._dll("msi.dll")
    handle = C.c_uint()
    open_db = api._function(dll, "MsiOpenDatabaseW", [C.c_wchar_p, C.c_void_p, C.POINTER(C.c_uint)])
    api.Database.check(open_db(str(path), C.c_void_p(3), C.byref(handle)), "create test database")
    db = object.__new__(api.Database)
    db.handle, db.dll = handle, dll
    db.close_handle = api._function(dll, "MsiCloseHandle", [C.c_uint])
    db.open_view = api._function(dll, "MsiDatabaseOpenViewW", [C.c_uint, C.c_wchar_p, C.POINTER(C.c_uint)])
    execute = api._function(dll, "MsiViewExecute", [C.c_uint, C.c_uint])

    def sql(command, record=0):
        view = C.c_uint()
        db.check(db.open_view(handle, command, C.byref(view)), "open test SELECT")
        try:
            db.check(execute(view, record), "execute test SQL")
        finally:
            db.close_handle(view)

    try:
        sql("CREATE TABLE `Directory` (`Directory` CHAR(72) NOT NULL, `Directory_Parent` CHAR(72), `DefaultDir` CHAR(255) NOT NULL PRIMARY KEY `Directory`)")
        sql("CREATE TABLE `Component` (`Component` CHAR(72) NOT NULL, `Directory_` CHAR(72) NOT NULL PRIMARY KEY `Component`)")
        sql("CREATE TABLE `File` (`File` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL, `FileSize` LONG NOT NULL, `Attributes` SHORT, `Sequence` SHORT NOT NULL PRIMARY KEY `File`)")
        sql("CREATE TABLE `Media` (`DiskId` SHORT NOT NULL, `LastSequence` SHORT NOT NULL, `Cabinet` CHAR(255), `Source` CHAR(72) PRIMARY KEY `DiskId`)")
        sql("CREATE TABLE `Feature` (`Feature` CHAR(38) NOT NULL, `Feature_Parent` CHAR(38), `Level` SHORT NOT NULL, `Attributes` SHORT PRIMARY KEY `Feature`)")
        sql("CREATE TABLE `FeatureComponents` (`Feature_` CHAR(38) NOT NULL, `Component_` CHAR(72) NOT NULL PRIMARY KEY `Feature_`, `Component_`)")
        fixture = FixtureDatabase()
        if tables:
            fixture.tables.update(tables)
        columns = {"Directory": ("Directory", "Directory_Parent", "DefaultDir"),
                   "Component": ("Component", "Directory_"),
                   "File": ("File", "Component_", "FileName", "FileSize", "Attributes", "Sequence"),
                   "Media": ("DiskId", "LastSequence", "Cabinet", "Source"),
                   "Feature": ("Feature", "Feature_Parent", "Level", "Attributes"),
                   "FeatureComponents": ("Feature_", "Component_")}
        for table in ("Directory", "Component", "File", "Media", "Feature", "FeatureComponents"):
            for row in fixture.tables[table]:
                values = ", ".join(value if value.isdigit() else "'" + value.replace("'", "''") + "'" for value in row)
                names = ", ".join(f"`{name}`" for name in columns[table])
                sql(f"INSERT INTO `{table}` ({names}) VALUES ({values})")
        create_record = api._function(dll, "MsiCreateRecord", [C.c_uint])
        set_string = api._function(dll, "MsiRecordSetStringW", [C.c_uint, C.c_uint, C.c_wchar_p])
        set_stream = api._function(dll, "MsiRecordSetStreamW", [C.c_uint, C.c_uint, C.c_wchar_p])
        for name, cabinet in cabinets.items():
            record = create_record(2)
            try:
                db.check(set_string(record, 1, name), "set test stream name")
                db.check(set_stream(record, 2, str(cabinet)), "set test stream")
                sql("INSERT INTO `_Streams` (`Name`, `Data`) VALUES (?, ?)", record)
            finally:
                db.close_handle(record)
        summary = C.c_uint()
        opening = api._function(dll, "MsiGetSummaryInformationW", [C.c_uint, C.c_wchar_p, C.c_uint, C.POINTER(C.c_uint)])
        db.check(opening(handle, None, 1, C.byref(summary)), "create test summary")
        try:
            setting = api._function(dll, "MsiSummaryInfoSetPropertyW", [C.c_uint, C.c_uint, C.c_uint, C.c_int, C.c_void_p, C.c_wchar_p])
            db.check(setting(summary, 15, 3, 2, None, None), "set test source type")
            db.check(api._function(dll, "MsiSummaryInfoPersist", [C.c_uint])(summary), "persist test summary")
        finally:
            db.close_handle(summary)
        db.check(api._function(dll, "MsiDatabaseCommit", [C.c_uint])(handle), "commit test fixture")
    finally:
        db.close_handle(handle)


@unittest.skipUnless(os.name == "nt", "requires read-only Windows MSI/cabinet APIs")
class NativeMsiWindowsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="spark-msi-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def artifact(self, first=None, optional=None, omit=False):
        first_cab, optional_cab = self.root / "first.cab", self.root / "optional.cab"
        write_cabinet(first_cab, [("Engine", b"engine"), ("Notice", b"notices")] if first is None else first)
        write_cabinet(optional_cab, [("Optional", b"optional")] if optional is None else optional)
        artifact = self.root / "SparkEngine-Runtime.msi"
        cabinets = {"first.cab": first_cab}
        if not omit:
            cabinets["optional.cab"] = optional_cab
        write_msi(artifact, cabinets)
        return artifact

    def test_real_two_cabinet_database_extracts_optional_and_binds_bytes(self):
        artifact = self.artifact()
        before = artifact.read_bytes()
        with native.extracted_payload(artifact) as (evidence, notice, outer, payload_files):
            self.assertEqual(outer, hashlib.sha256(before).hexdigest())
            self.assertEqual(notice.read_bytes(), b"notices")
            self.assertEqual(len(evidence["cabinets"]), 2)
            self.assertEqual(evidence["files"][-1]["sha256"], hashlib.sha256(b"optional").hexdigest())
        self.assertEqual(artifact.read_bytes(), before)

    def test_missing_extra_wrong_size_duplicate_and_unsafe_members_are_rejected(self):
        cases = [[], [("Other", b"optional")], [("Optional", b"wrong")],
                 [("Optional", b"optional"), ("Optional", b"optional")],
                 [("Optional", b"optional"), ("../escape", b"x")]]
        for optional in cases:
            with self.subTest(optional=optional):
                artifact = self.artifact(optional=optional)
                with self.assertRaises(ValueError):
                    with native.extracted_payload(artifact):
                        self.fail("incomplete cabinet was accepted")
                artifact.unlink()

    def test_missing_embedded_cabinet_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing embedded cabinet"):
            with native.extracted_payload(self.artifact(omit=True)):
                self.fail("missing cabinet was accepted")

    def test_original_and_extracted_mutation_are_rejected(self):
        for original in (True, False):
            with self.subTest(original=original):
                artifact = self.artifact()
                with self.assertRaisesRegex(ValueError, "mutat|changed"):
                    with native.extracted_payload(artifact) as (_, notice, _, _):
                        target = artifact if original else notice
                        with target.open("ab") as stream:
                            stream.write(b"mutation")
                artifact.unlink()

    def test_extractor_failure_and_private_copy_mutation_are_rejected(self):
        artifact = self.artifact()
        with patch.object(native.api, "extract_cabinet", side_effect=ValueError("extractor failed")):
            with self.assertRaisesRegex(ValueError, "extractor failed"):
                with native.extracted_payload(artifact):
                    self.fail("failed extractor accepted")
        with self.assertRaisesRegex(ValueError, "changed"):
            with native.extracted_payload(artifact) as (_, notice, _, _):
                with (notice.parent / "package.msi").open("ab") as stream:
                    stream.write(b"mutation")

    def test_cabinet_file_sequence_order_is_required(self):
        artifact = self.artifact(first=[("Notice", b"notices"), ("Engine", b"engine")])
        with self.assertRaisesRegex(ValueError, "sequence order"):
            with native.extracted_payload(artifact):
                self.fail("misordered cabinet accepted")

    def test_spanned_cabinet_header_and_nonregular_attributes_are_rejected(self):
        for offset, value in ((30, 1), (30, 2), (58, 0x400)):
            cabinet = self.root / "untrusted.cab"
            write_cabinet(cabinet, [("Entry", b"data")])
            data = bytearray(cabinet.read_bytes())
            struct.pack_into("<H", data, offset, value)
            cabinet.write_bytes(data)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                native.api.extract_cabinet(cabinet, {"Entry": (self.root / "target", 4)})
            self.assertFalse((self.root / "target").exists())

    def test_hardlinked_package_and_extra_extraction_output_fail(self):
        artifact = self.artifact()
        linked = self.root / "linked.msi"
        os.link(artifact, linked)
        with self.assertRaisesRegex(ValueError, "hard-linked"):
            with native.extracted_payload(artifact):
                self.fail("hardlinked package accepted")
        linked.unlink()
        with self.assertRaisesRegex(ValueError, "unexpected extracted"):
            with native.extracted_payload(artifact) as (_, notice, _, _):
                (notice.parent / "unexpected").write_bytes(b"unmapped")


if __name__ == "__main__":
    unittest.main()
