from __future__ import annotations

import copy
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "site-data"))
import module_content  # noqa: E402
import assets as site_assets  # noqa: E402
from unittest import mock


def _evidence_modules(root: Path) -> dict:
    evidence = json.loads((root / module_content.EVIDENCE_RELATIVE).read_text(encoding="utf-8"))
    return {module["name"]: module for module in evidence["modules"]}


class ModuleContentInventoryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="module-content-inventory-")
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        evidence = self.root / "tools" / "module-evidence" / "manifest.json"
        evidence.parent.mkdir(parents=True)
        evidence.write_text(json.dumps({
            "schemaVersion": "stable-v2",
            "profiles": [{"id": "stable-v1", "includedModules": ["SparkGameFPS"], "excludedModules": []}],
            "modules": [{
                "name": "SparkGameFPS",
                "cmakeTarget": "SparkGameFPS",
                "sourceDirectory": "GameModules/SparkGameFPS/Source",
                "profileApplicability": {"stable-v1": "required"},
            }],
        }), encoding="utf-8")
        (self.root / "GameModules" / "SparkGameFPS" / "Source").mkdir(parents=True)
        (self.root / "GameModules" / "SparkGameFPS" / "Source" / "Main.cpp").write_text("// source\n", encoding="utf-8")
        (self.root / "GameModules" / "SparkGameFPS" / "CMakeLists.txt").write_text("add_custom_command(copy_directory Assets/Models)\nadd_custom_command(copy_directory Assets/Scenes)\n", encoding="utf-8")
        (self.root / "GameModules" / "SparkGameFPS" / "Assets").mkdir()
        (self.root / "GameModules" / "SparkGameFPS" / "Assets" / "manifest.json").write_text("{}\n", encoding="utf-8")
        for relative in ("Assets/Models/model.obj", "Assets/Scenes/scene.scene"):
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("fixture\n", encoding="utf-8")
        integrity = self.root / "Assets" / "assets.integrity.json"
        integrity.write_text(json.dumps({"root": "Assets", "entries": [{"path": "Models/model.obj"}, {"path": "Scenes/scene.scene"}]}), encoding="utf-8")
        self._write_module_manifest_fixture()
        # The fixture profile has one module, so the evidence partition is exact.
        # Its empty excluded list is corrected after the module directory exists.
        data = json.loads(evidence.read_text(encoding="utf-8"))
        data["profiles"][0]["excludedModules"] = []
        evidence.write_text(json.dumps(data), encoding="utf-8")
        self.write(module_content.generate(self.root))

    def _write_module_manifest_fixture(self) -> None:
        """MOD-290: the per-module module.json and everything it references."""
        module_dir = self.root / "GameModules" / "SparkGameFPS"
        (module_dir / "README.md").write_text("# SparkGameFPS\n", encoding="utf-8")
        (self.root / "Tests").mkdir()
        (self.root / "Tests" / "CMakeLists.txt").write_text("add_executable(SparkTests\n    TestFixtureFPS.cpp\n)\n", encoding="utf-8")
        (self.root / "Tests" / "TestFixtureFPS.cpp").write_text("TEST(FPSFixture_Runs)\n{\n}\n", encoding="utf-8")
        work_items = self.root / "docs" / "readiness" / "work-items"
        work_items.mkdir(parents=True)
        (work_items / "30-game-modules.json").write_text(json.dumps({
            "parityDimensions": {"dimensions": ["networking"], "currentScores": {"SparkGameFPS": [1]}},
        }), encoding="utf-8")
        (module_dir / module_content.MODULE_MANIFEST_NAME).write_text(json.dumps({
            "schemaVersion": 1,
            "name": "SparkGameFPS",
            "cmakeTarget": "SparkGameFPS",
            "sourceDirectory": "GameModules/SparkGameFPS/Source",
            "assets": {"state": "declared", "roots": [
                {"directory": "Assets/Models", "manifest": "Assets/assets.integrity.json"},
                {"directory": "Assets/Scenes", "manifest": "Assets/assets.integrity.json"},
            ]},
            "tests": {"files": ["Tests/TestFixtureFPS.cpp"], "prefixes": [{"prefix": "FPSFixture_", "count": 1}]},
            "docs": {"readme": "GameModules/SparkGameFPS/README.md"},
            "parity": {"notApplicable": []},
        }), encoding="utf-8")

    def write(self, payload: dict) -> None:
        path = self.root / module_content.INVENTORY_RELATIVE
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    def payload(self) -> dict:
        return json.loads((self.root / module_content.INVENTORY_RELATIVE).read_text(encoding="utf-8"))

    def messages(self) -> list[str]:
        return [message for _, message in module_content.validate(self.root)]

    def test_generated_inventory_is_clean(self) -> None:
        self.assertEqual([], self.messages())

    def test_missing_module_entry_is_rejected(self) -> None:
        payload = self.payload()
        payload["modules"] = []
        self.write(payload)
        self.assertTrue(any("module directory is undeclared" in message for message in self.messages()))

    def test_missing_module_asset_manifest_is_rejected(self) -> None:
        (self.root / "GameModules" / "SparkGameFPS" / "Assets" / "payload.bin").write_bytes(b"payload")
        (self.root / "GameModules" / "SparkGameFPS" / "Assets" / "manifest.json").unlink()
        self.assertTrue(any("asset directory has no manifest" in message for message in self.messages()))

    def test_empty_asset_directory_is_allowed(self) -> None:
        (self.root / "GameModules" / "SparkGameFPS" / "Assets" / "manifest.json").unlink()
        self.assertFalse(any("payload asset directory has no manifest" in message for message in self.messages()))

    def test_missing_or_malformed_evidence_is_rejected(self) -> None:
        evidence = self.root / "tools" / "module-evidence" / "manifest.json"
        evidence.unlink()
        self.assertTrue(any("manifest is missing" in message for message in self.messages()))
        evidence.write_text("{broken", encoding="utf-8")
        self.assertTrue(any("malformed" in message for message in self.messages()))

    def test_authoritative_profile_mismatch_is_rejected(self) -> None:
        evidence = self.root / "tools" / "module-evidence" / "manifest.json"
        data = json.loads(evidence.read_text(encoding="utf-8"))
        data["profiles"][0]["includedModules"] = []
        data["profiles"][0]["excludedModules"] = ["SparkGameFPS"]
        data["modules"][0]["profileApplicability"] = {"stable-v1": "outside"}
        evidence.write_text(json.dumps(data), encoding="utf-8")
        messages = self.messages()
        self.assertTrue(any("stable-v1 must include exactly" in message for message in messages), messages)

    def test_authoritative_profile_keys_must_be_exact(self) -> None:
        evidence = self.root / "tools" / "module-evidence" / "manifest.json"
        data = json.loads(evidence.read_text(encoding="utf-8"))
        data["modules"][0]["profileApplicability"]["unexpected"] = "outside"
        evidence.write_text(json.dumps(data), encoding="utf-8")
        self.assertTrue(any("keys must exactly match" in message for message in self.messages()))

    def test_fps_root_dependencies_are_inventoried(self) -> None:
        entry = self.payload()["modules"][0]
        self.assertEqual("root-dependent", entry["moduleLocalContentState"])
        self.assertEqual("shipping", entry["releaseClassification"])
        self.assertEqual(["Assets/Models", "Assets/Scenes"], [item["path"] for item in entry["sharedRootDependencies"]])

    def test_fps_root_declaration_deletion_or_comment_fails_after_regeneration(self) -> None:
        cmake = self.root / "GameModules" / "SparkGameFPS" / "CMakeLists.txt"
        cmake.write_text("# add_custom_command(copy_directory Assets/Models)\n# add_custom_command(copy_directory Assets/Scenes)\n", encoding="utf-8")
        self.write(module_content.generate(self.root))
        messages = self.messages()
        self.assertTrue(any("build declaration is missing" in message for message in messages), messages)

    def test_multiline_bracket_comments_cannot_fake_fps_declarations(self) -> None:
        cmake = self.root / "GameModules" / "SparkGameFPS" / "CMakeLists.txt"
        cmake.write_text(
            "#[[ add_custom_command(\n copy_directory Assets/Models\n) ]]\n"
            "#[=[ add_custom_command(\n copy_directory Assets/Scenes\n) ]=]\n",
            encoding="utf-8",
        )
        self.write(module_content.generate(self.root))
        messages = self.messages()
        self.assertEqual(2, sum("build declaration is missing" in message for message in messages), messages)

    def test_add_custom_command_declarations_are_not_cross_command_matched(self) -> None:
        cmake = self.root / "GameModules" / "SparkGameFPS" / "CMakeLists.txt"
        cmake.write_text(
            "add_custom_command(TARGET Foo COMMAND echo Assets/Models)\n"
            "add_custom_command(TARGET Bar COMMAND copy_directory Assets/Scenes)\n",
            encoding="utf-8",
        )
        self.write(module_content.generate(self.root))
        messages = self.messages()
        self.assertTrue(any("build declaration is missing: Assets/Models" in message for message in messages), messages)

    def test_stage_helper_copy_directories_count_as_fps_declarations(self) -> None:
        cmake = self.root / "GameModules" / "SparkGameFPS" / "CMakeLists.txt"
        cmake.write_text(
            'spark_stage_game_module_content(SparkGameFPS COPY_DIRECTORIES "${ROOT}/Assets/Models" Assets/Models)\n'
            "spark_stage_game_module_content(SparkGameFPS DIRECTORIES Assets/Scenes)\n",
            encoding="utf-8",
        )
        self.write(module_content.generate(self.root))
        messages = self.messages()
        self.assertFalse(any("build declaration is missing: Assets/Models" in message for message in messages), messages)
        self.assertTrue(any("build declaration is missing: Assets/Scenes" in message for message in messages), messages)

    def test_unterminated_add_custom_command_fails_after_regeneration(self) -> None:
        cmake = self.root / "GameModules" / "SparkGameFPS" / "CMakeLists.txt"
        cmake.write_text("add_custom_command(\n copy_directory Assets/Models\n", encoding="utf-8")
        self.write(module_content.generate(self.root))
        messages = self.messages()
        self.assertTrue(any("unbalanced parentheses" in message for message in messages), messages)

    def _write_module_asset(self, name: str, payload: bytes, manifest_path: str = "manifest.json") -> list[str]:
        assets = self.root / "GameModules" / "SparkGameFPS" / "Assets"
        target = assets / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        manifest = assets / manifest_path
        manifest.write_text(json.dumps({
            "manifestVersion": 1,
            "package": "SparkGameFPS",
            "license": "Spark Open License 1.0",
            "assets": [{"path": name, "origin": "fixture", "sha256": hashlib.sha256(payload).hexdigest()}],
        }), encoding="utf-8")
        return [f"GameModules/SparkGameFPS/Assets/{name}", f"GameModules/SparkGameFPS/Assets/{manifest_path}"]

    def _asset_messages(self, tracked: list[str]) -> list[str]:
        with mock.patch.object(site_assets, "REPO_ROOT", self.root):
            return [message for _, message in site_assets.validate_package("GameModules/SparkGameFPS/Assets", frozenset(tracked))]

    def test_module_asset_manifest_case_and_traversal_are_rejected(self) -> None:
        tracked = self._write_module_asset("Payload.bin", b"payload")
        manifest = self.root / "GameModules" / "SparkGameFPS" / "Assets" / "manifest.json"
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["assets"][0]["path"] = "payload.bin"
        manifest.write_text(json.dumps(data), encoding="utf-8")
        messages = self._asset_messages(tracked)
        self.assertTrue(any("resolves to no tracked file" in message for message in messages), messages)
        data["assets"][0]["path"] = "../escape.bin"
        manifest.write_text(json.dumps(data), encoding="utf-8")
        self.assertTrue(any("traverses outside" in message for message in self._asset_messages(tracked)))

    def test_module_asset_undeclared_and_symlink_payload_are_rejected(self) -> None:
        tracked = self._write_module_asset("payload.bin", b"payload")
        (self.root / "GameModules" / "SparkGameFPS" / "Assets" / "undeclared.bin").write_bytes(b"extra")
        messages = self._asset_messages(tracked + ["GameModules/SparkGameFPS/Assets/undeclared.bin"])
        self.assertTrue(any("no manifest declares it" in message for message in messages), messages)
        outside = self.root / "outside.bin"
        outside.write_bytes(b"outside")
        link = self.root / "GameModules" / "SparkGameFPS" / "Assets" / "linked.bin"
        try:
            link.symlink_to(outside)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"symlink unavailable: {error}")
        manifest = self.root / "GameModules" / "SparkGameFPS" / "Assets" / "manifest.json"
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["assets"][0] = {"path": "linked.bin", "origin": "fixture", "sha256": hashlib.sha256(b"outside").hexdigest()}
        manifest.write_text(json.dumps(data), encoding="utf-8")
        messages = self._asset_messages(tracked + ["GameModules/SparkGameFPS/Assets/linked.bin"])
        self.assertTrue(any("not a regular file" in message or "reparse" in message for message in messages), messages)

    def test_two_generations_are_byte_identical(self) -> None:
        first = json.dumps(module_content.generate(self.root), indent=2) + "\n"
        second = json.dumps(module_content.generate(self.root), indent=2) + "\n"
        self.assertEqual(first, second)

    def test_assets_mode_has_one_authoritative_asset_walk(self) -> None:
        validate_source = (ROOT / "tools" / "site-data" / "validate.py").read_text(encoding="utf-8")
        self.assertEqual(1, validate_source.count("for location, message in validate_assets():"))

    def test_undeclared_module_asset_directory_is_rejected(self) -> None:
        payload = self.payload()
        payload["modules"][0]["assetsDirectory"] = None
        payload["modules"][0]["assetManifest"] = None
        payload["modules"][0]["assetFileCount"] = 0
        self.write(payload)
        messages = self.messages()
        self.assertTrue(any("assetsDirectory drift" in message for message in messages), messages)

    def test_directory_traversal_is_rejected(self) -> None:
        payload = self.payload()
        payload["modules"][0]["directory"] = "GameModules/../Assets"
        self.write(payload)
        self.assertTrue(any("directory drift" in message for message in self.messages()))

    def test_symlink_module_boundary_is_rejected(self) -> None:
        link = self.root / "GameModules" / "SparkGameFPS" / "Source" / "Linked"
        outside = self.root / "outside"
        outside.mkdir()
        try:
            link.symlink_to(outside, target_is_directory=True)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"symlink unavailable: {error}")
        # The inventory's declared source count now observes an unsafe child;
        # validation must not silently certify through the reparse boundary.
        self.assertTrue(any("symlink or reparse" in message for message in self.messages()))

    def test_duplicate_and_case_drift_are_rejected(self) -> None:
        payload = self.payload()
        duplicate = copy.deepcopy(payload["modules"][0])
        duplicate["name"] = "sparkgamefps"
        payload["modules"].append(duplicate)
        self.write(payload)
        messages = self.messages()
        self.assertTrue(any("differs only by case" in message for message in messages), messages)
        self.assertTrue(any("module directory is missing" in message for message in messages), messages)

    # RDY-020: fallback-policy inventory for in-profile modules.

    SUBSTITUTION_SOURCE = (
        'void Load()\n{\n'
        '    m_rifleModel->LoadObj(Resolve(L"Models/rifle.obj"), device);\n'
        '    m_shotgunModel->LoadObj(Resolve(L"Models/rifle.obj"), device);\n'
        '}\n'
    )

    def _write_source(self, relative: str, text: str, module: str = "SparkGameFPS") -> None:
        path = self.root / "GameModules" / module / "Source" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def _declare(self, **overrides: object) -> dict:
        entry = {
            "module": "SparkGameFPS",
            "kind": "model-substitution",
            "symbol": "shotgun -> rifle.obj",
            "files": ["GameModules/SparkGameFPS/Source/Player.cpp"],
            "policy": "tracked",
            "owner": "MOD-310",
            "reason": "Shotgun renders the rifle model until the authored shotgun.obj is wired in.",
        }
        entry.update(overrides)
        payload = module_content.generate(self.root)
        payload["fallbackSites"] = [{key: value for key, value in entry.items() if value is not None}]
        self.write(payload)
        return entry

    def _add_owner_work_item(self) -> None:
        path = self.root / "docs" / "readiness" / "work-items" / "30-game-modules.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        data["workItems"] = [{"id": "MOD-310"}]
        path.write_text(json.dumps(data), encoding="utf-8")

    def test_undeclared_model_substitution_is_rejected(self) -> None:
        self._write_source("Player.cpp", self.SUBSTITUTION_SOURCE)
        messages = self.messages()
        self.assertTrue(any("undeclared fallback site" in message and "shotgun -> rifle.obj" in message for message in messages), messages)
        self.assertFalse(any("rifle -> rifle.obj" in message for message in messages), messages)

    def test_declared_tracked_substitution_passes(self) -> None:
        self._add_owner_work_item()
        self._write_source("Player.cpp", self.SUBSTITUTION_SOURCE)
        self._declare()
        self.assertEqual([], self.messages())

    def test_regeneration_keeps_reviewed_policy_and_never_invents_one(self) -> None:
        self._add_owner_work_item()
        self._write_source("Player.cpp", self.SUBSTITUTION_SOURCE)
        declared = self._declare()
        self._write_source("Extra.cpp", "int UseFallbackMesh();\n")
        self.write(module_content.generate(self.root))
        self.assertEqual([declared], self.payload()["fallbackSites"])
        self.assertTrue(any("undeclared fallback site" in message and "UseFallbackMesh" in message for message in self.messages()))

    def test_tracked_policy_requires_existing_owner_work_item(self) -> None:
        self._write_source("Player.cpp", self.SUBSTITUTION_SOURCE)
        self._declare()
        self.assertTrue(any("existing work item: 'MOD-310'" in message for message in self.messages()))
        self._add_owner_work_item()
        self._declare(owner=None)
        self.assertTrue(any("existing work item: None" in message for message in self.messages()))

    def test_invalid_policy_short_reason_and_stray_owner_are_rejected(self) -> None:
        self._add_owner_work_item()
        self._write_source("Player.cpp", self.SUBSTITUTION_SOURCE)
        self._declare(policy="accepted")
        self.assertTrue(any("policy must be one of" in message for message in self.messages()))
        self._declare(policy="intentional")
        self.assertTrue(any("only a tracked fallback site may name an owner" in message for message in self.messages()))
        self._declare(reason="todo")
        self.assertTrue(any("reason must be a written explanation" in message for message in self.messages()))

    def test_stale_declaration_and_file_drift_are_rejected(self) -> None:
        self._add_owner_work_item()
        self._write_source("Player.cpp", self.SUBSTITUTION_SOURCE)
        self._declare(files=["GameModules/SparkGameFPS/Source/Other.cpp"])
        self.assertTrue(any("files drift for fallback site" in message for message in self.messages()))
        self._write_source("Player.cpp", self.SUBSTITUTION_SOURCE.replace("rifle.obj\"), device);\n}", "shotgun.obj\"), device);\n}"))
        self._declare()
        self.assertTrue(any("no longer exists in in-profile sources" in message for message in self.messages()))

    def test_fallback_and_procedural_helpers_are_detected_outside_comments_and_strings(self) -> None:
        self._write_source(
            "Helpers.cpp",
            "RespawnPoint MakeFallbackSpawnPoint();\n"
            "std::string ProceduralMaterialFor(const wchar_t* path);\n"
            "// UseFallbackTexture(path);\n"
            "/* BuildProceduralMesh(); */\n"
            "Log(\"using FallbackModel(\");\n"
            "RespawnPoint fallback; fallback.name = \"Fallback\";\n",
        )
        symbols = {site["symbol"] for site in module_content.scan_fallback_sites(self.root, ["SparkGameFPS"])}
        self.assertEqual({"MakeFallbackSpawnPoint", "ProceduralMaterialFor"}, symbols)

    def test_leading_fallback_and_procedural_identifiers_are_detected(self) -> None:
        self._write_source("Leading.cpp", "Mesh m = FallbackMesh(path);\nFallback(path);\nProcedural(seed);\n")
        symbols = {site["symbol"] for site in module_content.scan_fallback_sites(self.root, ["SparkGameFPS"])}
        self.assertEqual({"FallbackMesh", "Fallback", "Procedural"}, symbols)

    def test_comment_markers_inside_literals_cannot_hide_code(self) -> None:
        self._write_source(
            "Literals.cpp",
            'Log("x//y"); auto z = MakeFallbackZ(1);\n'
            'Log("open /* here"); UseFallbackBlock();\n'
            'Log(R"raw(a // b /* c)raw"); UseFallbackRaw();\n'
            "char quote = '\"'; UseFallbackChar();\n"
            "int count = 1'000; UseFallbackNumber();\n"
            "/* \"UseFallbackCommented();\" */ // UseFallbackLine();\n",
        )
        symbols = {site["symbol"] for site in module_content.scan_fallback_sites(self.root, ["SparkGameFPS"])}
        self.assertEqual({"MakeFallbackZ", "UseFallbackBlock", "UseFallbackRaw", "UseFallbackChar", "UseFallbackNumber"}, symbols)

    def test_raw_and_prefixed_string_model_substitutions_are_detected(self) -> None:
        self._write_source(
            "Player.cpp",
            'm_rocketModel->LoadObj(R"(Models/rifle.obj)", device);\n'
            'm_grenadeModel->LoadObj(LR"obj(Models/rifle.obj)obj", device);\n'
            'm_shotgunModel->LoadObj(u8"Models/rifle.obj", device);\n'
            'm_rifleModel->LoadObj(R"(Models/rifle.obj)", device);\n',
        )
        symbols = {site["symbol"] for site in module_content.scan_fallback_sites(self.root, ["SparkGameFPS"])}
        self.assertEqual({"rocket -> rifle.obj", "grenade -> rifle.obj", "shotgun -> rifle.obj"}, symbols)

    def test_non_string_fallback_site_fields_are_reported_not_raised(self) -> None:
        self._write_source("Player.cpp", self.SUBSTITUTION_SOURCE)
        for field, value in (("module", ["SparkGameFPS"]), ("symbol", {"a": 1}), ("policy", ["tracked"]), ("files", "Player.cpp"), ("files", [1])):
            self._declare(**{field: value})
            messages = self.messages()
            self.assertTrue(any("must be strings and files a list of strings" in message for message in messages), (field, messages))

    def test_out_of_profile_module_sites_are_not_required(self) -> None:
        evidence = self.root / module_content.EVIDENCE_RELATIVE
        data = json.loads(evidence.read_text(encoding="utf-8"))
        data["modules"].append({"name": "SparkGameOther", "profileApplicability": {"stable-v1": "outside"}})
        evidence.write_text(json.dumps(data), encoding="utf-8")
        self._write_source("Player.cpp", self.SUBSTITUTION_SOURCE, module="SparkGameOther")
        in_profile = module_content._in_profile_modules(_evidence_modules(self.root))
        self.assertEqual(["SparkGameFPS"], in_profile)
        self.assertEqual([], module_content.scan_fallback_sites(self.root, in_profile))
        self.assertEqual(1, len(module_content.scan_fallback_sites(self.root, ["SparkGameOther"])))

    def test_missing_fallback_sites_list_is_rejected(self) -> None:
        payload = self.payload()
        del payload["fallbackSites"]
        self.write(payload)
        self.assertTrue(any("fallbackSites must be a list" in message for message in self.messages()))

    def test_repository_fallback_inventory_matches_fps_sources(self) -> None:
        inventory = json.loads((ROOT / module_content.INVENTORY_RELATIVE).read_text(encoding="utf-8"))
        findings = module_content.validate_fallback_sites(ROOT, inventory.get("fallbackSites"), _evidence_modules(ROOT))
        self.assertEqual([], findings)
        substitutions = {site["symbol"]: site for site in inventory["fallbackSites"] if site["kind"] == "model-substitution"}
        self.assertEqual({"grenade -> rifle.obj", "rocket -> rifle.obj", "shotgun -> rifle.obj"}, set(substitutions))
        self.assertTrue(all(site["policy"] == "tracked" and site["owner"] == "MOD-310" for site in substitutions.values()))

    def test_profile_classification_drift_is_rejected(self) -> None:
        payload = self.payload()
        payload["modules"][0]["profileApplicability"] = {"stable-v1": "outside"}
        self.write(payload)
        self.assertTrue(any("profileApplicability drift" in message for message in self.messages()))


if __name__ == "__main__":
    unittest.main()
