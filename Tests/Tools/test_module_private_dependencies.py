#!/usr/bin/env python3
"""MOD-295 / MOD-310: engine-private dependency ratchet for game modules.

module_content.classify_module_includes is the one include resolver shared by
every module-boundary ratchet. These tests check the resolver against the
include search order the module CMakeLists declare, check that the committed
inventory matches the live tree for every prototype module and for the
RATCHETED_RELEASE_MODULES (SparkGameFPS), and mutate temporary copies of real
modules so that each kind of drift fails by name.

The ratchet measures the "prototype modules no longer copy private
infrastructure" (MOD-295) and "the module builds without SparkEngineLib or
engine-source include paths" (MOD-310) criteria; it does not satisfy them.
SparkGameFPS additionally has a reviewed one-way ceiling below, which
regenerating the inventory cannot raise.

FPSPrivateIncludeRatchetTests and FPSBuildCouplingTests are also registered as the CTest
FPSPublicSDK_PrivateIncludeRatchet.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "site-data"))
import module_content  # noqa: E402

MUTATION_MODULE = "SparkGameARPG"
FPS_MODULE = "SparkGameFPS"
LOCATION = "inventory"

# Reviewed one-way ceilings for SparkGameFPS (MOD-310 target: zero). Lower them
# together with the committed inventory whenever headers or copied files are
# removed; never raise them.
FPS_PRIVATE_HEADER_CEILING = 44
FPS_COPIED_INFRASTRUCTURE_CEILING = 1


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
    """Every ratcheted module's committed entry matches the live tree; no other module carries the fields."""

    def test_prototype_modules_match_committed_inventory(self) -> None:
        authoritative, entries = _authoritative(ROOT), _committed_entries(ROOT)
        prototypes = [name for name, module in authoritative.items() if module_content._is_prototype(module["profileApplicability"])]
        self.assertGreaterEqual(len(prototypes), 1)
        for name in prototypes:
            with self.subTest(module=name):
                self.assertIn(name, entries)
                findings = module_content._validate_private_dependencies(ROOT, ROOT / "GameModules" / name, entries[name], LOCATION)
                self.assertEqual([], findings)

    def test_only_ratcheted_modules_publish_fields(self) -> None:
        entries = _committed_entries(ROOT)
        for name, module in _authoritative(ROOT).items():
            ratcheted = module_content._is_ratcheted(name, module["profileApplicability"])
            with self.subTest(module=name, ratcheted=ratcheted):
                published = set(module_content.PRIVATE_DEPENDENCY_KEYS) & set(entries[name])
                self.assertEqual(set(module_content.PRIVATE_DEPENDENCY_KEYS) if ratcheted else set(), published)

    def test_copied_infrastructure_is_measured(self) -> None:
        entry = _committed_entries(ROOT)[MUTATION_MODULE]
        self.assertEqual([f"GameModules/{MUTATION_MODULE}/Source/Core/ARPGEngineSystems.cpp"], entry["copiedInfrastructureFiles"])
        self.assertIn("Engine/Coroutine/CoroutineScheduler.h", entry["privateEngineHeaders"])


