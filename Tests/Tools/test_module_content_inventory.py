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
            "modules": [{"name": "SparkGameFPS", "profileApplicability": {"stable-v1": "required"}}],
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
        integrity.write_text(json.dumps({"entries": [{"path": "Models/model.obj"}, {"path": "Scenes/scene.scene"}]}), encoding="utf-8")
        # The fixture profile has one module, so the evidence partition is exact.
        # Its empty excluded list is corrected after the module directory exists.
        data = json.loads(evidence.read_text(encoding="utf-8"))
        data["profiles"][0]["excludedModules"] = []
        evidence.write_text(json.dumps(data), encoding="utf-8")
        self.write(module_content.generate(self.root))

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

    def test_profile_classification_drift_is_rejected(self) -> None:
        payload = self.payload()
        payload["modules"][0]["profileApplicability"] = {"stable-v1": "outside"}
        self.write(payload)
        self.assertTrue(any("profileApplicability drift" in message for message in self.messages()))


if __name__ == "__main__":
    unittest.main()
