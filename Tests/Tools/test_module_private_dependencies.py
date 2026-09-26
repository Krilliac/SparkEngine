#!/usr/bin/env python3
"""MOD-295: engine-private dependency ratchet for prototype game modules.

module_content.classify_module_includes is the one include resolver shared by
every module-boundary ratchet (MOD-310 reuses it for SparkGameFPS). These tests
check the resolver against the include search order the module CMakeLists
declare, check that the committed inventory matches the live tree for every
prototype module, and mutate temporary copies of a real module so that each
kind of drift fails by name. The ratchet measures the "prototype modules no
longer copy private infrastructure" criterion; it does not satisfy it.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "site-data"))
import module_content  # noqa: E402

MUTATION_MODULE = "SparkGameARPG"
LOCATION = "inventory"


def _authoritative(root: Path) -> dict[str, dict]:
    evidence = json.loads((root / module_content.EVIDENCE_RELATIVE).read_text(encoding="utf-8"))
    return {module["name"]: module for module in evidence["modules"]}


def _committed_entries(root: Path) -> dict[str, dict]:
    inventory = json.loads((root / module_content.INVENTORY_RELATIVE).read_text(encoding="utf-8"))
    return {entry["name"]: entry for entry in inventory["modules"]}


class ResolverTests(unittest.TestCase):
    """The resolver follows the declared search order and classifies by resolved location."""

    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="module-includes-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.module = self.root / "GameModules" / "SparkGameProbe"
        for relative in (
            "SparkEngine/Source/Engine/Coroutine/CoroutineScheduler.h",
            "SparkEngine/Source/Utils/Logger.h",
            "SparkEngine/Source/Core/Shadowed.h",
            "SparkSDK/Include/Spark/IEngineContext.h",
            "GameModules/SparkGameProbe/Source/Core/Local.h",
            "GameModules/SparkGameProbe/Source/Core/Shadowed.h",
            "GameModules/SparkGameProbe/Source/Shared/Types.h",
        ):
            self._write(relative, "#pragma once\n")

    def _write(self, relative: str, text: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def classify(self, text: str) -> dict:
        self._write("GameModules/SparkGameProbe/Source/Core/Main.cpp", text)
        return module_content.classify_module_includes(self.root, self.module)

    def test_each_location_is_classified(self) -> None:
        result = self.classify(
            '#include "Local.h"\n'
            '#include "Shared/Types.h"\n'
            '#include "Engine/Coroutine/CoroutineScheduler.h"\n'
            "#include <Utils/Logger.h>\n"
            '#include "Spark/IEngineContext.h"\n'
            "#include <vector>\n"
        )
        self.assertEqual({"Source/Core/Local.h", "Source/Shared/Types.h"}, result["module"])
        self.assertEqual({"Engine/Coroutine/CoroutineScheduler.h", "Utils/Logger.h"}, result["engine"])
        self.assertEqual({"Spark/IEngineContext.h"}, result["sdk"])
        self.assertEqual([], result["unresolved"])

    def test_quoted_include_prefers_the_including_directory(self) -> None:
        # Core/Main.cpp sees Core/Shadowed.h next to it before any -I directory.
        self.assertEqual(set(), self.classify('#include "Shadowed.h"\n')["engine"])
        # The module "Source" directory is declared before ENGINE_SOURCE_DIR.
        result = self.classify('#include "Core/Shadowed.h"\n')
        self.assertEqual((set(), {"Source/Core/Shadowed.h"}), (result["engine"], result["module"]))
        # A spelling only the engine provides still reaches the engine.
        self._write("SparkEngine/Source/Core/EngineOnly.h", "#pragma once\n")
        self.assertEqual({"Core/EngineOnly.h"}, self.classify('#include "Core/EngineOnly.h"\n')["engine"])

    def test_relative_escape_into_engine_is_engine_private(self) -> None:
        result = self.classify('#include "../../../../SparkEngine/Source/Utils/Logger.h"\n')
        self.assertEqual({"Utils/Logger.h"}, result["engine"])

    def test_comments_and_string_literals_are_not_directives(self) -> None:
        result = self.classify(
            '// #include "Utils/Logger.h"\n'
            '/* #include "Utils/Logger.h" */\n'
            'const char* text = "#include \\"Utils/Logger.h\\"";\n'
        )
        self.assertEqual(set(), result["engine"])

    def test_spaced_and_conditional_directives_are_counted(self) -> None:
        result = self.classify('#if SOME_FLAG\n  #  include   "Utils/Logger.h"\n#endif\n')
        self.assertEqual({"Utils/Logger.h"}, result["engine"])

    def test_unresolvable_quoted_include_is_reported(self) -> None:
        result = self.classify('#include "Nowhere/Missing.h"\n')
        self.assertEqual([("GameModules/SparkGameProbe/Source/Core/Main.cpp", "Nowhere/Missing.h")], result["unresolved"])


class CommittedInventoryTests(unittest.TestCase):
    """Every prototype module's committed entry matches the live tree; the stable profile carries none."""

    def test_prototype_modules_match_committed_inventory(self) -> None:
        authoritative, entries = _authoritative(ROOT), _committed_entries(ROOT)
        prototypes = [name for name, module in authoritative.items() if module_content._is_prototype(module["profileApplicability"])]
        self.assertGreaterEqual(len(prototypes), 1)
        for name in prototypes:
            with self.subTest(module=name):
                self.assertIn(name, entries)
                findings = module_content._validate_private_dependencies(ROOT, ROOT / "GameModules" / name, entries[name], LOCATION)
                self.assertEqual([], findings)

    def test_stable_profile_modules_are_not_prototypes(self) -> None:
        entries = _committed_entries(ROOT)
        for name, module in _authoritative(ROOT).items():
            if module_content._is_prototype(module["profileApplicability"]):
                continue
            with self.subTest(module=name):
                self.assertFalse(set(module_content.PRIVATE_DEPENDENCY_KEYS) & set(entries[name]))

    def test_copied_infrastructure_is_measured(self) -> None:
        entry = _committed_entries(ROOT)[MUTATION_MODULE]
        self.assertEqual([f"GameModules/{MUTATION_MODULE}/Source/Core/ARPGEngineSystems.cpp"], entry["copiedInfrastructureFiles"])
        self.assertIn("Engine/Coroutine/CoroutineScheduler.h", entry["privateEngineHeaders"])