class FPSPrivateIncludeRatchetTests(unittest.TestCase):
    """MOD-310: the stable-v1 FPS module is ratcheted like a prototype, under a reviewed ceiling."""

    def test_fps_is_a_ratcheted_release_module(self) -> None:
        applicability = _authoritative(ROOT)[FPS_MODULE]["profileApplicability"]
        self.assertEqual("required", applicability.get("stable-v1"))
        self.assertFalse(module_content._is_prototype(applicability))
        self.assertTrue(module_content._is_ratcheted(FPS_MODULE, applicability))

    def test_fps_gameplay_types_have_one_public_definition(self) -> None:
        module = ROOT / "GameModules" / FPS_MODULE
        self.assertFalse((module / "Source/Enums/GameSystemEnums.h").exists(),
                         "Do not restore the FPS copy of the shared gameplay enum declarations")
        public = ROOT / "SparkSDK/Include/Spark/GameTypes.h"
        self.assertTrue(public.is_file())
        classified = module_content.classify_module_includes(ROOT, module)
        self.assertIn("Spark/GameTypes.h", classified["sdk"])
        self.assertNotIn("Enums/GameSystemEnums.h", classified["engine"])
        compatibility = ROOT / "SparkEngine/Source/Enums/GameSystemEnums.h"
        code, _ = module_content._lex_cpp(compatibility.read_text(encoding="utf-8"))
        self.assertIn("#include <Spark/GameTypes.h>", code)
        self.assertNotIn("enum class", code, "The runtime must consume the same SDK declarations")

    def test_fps_utility_types_have_one_public_definition(self) -> None:
        module = ROOT / "GameModules" / FPS_MODULE
        classified = module_content.classify_module_includes(ROOT, module)
        for public, private in (("Spark/StateMachine.h", "Utils/StateMachine.h"),
                                ("Spark/AngleUtils.h", "Utils/AngleUtils.h")):
            with self.subTest(header=public):
                self.assertTrue((ROOT / module_content.SDK_INCLUDE_ROOT / public).is_file())
                self.assertIn(public, classified["sdk"])
                self.assertNotIn(private, classified["engine"])
                compatibility = ROOT / module_content.ENGINE_PRIVATE_ROOT / private
                code, _ = module_content._lex_cpp(compatibility.read_text(encoding="utf-8"))
                self.assertIn(f"#include <{public}>", code)
                for definition in ("class ", "struct ", "constexpr", "namespace "):
                    self.assertNotIn(definition, code, "The runtime must consume the same SDK definitions")
        self.assertNotIn("Utils/ScheduledCallback.h", classified["engine"],
                         "WaveSpawner's rest countdown does not need the engine scheduler")

    def test_fps_matches_committed_inventory(self) -> None:
        entry = _committed_entries(ROOT)[FPS_MODULE]
        findings = module_content._validate_private_dependencies(ROOT, ROOT / "GameModules" / FPS_MODULE, entry, LOCATION)
        self.assertEqual([], findings)

    def test_fps_ceiling(self) -> None:
        entry = _committed_entries(ROOT)[FPS_MODULE]
        self.assertLessEqual(entry["privateEngineHeaderCount"], FPS_PRIVATE_HEADER_CEILING)
        self.assertEqual(
            FPS_PRIVATE_HEADER_CEILING,
            entry["privateEngineHeaderCount"],
            "SparkGameFPS dropped engine-private headers; lower FPS_PRIVATE_HEADER_CEILING to lock it in",
        )
        self.assertLessEqual(len(entry["copiedInfrastructureFiles"]), FPS_COPIED_INFRASTRUCTURE_CEILING)
        self.assertEqual(
            FPS_COPIED_INFRASTRUCTURE_CEILING,
            len(entry["copiedInfrastructureFiles"]),
            "SparkGameFPS dropped copied infrastructure; lower FPS_COPIED_INFRASTRUCTURE_CEILING to lock it in",
        )

    def test_fps_gained_header_fails_by_name(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="fps-private-ratchet-")
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        entry = _committed_entries(ROOT)[FPS_MODULE]
        module = root / "GameModules" / FPS_MODULE
        shutil.copytree(ROOT / "GameModules" / FPS_MODULE / "Source", module / "Source")
        shutil.copytree(ROOT / module_content.SDK_INCLUDE_ROOT, root / module_content.SDK_INCLUDE_ROOT)
        # Resolution needs only the files to exist: stub every listed engine
        # header plus the mutation target.
        for header in entry["privateEngineHeaders"] + ["Utils/Logger.h"]:
            stub = root / module_content.ENGINE_PRIVATE_ROOT / header
            stub.parent.mkdir(parents=True, exist_ok=True)
            stub.write_text("#pragma once\n", encoding="utf-8")

        def findings() -> list[str]:
            return [message for _, message in module_content._validate_private_dependencies(root, module, entry, LOCATION)]

        self.assertEqual([], findings())
        main = module / "Source" / "Core" / "Main.cpp"
        main.write_text('#include "Utils/Logger.h"\n' + main.read_text(encoding="utf-8"), encoding="utf-8")
        self.assertEqual(
            [f"{FPS_MODULE} gained engine-private header not listed in its committed inventory: Utils/Logger.h"],
            findings(),
        )


