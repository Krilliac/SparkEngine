# Test Suite Audit — Coverage Gaps and Orphaned Tests

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** Medium

## Description

82 test files exist. 71 compile (in CMakeLists.txt). 11 are orphaned (not in CMake). 3 test dead code. 14 major subsystems have zero test coverage.

## 11 Orphaned Test Files (Not in CMakeLists.txt)

These test files exist in `Tests/` but are NOT listed in `Tests/CMakeLists.txt` and never compile:

| File | Lines | Assertions |
|------|-------|-----------|
| TestAchievementSystem.cpp | 84 | 8 |
| TestClientPrediction.cpp | 91 | 7 |
| TestClothSimulation.cpp | 79 | 6 |
| TestDestructionSystem.cpp | 83 | 5 |
| TestDialogueSystem.cpp | 100 | 14 |
| TestInputBindings.cpp | 71 | 7 |
| TestLoadingScreen.cpp | 76 | 4 |
| TestLocalizationSystem.cpp | 80 | 6 |
| TestReplaySystem.cpp | 104 | 10 |
| TestUISystem.cpp | 91 | 12 |
| TestVisualScriptSystem.cpp | 298 | 21 |

**Fix:** Add all 11 to `Tests/CMakeLists.txt` lines 10-82.

## 3 Tests Testing Dead Code

| Test File | Dead Header | Status |
|-----------|------------|--------|
| TestTween.cpp (342 lines) | Tween.h (0 engine usage) | Reimplements locally |
| TestChromeTracing.cpp (83 lines) | ChromeTracing.h (0 engine usage) | Includes dead header |
| TestDebugTools.cpp (514 lines) | MemoryDebugger.h + FrameInspector.h | Tests dead utilities |

When the dead headers are deleted, these tests should be deleted too.

## 14 Major Subsystems With Zero Tests

1. **Core/** — Platform abstraction, service locator
2. **Graphics/** — RHI, render graph, post-processing
3. **Audio/** — XAudio2 integration
4. **Networking/** — NetworkManager, AreaServer, WorldServer
5. **Streaming/** — SeamlessAreaManager, scene transitions
6. **Scripting/** — AngelScript VM, hot-reload
7. **Cinematic/** — Sequencer playback
8. **Gameplay/** — Inventory, quests, weapons
9. **Mobile/** — Mobile platform support
10. **Modding/** — Game modding
11. **Procedural/** — Procedural generation system (utilities tested, not the system)
12. **VR/** — VR headset/controller/tracking
13. **Editor/** — ImGui editor, collaborative editing
14. **SceneManager/** — Scene management

## Test Quality

- 74% of tests use standalone implementations (don't include engine headers) for CI/Linux compatibility
- Assertion density is generally strong (1:5 to 1:9 ratio)
- 12 test files exceed 400 lines (acceptable for standalone test implementations)

## Notes

- Tests are cross-platform (standalone reimplementations avoid DirectXMath dependency)
- TestFramework.h provides EXPECT/ASSERT macros
- Test file naming convention: `Test<SystemName>.cpp`