class RatchetMutationTests(unittest.TestCase):
    """Temporary copies of a real module: each kind of drift must fail by name."""

    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="module-private-ratchet-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.entry = json.loads(json.dumps(_committed_entries(ROOT)[MUTATION_MODULE]))
        self.module = self.root / "GameModules" / MUTATION_MODULE
        shutil.copytree(ROOT / "GameModules" / MUTATION_MODULE / "Source", self.module / "Source")
        shutil.copytree(ROOT / module_content.SDK_INCLUDE_ROOT, self.root / module_content.SDK_INCLUDE_ROOT)
        # Only the engine headers the committed entry names, plus the mutation
        # target, so the copy stays small and resolution stays exact.
        for header in self.entry["privateEngineHeaders"] + ["Graphics/Shader.h"]:
            target = self.root / module_content.ENGINE_PRIVATE_ROOT / header
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / module_content.ENGINE_PRIVATE_ROOT / header, target)

    def findings(self) -> list[str]:
        return [message for _, message in module_content._validate_private_dependencies(self.root, self.module, self.entry, LOCATION)]

    def _source(self, name: str) -> Path:
        return self.module / "Source" / "Core" / name

    def test_unmodified_copy_is_clean(self) -> None:
        self.assertEqual([], self.findings())

    def test_added_engine_include_fails(self) -> None:
        main = self._source("Main.cpp")
        main.write_text('#include "Graphics/Shader.h"\n' + main.read_text(encoding="utf-8"), encoding="utf-8")
        self.assertIn(
            f"{MUTATION_MODULE} gained engine-private header not listed in its committed inventory: Graphics/Shader.h",
            self.findings(),
        )

    def test_unlisting_a_header_without_removing_the_include_fails(self) -> None:
        header = "Engine/Coroutine/CoroutineScheduler.h"
        self.entry["privateEngineHeaders"].remove(header)
        self.entry["privateEngineHeaderCount"] -= 1
        self.assertIn(f"{MUTATION_MODULE} gained engine-private header not listed in its committed inventory: {header}", self.findings())

    def test_removed_include_requires_the_list_to_shrink(self) -> None:
        header = "Engine/Coroutine/CoroutineScheduler.h"
        for path in (self.module / "Source").rglob("*"):
            if path.is_file() and path.suffix in module_content.INCLUDE_SOURCE_SUFFIXES:
                text = path.read_text(encoding="utf-8")
                path.write_text(text.replace(f'#include "{header}"', ""), encoding="utf-8")
        self.assertIn(f"{MUTATION_MODULE} no longer includes engine-private header {header}; shrink privateEngineHeaders", self.findings())

    def test_count_must_match_the_list(self) -> None:
        self.entry["privateEngineHeaderCount"] += 1
        self.assertTrue(any("privateEngineHeaderCount" in message for message in self.findings()))

    def test_unsorted_list_is_rejected(self) -> None:
        self.entry["privateEngineHeaders"].reverse()
        self.assertTrue(any("must be sorted and unique" in message for message in self.findings()))

    def test_new_copied_infrastructure_file_fails(self) -> None:
        shutil.copy2(self._source("ARPGEngineSystems.cpp"), self._source("ExtraEngineSystems.cpp"))
        self.assertTrue(any(message.startswith(f"copiedInfrastructureFiles drift for {MUTATION_MODULE}") for message in self.findings()))

    def test_unclassifiable_include_fails(self) -> None:
        main = self._source("Main.cpp")
        main.write_text('#include "Nowhere/Missing.h"\n' + main.read_text(encoding="utf-8"), encoding="utf-8")
        self.assertTrue(any('unclassifiable include' in message and "Nowhere/Missing.h" in message for message in self.findings()))


