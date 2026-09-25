#!/usr/bin/env python3
"""Classify SparkEngine test sources by what they actually execute.

The suite headline ("6817 passed") counts every registered TEST equally, but a
test that includes no production header executes a test-local reimplementation:
it can never detect a regression in the shipped code. This tool separates the
two so the readiness evidence can quote a production-source number instead of a
raw total, and so a NEW mirror file fails the build instead of inflating it.

Classification (first match wins):
  production-source  includes at least one header that resolves to a real file
                     under a production root (SparkEngine/, SparkEditor/, ...)
  process-smoke      no production header, but the file launches and inspects a
                     shipped binary, so it does exercise production code
  mirror             no production header and no process launch: whatever it
                     asserts about, it defined itself

Usage:
  python Tools/test_source_census.py                  # human-readable report
  python Tools/test_source_census.py --json out.json  # machine-readable stats
  python Tools/test_source_census.py --check          # non-zero on a regression
  python Tools/test_source_census.py --profile-selectors [ctest.json]
      # non-zero when a stable-v1/module-profile SparkTests selector reaches a
      # mirror or tautological TEST (static Tests/CMakeLists.txt view, plus the
      # 'ctest --show-only=json-v1' inventory of a configured tree when given)

--check fails when a mirror file appears that is not in MIRROR_BASELINE below,
or when a test named *_Skipped fabricates a pass instead of calling SKIP_TEST.
Fail-closed: an unreadable file, an unresolvable repository root, or an empty
scan is an error, never a silent pass.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Directories whose headers count as production code.
PRODUCTION_ROOTS = (
    "SparkEngine/Source",
    "SparkEditor/Source",
    "SparkSDK/Include",
    "SparkServer/src",
    "SparkDaemon/src",
    "GameModules",
    "Templates",
)

# Test-support headers that never make a file production-source. The harness is
# itself production code for every gate that reads the runner's output, but it
# is also the vehicle every other test rides in, so including it cannot be what
# makes a file production-source.
TEST_SUPPORT_HEADERS = {"TestFramework.h", "TestWarnings.h"}

# ...which leaves the files whose SUBJECT is the harness. Includes alone cannot
# distinguish those, so they are named here.
HARNESS_TESTS = frozenset({
    "Tests/TestRunnerSemanticsReal.cpp",
    # Verifies the sanitizer harness and instrumented C++ runtime, not a
    # test-local copy of engine behavior. Non-MSan builds explicitly skip it.
    "Tests/TestMSanCanary.cpp",
})

# A file with no production header still exercises production code if it drives
# a shipped executable and asserts on the result.
PROCESS_SMOKE_MARKERS = (
    "CreateProcessA",
    "CreateProcessW",
    "_popen",
    "popen(",
    "posix_spawn",
    "execvp",
    "SPARK_TEST_DEBUG_HOOK_TEARDOWN_PROBE_PATH",
    "LaunchProcess",
)

TEST_MACRO_RE = re.compile(r"^\s*TEST(?:_F)?\s*\(", re.MULTILINE)
# Both include forms count: an SDK consumer test legitimately writes
# #include <Spark/Version.h>, and treating only quoted includes as real
# would file that test as a mirror it is not.
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>)', re.MULTILINE)
TAUTOLOGY_RE = re.compile(r"EXPECT_TRUE\s*\(\s*true\s*\)")
SKIPPED_TEST_RE = re.compile(r"^\s*TEST\s*\(\s*([A-Za-z0-9_]*_Skipped)\s*\)", re.MULTILINE)

# Mirror files known at the time this tool was introduced. Every entry is a test
# that asserts about a test-local copy of the engine rather than about the
# engine. Shrink this list by giving the file a production-source companion (or
# by including the real header); never grow it - --check rejects a mirror file
# that is not listed here, and equally rejects a listed file that stopped being
# a mirror, so the improvement is locked in.
MIRROR_BASELINE: frozenset[str] = frozenset(
    {
        "Tests/TestAIBehaviorTree.cpp",
        "Tests/TestAIDebugRenderer.cpp",
        "Tests/TestAIStress.cpp",
        "Tests/TestAbilitySystem.cpp",
        "Tests/TestAlignedHeapArray.cpp",
        "Tests/TestAngelScriptEngine.cpp",
        "Tests/TestAngleUtils.cpp",
        "Tests/TestAnimationCompression.cpp",
        "Tests/TestAnimationPhysicsIntegration.cpp",
        "Tests/TestAnimationStress.cpp",
        "Tests/TestAnimationSystem.cpp",
        "Tests/TestAssertSuppression.cpp",
        "Tests/TestAssetPipelineCache.cpp",
        "Tests/TestAssetStallDetector.cpp",
        "Tests/TestAsyncComputeScheduler.cpp",
        "Tests/TestAsyncDatabase.cpp",
        "Tests/TestAtomicSharedPtr.cpp",
        "Tests/TestAudioEngine.cpp",
        "Tests/TestBitUtils.cpp",
        "Tests/TestBlendSpace.cpp",
        "Tests/TestCameraInterpolation.cpp",
        "Tests/TestCameraTransforms.cpp",
        "Tests/TestClusteredLightGPU.cpp",
        "Tests/TestCollisionAvoidance.cpp",
        "Tests/TestCollisionLayers.cpp",
        "Tests/TestCommandHistory.cpp",
        "Tests/TestCompressionUtils.cpp",
        "Tests/TestConditionSystem.cpp",
        "Tests/TestConnectionScope.cpp",
        "Tests/TestConsoleRBAC.cpp",
        "Tests/TestConsoleVariables.cpp",
        "Tests/TestConstantBufferDiff.cpp",
        "Tests/TestContainerUtils.cpp",
        "Tests/TestCoroutineScheduler.cpp",
        "Tests/TestCoverSystem.cpp",
        "Tests/TestCrossSystemIntegration.cpp",
        "Tests/TestDayNightCycle.cpp",
        "Tests/TestDeadlockDetector.cpp",
        "Tests/TestDedicatedServer.cpp",
        "Tests/TestDeferredQueue.cpp",
        "Tests/TestDelegate.cpp",
        "Tests/TestDescriptorCache.cpp",
        "Tests/TestDirtyRectTracker.cpp",
        "Tests/TestDrawIndirect.cpp",
        "Tests/TestDynamicResponseSystem.cpp",
        "Tests/TestECSIntegration.cpp",
        "Tests/TestECSStress.cpp",
        "Tests/TestECSWorld.cpp",
        "Tests/TestECSystemOrdering.cpp",
        "Tests/TestECSystemSpecialized.cpp",
        "Tests/TestEditorAutomation.cpp",
        "Tests/TestEditorCommands.cpp",
        "Tests/TestEngineContext.cpp",
        "Tests/TestEngineSettingsEdgeCases.cpp",
        "Tests/TestEngineSettingsParser.cpp",
        "Tests/TestEntityPresetManager.cpp",
        "Tests/TestEventResponseSystem.cpp",
        "Tests/TestEventSystem.cpp",
        "Tests/TestExtendedSystems.cpp",
        "Tests/TestFPSComponents.cpp",
        "Tests/TestFaultIsolation.cpp",
        "Tests/TestFixtures.cpp",
        "Tests/TestFormationSystem.cpp",
        "Tests/TestFreezeDetector.cpp",
        "Tests/TestFreezeSystem.cpp",
        "Tests/TestFrustumCulling.cpp",
        "Tests/TestGPUClusterCulling.cpp",
        "Tests/TestGPUParticleSystem.cpp",
        "Tests/TestGPUPerfCounters.cpp",
        "Tests/TestGPUResourceLeakDetector.cpp",
        "Tests/TestGPUSkinning.cpp",
        "Tests/TestGPUStallProfiler.cpp",
        "Tests/TestGameMode.cpp",
        "Tests/TestGameObjectTransforms.cpp",
        "Tests/TestGamepadInputProcessing.cpp",
        "Tests/TestGameplayExtensionRegistry.cpp",
        "Tests/TestGizmoMath.cpp",
        "Tests/TestGroupAI.cpp",
        "Tests/TestHitchDetector.cpp",
        "Tests/TestInputManagerState.cpp",
        "Tests/TestInputSystem.cpp",
        "Tests/TestInstanceManager.cpp",
        "Tests/TestInventorySystem.cpp",
        "Tests/TestLODGenerator.cpp",
        "Tests/TestLagCompensationIntegration.cpp",
        "Tests/TestLauncherPaths.cpp",
        "Tests/TestLockFreeRingAllocator.cpp",
        "Tests/TestMaterialEffects.cpp",
        "Tests/TestMaterialSystemEdgeCases.cpp",
        "Tests/TestMaterialSystemValidation.cpp",
        "Tests/TestMathUtils.cpp",
        "Tests/TestMeshLOD.cpp",
        "Tests/TestMeshShaderPipeline.cpp",
        "Tests/TestModuleDependency.cpp",
        "Tests/TestMovementSystem.cpp",
        "Tests/TestNavMesh.cpp",
        "Tests/TestNetBuffer.cpp",
        "Tests/TestNetworkEncryption.cpp",
        "Tests/TestNetworkHealthMonitor.cpp",
        "Tests/TestNetworkInterpolation.cpp",
        "Tests/TestNetworkManagerOrchestration.cpp",
        "Tests/TestNoiseGenerator.cpp",
        "Tests/TestNullRHIDevice.cpp",
        "Tests/TestObjectPool.cpp",
        "Tests/TestOcclusionCulling.cpp",
        "Tests/TestParallelCulling.cpp",
        "Tests/TestPathCache.cpp",
        "Tests/TestPerceptionSystemMath.cpp",
        "Tests/TestPerformanceStats.cpp",
        "Tests/TestPhysicsComponents.cpp",
        "Tests/TestPhysicsECSIntegration.cpp",
        "Tests/TestPhysicsInterpolation.cpp",
        "Tests/TestPortalCulling.cpp",
        "Tests/TestPostProcessingPipeline.cpp",
        "Tests/TestProximityTriggerSystem.cpp",
        "Tests/TestRHIHandlePool.cpp",
        "Tests/TestReflection.cpp",
        "Tests/TestRenderCommandRing.cpp",
        "Tests/TestRenderECSIntegration.cpp",
        "Tests/TestReplicationFields.cpp",
        "Tests/TestSHLighting.cpp",
        "Tests/TestSaveSystem.cpp",
        "Tests/TestSceneConfigDatabase.cpp",
        "Tests/TestSceneManager.cpp",
        "Tests/TestSceneSerializer.cpp",
        "Tests/TestScheduledCallback.cpp",
        "Tests/TestScriptHookManager.cpp",
        "Tests/TestScriptHotReload.cpp",
        "Tests/TestSeamlessAreaManager.cpp",
        "Tests/TestSelfRecovery.cpp",
        "Tests/TestSequencer.cpp",
        "Tests/TestServerMockClient.cpp",
        "Tests/TestShaderDiskCache.cpp",
        "Tests/TestShaderGraphCompiler.cpp",
        "Tests/TestSkyAtmosphere.cpp",
        "Tests/TestSparkBuildConfig.cpp",
        "Tests/TestSparkGameARPG.cpp",
        "Tests/TestSparkGamePlatformer.cpp",
        "Tests/TestSparkGameRPG.cpp",
        "Tests/TestSparkGameRTS.cpp",
        "Tests/TestSparkGameRacing.cpp",
        "Tests/TestSparkGatewayCoordinator.cpp",
        "Tests/TestSpatialGrid.cpp",
        "Tests/TestSplineMath.cpp",
        "Tests/TestSprite2DComponents.cpp",
        "Tests/TestSteeringBehaviors.cpp",
        "Tests/TestStringPool.cpp",
        "Tests/TestSubsystemConsoleCommands.cpp",
        "Tests/TestTFCaptureMath.cpp",
        "Tests/TestTFDamageModel.cpp",
        "Tests/TestTacticalPointSystem.cpp",
        "Tests/TestTemporalEffects.cpp",
        "Tests/TestTerrainRenderer.cpp",
        "Tests/TestTextureZombiePool.cpp",
        "Tests/TestThirdPartyIntegration.cpp",
        "Tests/TestTimeOfDaySystem.cpp",
        "Tests/TestTransientBufferAllocator.cpp",
        "Tests/TestTween.cpp",
        "Tests/TestUndoRedoManager.cpp",
        "Tests/TestVRSystem.cpp",
        "Tests/TestVersionedHandle.cpp",
        "Tests/TestWaterRenderer.cpp",
        "Tests/TestWeaponMechanics.cpp",
        "Tests/TestWeaponSystem.cpp",
        "Tests/TestWeatherSystem.cpp",
        "Tests/TestWorkSema.cpp",
        "Tests/TestWorldServerRouting.cpp",
        "Tests/harden/Test_core_hardening.cpp",
        "Tests/harden/Test_ecs_ai_pathfollow.cpp",
        "Tests/harden/Test_ecs_audio_doppler.cpp",
        "Tests/harden/Test_scripting_hardening.cpp",
        "Tests/harden/Test_tooling_CommandParser.cpp",
    }
)


def repo_root(start: Path) -> Path:
    """Locate the repository root by walking up to the directory holding Tests/."""
    for candidate in [start, *start.parents]:
        if (candidate / "Tests").is_dir() and (candidate / "CMakeLists.txt").is_file():
            return candidate
    raise SystemExit("error: could not locate the repository root (no Tests/ + CMakeLists.txt above this script)")


def resolves_to_production(include: str, root: Path) -> bool:
    name = include.rsplit("/", 1)[-1]
    if name in TEST_SUPPORT_HEADERS:
        return False
    normalized = include.replace("\\", "/").lstrip("./")
    for production_root in PRODUCTION_ROOTS:
        if (root / production_root / normalized).is_file():
            return True
        # Includes are also written relative to the repository root.
        if normalized.startswith(production_root + "/") and (root / normalized).is_file():
            return True
    # Game-module tests include module headers relative to the module's own include
    # root (e.g. "Persistence/TFDatabase.h" -> GameModules/SparkGameMMOFPS/Source/...).
    for module_source in sorted((root / "GameModules").glob("*/Source")):
        if (module_source / normalized).is_file():
            return True
    return False


def classify(text: str, root: Path, relative_path: str) -> str:
    if relative_path in HARNESS_TESTS:
        return "production-source"
    for quoted, angled in INCLUDE_RE.findall(text):
        if resolves_to_production(quoted or angled, root):
            return "production-source"
    for marker in PROCESS_SMOKE_MARKERS:
        if marker in text:
            return "process-smoke"
    return "mirror"


def scan(root: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for path in sorted((root / "Tests").rglob("*.cpp")):
        try:
            text = path.read_text(encoding="utf-8", errors="strict")
        except (OSError, UnicodeDecodeError) as exc:
            raise SystemExit(f"error: cannot read {path}: {exc}")
        tests = len(TEST_MACRO_RE.findall(text))
        if tests == 0:
            continue
        relative_path = path.relative_to(root).as_posix()
        rows.append(
            {
                "path": relative_path,
                "tests": tests,
                "kind": classify(text, root, relative_path),
                "tautologies": len(TAUTOLOGY_RE.findall(text)),
                "fabricatedSkips": sorted(
                    name
                    for name in SKIPPED_TEST_RE.findall(text)
                    if _body_after(text, name) and "SKIP_TEST" not in _body_after(text, name)
                ),
            }
        )
    if not rows:
        raise SystemExit("error: no test files with TESTs were found - the scan is not trustworthy")
    return rows


def _body_after(text: str, test_name: str) -> str:
    """Return the brace-delimited body of TEST(test_name), or '' if not found."""
    match = re.search(r"TEST\s*\(\s*" + re.escape(test_name) + r"\s*\)\s*\{", text)
    if not match:
        return ""
    depth = 0
    start = match.end() - 1
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
    return ""


# ---------------------------------------------------------------------------
# Profile-selector guard (RDY-010)
#
# A CTest labeled stable-v1 or module-profile that runs SparkTests is release
# evidence, and what it executes is decided by the TestMain.cpp filters. Each
# such selector is resolved here to the TEST definitions it can reach, and it
# fails when any of them lives in a mirror file or asserts nothing - so a copied
# model or a tautology can never satisfy a release-profile gate.
# ---------------------------------------------------------------------------

PROFILE_LABELS = frozenset({"stable-v1", "module-profile"})
STATIC_ONLY = "<static>"
_SPARKTESTS_TARGET = "$<TARGET_FILE:SparkTests>"
_SPARKTESTS_EXECUTABLES = frozenset({"SparkTests", "SparkTests.exe"})
_SELECTOR_ASSIGNMENT_RE = re.compile(r"^(SPARK_TEST_[A-Z_]+)=(.*)$", re.DOTALL)
# add_test keywords that end the COMMAND argument list.
_ADD_TEST_KEYWORDS = frozenset({"CONFIGURATIONS", "WORKING_DIRECTORY", "COMMAND_EXPAND_LISTS"})
_TEST_DEFINITION_RE = re.compile(
    r"^[ \t]*(?:TEST[ \t]*\([ \t]*(\w+)[ \t]*\)|TEST_F[ \t]*\([ \t]*(\w+)[ \t]*,[ \t]*(\w+)[ \t]*\))\s*\{",
    re.MULTILINE,
)
# Statements that assert nothing about the code under test. EXPECT_NO_CRASH
# discards its argument unevaluated (TestFramework.h), so it proves nothing.
_TAUTOLOGICAL_STATEMENT_RE = re.compile(r"EXPECT_TRUE\s*\(\s*true\s*\)|EXPECT_NO_CRASH\s*\(.*\)", re.DOTALL)
_EXPECT_COUNT_RE = re.compile(r"^[0-9]+$")
_INT_MAX = 2**31 - 1


@dataclass(frozen=True)
class RegisteredTest:
    """One TEST/TEST_F as TestMain.cpp registers it: name, source file, and what its body asserts."""

    name: str
    path: str
    line: int
    kind: str
    tautological: bool


@dataclass
class ProfileSelector:
    """A profile-labeled CTest registration and the SparkTests filters it sets."""

    test: str
    origin: str
    runs_spark_tests: bool
    environment: dict[str, str] = field(default_factory=dict)
    unresolved: list[str] = field(default_factory=list)


def _literal_end(text: str, index: int) -> int:
    """Return the index just past a comment or literal starting at text[index], or index if none starts there."""
    if text.startswith("//", index):
        end = text.find("\n", index)
        return len(text) if end < 0 else end
    if text.startswith("/*", index):
        end = text.find("*/", index + 2)
        if end < 0:
            raise ValueError("unterminated /* comment")
        return end + 2
    char = text[index]
    if char not in "\"'":
        return index
    # The identifier/number token directly before the quote decides what it is.
    token_start = index
    while token_start > 0 and (text[token_start - 1].isalnum() or text[token_start - 1] == "_"):
        token_start -= 1
    prefix = text[token_start:index]
    if char == "'" and prefix[:1].isdigit():
        return index  # C++14 digit separator (1'000'000), not a character literal
    if char == '"' and prefix in ("R", "LR", "uR", "UR", "u8R"):
        open_paren = text.find("(", index)
        if open_paren < 0:
            raise ValueError("malformed raw string literal")
        terminator = ")" + text[index + 1 : open_paren] + '"'
        end = text.find(terminator, open_paren)
        if end < 0:
            raise ValueError("unterminated raw string literal")
        return end + len(terminator)
    cursor = index + 1
    while cursor < len(text) and text[cursor] != "\n":
        if text[cursor] == "\\":
            cursor += 2
            continue
        if text[cursor] == char:
            return cursor + 1
        cursor += 1
    raise ValueError(f"unterminated {char} literal")


def cpp_code_only(text: str) -> str:
    """Return text with comments blanked and literals emptied, keeping every newline so line numbers hold."""
    out: list[str] = []
    index = 0
    while index < len(text):
        end = _literal_end(text, index)
        if end == index:
            out.append(text[index])
            index += 1
            continue
        skipped = text[index:end]
        if skipped.startswith("/"):
            out.append(" ")
        else:
            out.append("''" if skipped.endswith("'") else '""')
        out.append("\n" * skipped.count("\n"))
        index = end
    return "".join(out)


def _braced_body(code: str, open_index: int) -> str:
    """Return code[open_index:] through the matching '}' (code must already be literal-free)."""
    depth = 0
    for index in range(open_index, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return code[open_index + 1 : index]
    raise ValueError(f"unbalanced braces after offset {open_index}")


def is_tautological(body: str) -> bool:
    """True when a literal-free body has no statement besides EXPECT_TRUE(true) / EXPECT_NO_CRASH(...)."""
    code = body.replace("{", " ").replace("}", " ")
    statements: list[str] = []
    current: list[str] = []
    depth = 0
    for char in code:
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        if char == ";" and depth == 0:
            statements.append("".join(current).strip())
            current = []
        else:
            current.append(char)
    statements.append("".join(current).strip())
    return all(_TAUTOLOGICAL_STATEMENT_RE.fullmatch(statement) for statement in statements if statement)


def resolve_registered_tests(root: Path, rows: list[dict[str, object]]) -> list[RegisteredTest]:
    """Resolve every TEST/TEST_F in the census rows to its registered name and body verdict.

    Fail-closed: a file whose TEST( count differs from the definitions parsed
    here holds a test whose registered name is unknown, so no selector could be
    proven unable to reach it.
    """
    definitions: list[RegisteredTest] = []
    for row in rows:
        relative_path = str(row["path"])
        try:
            code = cpp_code_only((root / relative_path).read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, ValueError) as exc:
            raise SystemExit(f"error: cannot resolve TEST definitions in {relative_path}: {exc}")
        matches = list(_TEST_DEFINITION_RE.finditer(code))
        declared = len(TEST_MACRO_RE.findall(code))
        if len(matches) != declared:
            raise SystemExit(
                f"error: {relative_path}: resolved {len(matches)} TEST definitions but found {declared} TEST( "
                "macros; a test whose registered name is unknown defeats the profile-selector guard"
            )
        for match in matches:
            definitions.append(
                RegisteredTest(
                    name=match.group(1) or f"{match.group(2)}.{match.group(3)}",
                    path=relative_path,
                    line=code.count("\n", 0, match.start(match.lastindex or 0)) + 1,
                    kind=str(row["kind"]),
                    tautological=is_tautological(_braced_body(code, match.end() - 1)),
                )
            )
    return definitions


def _selector_assignments(entries: list[str], selector: ProfileSelector, source: str) -> None:
    for entry in entries:
        assignment = _SELECTOR_ASSIGNMENT_RE.match(entry)
        if not assignment:
            continue
        key, value = assignment.groups()
        if "${" in value or "$<" in value:
            selector.unresolved.append(f"{source} {key}={value} is not a literal the guard can resolve")
        selector.environment[key] = value


def static_profile_selectors(text: str, origin: str) -> list[ProfileSelector]:
    """Profile-labeled registrations in a Tests/CMakeLists.txt, whatever platform branch guards them."""
    policy = _load_sibling("validate_ctest_policy")
    source = policy.strip_comments(text)
    commands: dict[str, list[list[str]]] = {}
    properties: dict[str, dict[str, str]] = {}
    position = 0
    while True:
        match = policy._COMMAND_RE.search(source, position)
        if not match:
            break
        body, position = policy._balanced_body(source, match.end() - 1)
        tokens = policy._tokens(body)
        if not tokens:
            continue
        if match.group(1).lower() == "add_test":
            if tokens[0] == "NAME" and len(tokens) >= 2:
                name = tokens[1]
                command = tokens[tokens.index("COMMAND") + 1 :] if "COMMAND" in tokens else []
            else:
                name, command = tokens[0], tokens[1:]
            cut = next((index for index, token in enumerate(command) if token in _ADD_TEST_KEYWORDS), len(command))
            commands.setdefault(name, []).append(command[:cut])
            continue
        if "PROPERTIES" not in tokens:
            continue
        split = tokens.index("PROPERTIES")
        values = tokens[split + 1 :]
        for target in tokens[:split]:
            for key_index in range(0, len(values) - 1, 2):
                # set_tests_properties replaces a property, so the last call wins.
                properties.setdefault(target, {})[values[key_index]] = values[key_index + 1]

    selectors: list[ProfileSelector] = []
    for name, variants in commands.items():
        props = properties.get(name, {})
        labels = props.get("LABELS", "")
        runs_spark_tests = any(_SPARKTESTS_TARGET in " ".join(command) for command in variants)
        if not PROFILE_LABELS.intersection(labels.split(";")):
            if runs_spark_tests and ("${" in labels or "$<" in labels):
                selector = ProfileSelector(name, origin, True)
                selector.unresolved.append(f"LABELS {labels!r} cannot be resolved to decide profile membership")
                selectors.append(selector)
            continue
        selector = ProfileSelector(name, origin, runs_spark_tests)
        if "${" in name or "$<" in name:
            selector.unresolved.append(f"test name {name!r} is not a literal")
        _selector_assignments(props.get("ENVIRONMENT", "").split(";"), selector, "ENVIRONMENT")
        if "SPARK_TEST_" in props.get("ENVIRONMENT_MODIFICATION", ""):
            selector.unresolved.append("ENVIRONMENT_MODIFICATION rewrites a SPARK_TEST_* filter")
        for command in variants:
            _selector_assignments(command, selector, "COMMAND")
        selectors.append(selector)
    return selectors


def ctest_profile_selectors(document: object, origin: str) -> list[ProfileSelector]:
    """Profile-labeled registrations in 'ctest --show-only=json-v1' output from a configured tree."""
    if not isinstance(document, dict) or not isinstance(document.get("tests"), list) or not document["tests"]:
        raise SystemExit(f"error: {origin}: not a non-empty 'ctest --show-only=json-v1' inventory")
    selectors: list[ProfileSelector] = []
    for entry in document["tests"]:
        if not isinstance(entry, dict):
            raise SystemExit(f"error: {origin}: malformed test entry {entry!r}")
        props = {prop.get("name"): prop.get("value") for prop in entry.get("properties", []) if isinstance(prop, dict)}
        labels = props.get("LABELS") or []
        if not isinstance(labels, list) or not PROFILE_LABELS.intersection(str(label) for label in labels):
            continue
        command = [str(part) for part in entry.get("command") or []]
        runs_spark_tests = any(re.split(r"[\\/]", part)[-1] in _SPARKTESTS_EXECUTABLES for part in command)
        selector = ProfileSelector(str(entry.get("name", "<unnamed>")), origin, runs_spark_tests)
        if not command:
            selector.runs_spark_tests = True
            selector.unresolved.append(
                "has no command: its executable is not built, or not available without 'ctest -C <config>'"
            )
        _selector_assignments([str(item) for item in props.get("ENVIRONMENT") or []], selector, "ENVIRONMENT")
        if any("SPARK_TEST_" in str(item) for item in props.get("ENVIRONMENT_MODIFICATION") or []):
            selector.unresolved.append("ENVIRONMENT_MODIFICATION rewrites a SPARK_TEST_* filter")
        _selector_assignments(command, selector, "COMMAND")
        selectors.append(selector)
    return selectors


def _file_candidates(root: Path, relative_path: str) -> tuple[str, ...]:
    # TestMain.cpp matches SPARK_TEST_FILE against __FILE__, which is compiler-
    # and platform-dependent (absolute, backslashed on MSVC). Matching any
    # spelling over-approximates the selection, which only makes the guard stricter.
    absolute = (root / relative_path).as_posix()
    return (relative_path, relative_path.replace("/", "\\"), absolute, absolute.replace("/", "\\"))


def select_definitions(
    selector: ProfileSelector, definitions: list[RegisteredTest], root: Path
) -> list[RegisteredTest]:
    """Apply the TestMain.cpp filters exactly: strstr on name and file, then comma-separated name excludes."""
    name_filter = selector.environment.get("SPARK_TEST_NAME")
    file_filter = selector.environment.get("SPARK_TEST_FILE")
    excludes = [pattern for pattern in selector.environment.get("SPARK_TEST_EXCLUDE", "").split(",") if pattern]
    selected: list[RegisteredTest] = []
    for definition in definitions:
        spellings = _file_candidates(root, definition.path)
        if file_filter is not None and not any(file_filter in spelling for spelling in spellings):
            continue
        if name_filter is not None and name_filter not in definition.name:
            continue
        if any(pattern in definition.name for pattern in excludes):
            continue
        selected.append(definition)
    return selected


def check_profile_selectors(
    selectors: list[ProfileSelector], definitions: list[RegisteredTest], root: Path, origin: str
) -> tuple[list[str], list[str]]:
    """Return (failures, report lines) for every profile-labeled SparkTests selector."""
    failures: list[str] = []
    report: list[str] = []
    spark_selectors = [selector for selector in selectors if selector.runs_spark_tests]
    if not spark_selectors:
        failures.append(
            f"{origin}: no stable-v1/module-profile CTest runs SparkTests; the inventory is not trustworthy"
        )
    mirror_files = len({definition.path for definition in definitions if definition.kind == "mirror"})
    for selector in spark_selectors:
        where = f"{selector.origin}: {selector.test}"
        if selector.unresolved:
            failures.extend(f"{where}: {problem}" for problem in selector.unresolved)
            continue
        environment = selector.environment
        if "SPARK_TEST_NAME" not in environment and "SPARK_TEST_FILE" not in environment:
            failures.append(
                f"{where}: runs the whole SparkTests suite, which includes {mirror_files} mirror files; "
                "set SPARK_TEST_NAME/SPARK_TEST_FILE to production-source tests"
            )
            continue
        expected_text = environment.get("SPARK_TEST_EXPECT_COUNT")
        if expected_text is None:
            failures.append(
                f"{where}: no SPARK_TEST_EXPECT_COUNT, so a selector that drifts to other tests still passes"
            )
            continue
        if not _EXPECT_COUNT_RE.match(expected_text) or not 0 < int(expected_text) <= _INT_MAX:
            failures.append(f"{where}: SPARK_TEST_EXPECT_COUNT={expected_text!r} is not a positive decimal integer")
            continue
        selected = select_definitions(selector, definitions, root)
        filters = ", ".join(f"{key}={environment[key]}" for key in sorted(environment))
        if not selected:
            failures.append(f"{where}: {filters} selects no TEST definition")
            continue
        for definition in selected:
            if definition.kind == "mirror":
                failures.append(
                    f"{where}: {filters} reaches TEST({definition.name}) in mirror file {definition.path}:"
                    f"{definition.line}; a test-local copy cannot be release-profile evidence"
                )
            if definition.tautological:
                failures.append(
                    f"{where}: {filters} reaches TEST({definition.name}) at {definition.path}:{definition.line}, "
                    "whose body is only EXPECT_TRUE(true)/EXPECT_NO_CRASH"
                )
        if len(selected) < int(expected_text):
            failures.append(
                f"{where}: SPARK_TEST_EXPECT_COUNT={expected_text} but only {len(selected)} TEST definitions "
                "match; the count can only be met by tests the guard cannot see"
            )
        files = len({definition.path for definition in selected})
        report.append(f"{where}: {filters} -> {len(selected)} TEST definition(s) in {files} file(s)")
    return failures, report


def run_profile_selectors(root: Path, rows: list[dict[str, object]], ctest_json: str, cmake_lists: Path | None) -> int:
    """Check the static Tests/CMakeLists.txt view, plus a configured tree's CTest inventory when one is given."""
    definitions = resolve_registered_tests(root, rows)
    cmake_path = cmake_lists or root / "Tests" / "CMakeLists.txt"
    try:
        views = [(str(cmake_path), static_profile_selectors(cmake_path.read_text(encoding="utf-8"), str(cmake_path)))]
        if ctest_json != STATIC_ONLY:
            document = json.loads(Path(ctest_json).read_text(encoding="utf-8"))
            views.append((ctest_json, ctest_profile_selectors(document, ctest_json)))
    except (OSError, ValueError) as exc:
        print(f"error: cannot read a profile-selector inventory: {exc}", file=sys.stderr)
        return 2

    failures: list[str] = []
    for origin, selectors in views:
        view_failures, report = check_profile_selectors(selectors, definitions, root, origin)
        failures.extend(view_failures)
        for line in report:
            print(line)
        others = sum(1 for selector in selectors if not selector.runs_spark_tests)
        print(f"{origin}: {len(selectors) - others} profile SparkTests selector(s), {others} other profile test(s)")
    if failures:
        print("\nProfile-selector check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("\nProfile-selector check passed: every release-profile selector reaches only production-source tests.")
    return 0


def _load_sibling(module_name: str):
    """Import a sibling Tools/*.py module whether this file runs as a script or is imported by path."""
    path = Path(__file__).resolve().parent / f"{module_name}.py"
    spec = importlib.util.spec_from_file_location(f"spark_{module_name}", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"error: cannot import {path}")
    module = sys.modules.get(spec.name)
    if module is None:
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
    return module


def summarize(rows: list[dict[str, object]]) -> dict[str, object]:
    def total(kind: str, field: str) -> int:
        return sum(int(row[field]) for row in rows if row["kind"] == kind)

    return {
        "schemaVersion": 1,
        "files": len(rows),
        "tests": sum(int(row["tests"]) for row in rows),
        "productionSourceFiles": sum(1 for row in rows if row["kind"] == "production-source"),
        "productionSourceTests": total("production-source", "tests"),
        "processSmokeFiles": sum(1 for row in rows if row["kind"] == "process-smoke"),
        "processSmokeTests": total("process-smoke", "tests"),
        "mirrorFiles": sum(1 for row in rows if row["kind"] == "mirror"),
        "mirrorTests": total("mirror", "tests"),
        "tautologicalAssertions": sum(int(row["tautologies"]) for row in rows),
        "files_detail": rows,
    }


def check(rows: list[dict[str, object]], baseline: set[str]) -> tuple[list[str], list[str]]:
    """Return (failures, advisories). Only failures make --check exit non-zero."""
    failures: list[str] = []
    for row in rows:
        path = str(row["path"])
        if row["kind"] == "mirror" and path not in baseline:
            failures.append(
                f"{path}: new mirror test file (no production header). Include the real header, "
                f"or add a production-source companion, before adding it to MIRROR_BASELINE."
            )
        for name in row["fabricatedSkips"]:  # type: ignore[union-attr]
            failures.append(
                f"{path}: TEST({name}) reports a pass for a compiled-out feature. Use "
                f'SKIP_TEST("<why it is compiled out>") so the skip is visible to the ratchet.'
            )
    # A baseline entry that stopped being a mirror is an improvement, never a
    # regression: say so and let the build pass, so nobody is punished for
    # fixing a file before pruning the list.
    advisories = [
        f"{path}: no longer a mirror file - remove it from MIRROR_BASELINE to lock the improvement in."
        for path in sorted(baseline - {str(row["path"]) for row in rows if row["kind"] == "mirror"})
    ]
    return failures, advisories


def load_baseline(root: Path) -> set[str]:
    if not MIRROR_BASELINE:
        raise SystemExit("error: MIRROR_BASELINE is empty; refusing to treat every mirror file as new")
    unknown = sorted(entry for entry in MIRROR_BASELINE if not (root / entry).is_file())
    if unknown:
        raise SystemExit("error: MIRROR_BASELINE lists files that no longer exist: " + ", ".join(unknown))
    return set(MIRROR_BASELINE)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", dest="json_path", type=Path, help="write machine-readable statistics")
    parser.add_argument("--check", action="store_true", help="exit non-zero on a new mirror file or a fabricated skip")
    parser.add_argument("--list", choices=["mirror", "production-source", "process-smoke"], help="list files of a kind")
    parser.add_argument(
        "--minimum-production-tests",
        type=int,
        help=(
            "fail when fewer than this many tests include a production header; "
            "the floor lives in .github/test-count-ratchet.json"
        ),
    )
    parser.add_argument(
        "--profile-selectors",
        nargs="?",
        const=STATIC_ONLY,
        metavar="CTEST_JSON",
        help=(
            "fail when a stable-v1/module-profile CTest selects a mirror or tautological TEST. Always checks "
            "Tests/CMakeLists.txt statically (every platform branch); also checks CTEST_JSON, the output of "
            "'ctest --show-only=json-v1', when given"
        ),
    )
    parser.add_argument(
        "--cmake-lists",
        type=Path,
        help="CMakeLists.txt for the static --profile-selectors view (default: Tests/CMakeLists.txt)",
    )
    args = parser.parse_args()
    if args.minimum_production_tests is not None and args.minimum_production_tests < 1:
        parser.error("--minimum-production-tests must be at least 1")
    if args.cmake_lists is not None and args.profile_selectors is None:
        parser.error("--cmake-lists only applies to --profile-selectors")

    root = repo_root(Path(__file__).resolve().parent)
    rows = scan(root)
    stats = summarize(rows)

    if args.profile_selectors is not None:
        return run_profile_selectors(root, rows, args.profile_selectors, args.cmake_lists)

    if args.list:
        for row in rows:
            if row["kind"] == args.list:
                print(f"{row['path']}\t{row['tests']}")
        return 0

    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(json.dumps(stats, indent=2) + "\n", encoding="utf-8")

    print(f"Test files with TESTs : {stats['files']}")
    print(f"Registered TESTs      : {stats['tests']}")
    print(f"  production-source   : {stats['productionSourceFiles']} files, {stats['productionSourceTests']} tests")
    print(f"  process-smoke       : {stats['processSmokeFiles']} files, {stats['processSmokeTests']} tests")
    print(f"  mirror              : {stats['mirrorFiles']} files, {stats['mirrorTests']} tests")
    print(f"EXPECT_TRUE(true)     : {stats['tautologicalAssertions']} occurrences")

    # A floor on production-source tests is the number that actually gates: the
    # headline total can keep climbing while the part of it that can detect a
    # regression in shipped code shrinks.
    if args.minimum_production_tests is not None:
        production_tests = int(stats["productionSourceTests"])
        if production_tests < args.minimum_production_tests:
            print(
                f"\nerror: {production_tests} production-source tests, below the floor of "
                f"{args.minimum_production_tests}. Restore the coverage, or re-measure the "
                "floor from the run that moved it - never lower it to match a regression.",
                file=sys.stderr,
            )
            return 1

    if args.check:
        failures, advisories = check(rows, load_baseline(root))
        for advisory in advisories:
            print(f"\nCensus advisory: {advisory}")
        if failures:
            print("\nCensus check failed:", file=sys.stderr)
            for failure in failures:
                print(f"  {failure}", file=sys.stderr)
            return 1
        print("\nCensus check passed: no new mirror files, no fabricated skips.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
