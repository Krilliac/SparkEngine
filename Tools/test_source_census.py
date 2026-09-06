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

--check fails when a mirror file appears that is not in MIRROR_BASELINE below,
or when a test named *_Skipped fabricates a pass instead of calling SKIP_TEST.
Fail-closed: an unreadable file, an unresolvable repository root, or an empty
scan is an error, never a silent pass.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Directories whose headers count as production code.
PRODUCTION_ROOTS = (
    "SparkEngine/Source",
    "SparkEditor/Source",
    "SparkSDK/Include",
    "SparkServer/Source",
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
HARNESS_TESTS = frozenset({"Tests/TestRunnerSemanticsReal.cpp"})

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
        "Tests/TestFPSGameplayIntegration.cpp",
        "Tests/TestFPSMultiplayer.cpp",
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
        "Tests/TestGameModeReal.cpp",
        "Tests/TestGameObjectTransforms.cpp",
        "Tests/TestGamepadInputProcessing.cpp",
        "Tests/TestGameplayExtensionRegistry.cpp",
        "Tests/TestGatewayAreaControl.cpp",
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
        "Tests/TestModuleDiscovery.cpp",
        "Tests/TestMovementSystem.cpp",
        "Tests/TestMultiISADispatch.cpp",
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
        "Tests/TestSparkServerApplication.cpp",
        "Tests/TestSpatialGrid.cpp",
        "Tests/TestSplineMath.cpp",
        "Tests/TestSprite2DComponents.cpp",
        "Tests/TestSteeringBehaviors.cpp",
        "Tests/TestStringPool.cpp",
        "Tests/TestSubsystemConsoleCommands.cpp",
        "Tests/TestTFAbilityWire.cpp",
        "Tests/TestTFCaptureMath.cpp",
        "Tests/TestTFChatRules.cpp",
        "Tests/TestTFDamageModel.cpp",
        "Tests/TestTFDeathRecapWire.cpp",
        "Tests/TestTFFixedStep.cpp",
        "Tests/TestTFNetProtocolLayout.cpp",
        "Tests/TestTFOnboarding.cpp",
        "Tests/TestTFOutfitStore.cpp",
        "Tests/TestTFRedeployRules.cpp",
        "Tests/TestTFSecondaryMotion.cpp",
        "Tests/TestTFServerValidation.cpp",
        "Tests/TestTFSocialStore.cpp",
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
    args = parser.parse_args()
    if args.minimum_production_tests is not None and args.minimum_production_tests < 1:
        parser.error("--minimum-production-tests must be at least 1")

    root = repo_root(Path(__file__).resolve().parent)
    rows = scan(root)
    stats = summarize(rows)

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