class FPSBuildCouplingTests(unittest.TestCase):
    """MOD-310's build-system half: SparkGameFPS linking SparkEngineLib or including SparkEngine/Source."""

    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="fps-build-coupling-")
        self.addCleanup(temp.cleanup)
        self.module = Path(temp.name) / FPS_MODULE
        self.module.mkdir()

    def measure(self, cmake: str) -> dict[str, bool]:
        (self.module / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
        return module_content._engine_build_coupling(self.module)

    def test_committed_inventory_matches_the_fps_cmake(self) -> None:
        measured = module_content._engine_build_coupling(ROOT / "GameModules" / FPS_MODULE)
        self.assertEqual(measured, _committed_entries(ROOT)[FPS_MODULE][module_content.ENGINE_BUILD_COUPLING_KEY])

    def test_comment_only_mentions_are_ignored(self) -> None:
        coupling = self.measure(
            "# target_link_libraries(SparkGameFPS PRIVATE SparkEngineLib)\n"
            "#[[ target_include_directories(SparkGameFPS PRIVATE \"${ENGINE_SOURCE_DIR}\") ]]\n"
            'target_include_directories(SparkGameFPS PRIVATE "Source" "${SPARK_SDK_INCLUDE_DIR}")\n'
            "target_link_libraries(SparkGameFPS PRIVATE SparkSDK)\n"
        )
        self.assertEqual({"linksSparkEngineLib": False, "engineSourceIncludeDirectory": False}, coupling)

    def test_link_and_include_are_detected_on_the_module_target_only(self) -> None:
        coupling = self.measure(
            "if(WIN32)\n    target_link_libraries(SparkGameFPS PRIVATE SparkEngineLib)\nendif()\n"
            "target_include_directories(OtherTarget PRIVATE ${CMAKE_SOURCE_DIR}/SparkEngine/Source)\n"
        )
        self.assertEqual({"linksSparkEngineLib": True, "engineSourceIncludeDirectory": False}, coupling)
        coupling = self.measure('target_include_directories(SparkGameFPS PRIVATE\n    "${ENGINE_SOURCE_DIR}"\n)\n')
        self.assertEqual({"linksSparkEngineLib": False, "engineSourceIncludeDirectory": True}, coupling)

    def test_recoupling_a_removed_dependency_fails_by_name(self) -> None:
        committed = {"linksSparkEngineLib": False, "engineSourceIncludeDirectory": True}
        measured = {"linksSparkEngineLib": True, "engineSourceIncludeDirectory": True}
        messages = [message for _, message in module_content._validate_engine_build_coupling(FPS_MODULE, measured, committed, LOCATION)]
        self.assertEqual(
            [
                f"engineBuildCoupling drift for {FPS_MODULE}: expected {measured!r}",
                f"{FPS_MODULE} regained engine build coupling linksSparkEngineLib; its committed inventory records it removed",
            ],
            messages,
        )
        self.assertEqual([], module_content._validate_engine_build_coupling(FPS_MODULE, committed, dict(committed), LOCATION))


class FPSBuildCouplingSpellingTests(unittest.TestCase):
    """The installed SDK's namespaced library must not evade the real CMake ratchet."""

    def test_namespaced_engine_link_is_detected(self) -> None:
        module = ROOT / "GameModules" / FPS_MODULE
        for library in ("Spark::SparkEngineLib", "$<LINK_ONLY:Spark::SparkEngineLib>"):
            with self.subTest(library=library):
                source = f"target_link_libraries({FPS_MODULE} PRIVATE {library})\n"
                with mock.patch.object(Path, "read_text", return_value=source):
                    coupling = module_content._engine_build_coupling(module)
                self.assertTrue(coupling["linksSparkEngineLib"])
                findings = module_content._validate_engine_build_coupling(
                    FPS_MODULE, coupling,
                    {"linksSparkEngineLib": False, "engineSourceIncludeDirectory": False}, LOCATION,
                )
                self.assertTrue(any("regained engine build coupling linksSparkEngineLib" in message
                                    for _, message in findings))


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