class ValidateIntegrationTests(unittest.TestCase):
    """module_content.validate() applies the ratchet and generate() publishes it."""

    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="module-private-validate-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        evidence = self.root / module_content.EVIDENCE_RELATIVE
        evidence.parent.mkdir(parents=True)
        evidence.write_text(json.dumps({
            "schemaVersion": "stable-v2",
            "profiles": [{"id": "stable-v1", "includedModules": [], "excludedModules": ["SparkGameProbe"]}],
            "modules": [{"name": "SparkGameProbe", "profileApplicability": {"stable-v1": "outside"}}],
        }), encoding="utf-8")
        self.main = self.root / "GameModules" / "SparkGameProbe" / "Source" / "Main.cpp"
        self.main.parent.mkdir(parents=True)
        self.main.write_text('#include "Utils/Logger.h"\n', encoding="utf-8")
        for header in ("Utils/Logger.h", "Graphics/Shader.h"):
            path = self.root / module_content.ENGINE_PRIVATE_ROOT / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#pragma once\n", encoding="utf-8")
        inventory = self.root / module_content.INVENTORY_RELATIVE
        inventory.write_text(json.dumps(module_content.generate(self.root), indent=2) + "\n", encoding="utf-8")

    def ratchet_messages(self) -> list[str]:
        return [message for _, message in module_content.validate(self.root) if "engine-private" in message]

    def test_generated_entry_publishes_the_ratchet(self) -> None:
        entry = _committed_entries(self.root)["SparkGameProbe"]
        self.assertEqual(["Utils/Logger.h"], entry["privateEngineHeaders"])
        self.assertEqual(1, entry["privateEngineHeaderCount"])
        self.assertEqual([], entry["copiedInfrastructureFiles"])
        self.assertEqual([], self.ratchet_messages())

    def test_validate_rejects_a_gained_header(self) -> None:
        self.main.write_text('#include "Utils/Logger.h"\n#include "Graphics/Shader.h"\n', encoding="utf-8")
        self.assertEqual(
            ["SparkGameProbe gained engine-private header not listed in its committed inventory: Graphics/Shader.h"],
            self.ratchet_messages(),
        )


if __name__ == "__main__":
    unittest.main()
