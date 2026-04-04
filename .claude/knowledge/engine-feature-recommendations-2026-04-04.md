# Engine Feature Recommendations — Game-Making Features

**Last updated:** 2026-04-04
**Type:** Decision
**Status:** Active

## Description

Analysis identified 15 feature recommendations for SparkEngine, of which 8 already existed
(Undo/Redo, Content Browser, Spline System, Decal System, Play-In-Editor, Object Pool,
Terrain System, Foliage System). 7 genuinely new systems were implemented.

## Context

Previous ~20 engine studies and 100+ recommendations focused on engine architecture
(job systems, render graphs, RHI abstractions, GPU acceleration). This session shifted
focus to **game development workflow** — tools developers use daily to create content.

## New Systems Implemented (7)

| # | System | File | Lines | Tests |
|---|--------|------|-------|-------|
| 1 | Animation Notify System | `Engine/Animation/AnimNotify.h` | ~260 | TestAnimNotify.cpp (10 tests) |
| 2 | Gameplay Tags (Hierarchical) | `Engine/Gameplay/GameplayTags.h` | ~280 | TestGameplayTags.cpp (13 tests) |
| 3 | Gameplay Debugger (In-World) | `Utils/GameplayDebugger.h` | ~310 | TestGameplayDebugger.cpp (11 tests) |
| 4 | Screenshot Capture | `Graphics/ScreenCapture.h` | ~280 | TestScreenCapture.cpp (9 tests) |
| 5 | Procedural Generation Framework | `Engine/Procedural/ProceduralGenerator.h` | ~480 | TestProceduralGenerator.cpp (12 tests) |
| 6 | Video Playback System | `Engine/Cinematic/VideoPlayer.h` | ~310 | TestVideoPlayer.cpp (12 tests) |
| 7 | Lightmap Baking (Offline GI) | `Graphics/LightmapBaker.h` | ~480 | TestLightmapBaker.cpp (9 tests) |

## Pre-Existing Systems (8 of 15 already existed)

| System | Existing Files |
|--------|---------------|
| Undo/Redo | UndoRedoManager.h, EditorCommand.h, CommandHistory.h, UndoHistoryPanel |
| Content Browser | AssetBrowserPanel.h/.cpp (460 lines) |
| Spline System | SplineComponents.h, SplineEditorPanel |
| Decal System | DecalSystem.h/.cpp, DecalEditorPanel |
| Play-In-Editor | PlayModeManager.h, PlayModeToolbarPanel |
| Object Pooling | ObjectPool.h, TestObjectPool.cpp |
| Terrain System | TerrainRenderer, ClipmapTerrain, TerrainSystem, TerrainEditor |
| Foliage System | FoliageSystem.h |

## Key Patterns

- All new systems follow singleton pattern with `GetInstance()`
- Header-only implementations (consistent with existing systems)
- Console_GetStatus() for debug output
- Doxygen comments on all public APIs
- Each system has dedicated test file
- All wired into engine init/update/shutdown in GameplaySystemLifecycle.cpp

## Notes

- Total new tests: ~76 across 7 files
- All 3388+ tests pass
- clang-format applied to all files
- ProceduralGenerator uses bundled FastNoiseLite for production use (built-in noise for tests)
- VideoPlayer has pluggable decoder backend (stub decoder for now)
- LightmapBaker performs real CPU path tracing with denoise filter
