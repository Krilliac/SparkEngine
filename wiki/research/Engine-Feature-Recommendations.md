# Engine Feature Recommendations

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (research/reference)
>
> **Platform/Backend Scope:** All platforms (game-development workflow features)

## Overview

A feature study focused on **game-development workflow** — the day-to-day tools content
creators use — rather than low-level engine architecture (job systems, render graphs,
RHI). Of 15 feature recommendations, 8 already existed and 7 genuinely new systems were
implemented.

This complements the earlier rounds of architecture-focused recommendations by shifting
the lens to what a developer actually touches while building content.

## New Systems Implemented (7)

All seven are present in the current tree (verified 2026-06-08).

| # | System | File | Approx. Lines | Tests |
|---|--------|------|---------------|-------|
| 1 | Animation Notify System | `Engine/Animation/AnimNotify.h` | ~260 | TestAnimNotify.cpp |
| 2 | Gameplay Tags (Hierarchical) | `Engine/Gameplay/GameplayTags.h` | ~280 | TestGameplayTags.cpp |
| 3 | Gameplay Debugger (In-World) | `Utils/GameplayDebugger.h` | ~310 | TestGameplayDebugger.cpp |
| 4 | Screenshot Capture | `Graphics/ScreenCapture.h` | ~280 | TestScreenCapture.cpp |
| 5 | Procedural Generation Framework | `Engine/Procedural/ProceduralGenerator.h` | ~480 | TestProceduralGenerator.cpp |
| 6 | Video Playback System | `Engine/Cinematic/VideoPlayer.h` | ~310 | TestVideoPlayer.cpp |
| 7 | Lightmap Baking (Offline GI) | `Graphics/LightmapBaker.h` | ~480 | TestLightmapBaker.cpp |

## Pre-Existing Systems (8 of 15 Already Existed)

| System | Existing Files |
|--------|---------------|
| Undo/Redo | UndoRedoManager.h, EditorCommand.h, CommandHistory.h, UndoHistoryPanel |
| Content Browser | AssetBrowserPanel.h/.cpp |
| Spline System | SplineComponents.h, SplineEditorPanel |
| Decal System | DecalSystem.h/.cpp, DecalEditorPanel |
| Play-In-Editor | PlayModeManager.h, PlayModeToolbarPanel |
| Object Pooling | ObjectPool.h, TestObjectPool.cpp |
| Terrain System | TerrainRenderer, ClipmapTerrain, TerrainSystem, TerrainEditor |
| Foliage System | FoliageSystem.h |

## Key Patterns

- New systems follow the singleton pattern with `GetInstance()`.
- Header-only implementations, consistent with existing systems.
- `Console_GetStatus()` for debug output.
- Doxygen comments on all public APIs.
- Each system has a dedicated test file.
- All wired into engine init/update/shutdown in the gameplay-systems lifecycle.

## Notes

- `ProceduralGenerator` uses bundled FastNoiseLite for production use (built-in noise
  for tests).
- `VideoPlayer` has a pluggable decoder backend (stub decoder).
- `LightmapBaker` performs real CPU path tracing with a denoise filter.

## Source & Freshness

- **Original entry date:** 2026-04-04 (`.claude/knowledge/engine-feature-recommendations-2026-04-04.md`)
- **Verified against codebase 2026-06-08.**

Updates / status changes since the original:

- **All 7 new systems still present** at the stated paths — Implemented and stable.
- **All 8 pre-existing systems still present** — Implemented.
- Test totals referenced in the original ("3388+ tests") are superseded — the suite has
  since grown to ~6,000 tests; the per-system test files for these features remain in
  place.
- No regressions or removals detected for any of the 15 systems.

## Related Pages

- [Project Recommendations](Project-Recommendations.md)
- [Engine Viability Evaluation](Engine-Viability-Evaluation.md)
- [Third-Party Library Evaluation](Third-Party-Library-Evaluation.md)
- [Mac Compatibility Analysis](Mac-Compatibility-Analysis.md)
