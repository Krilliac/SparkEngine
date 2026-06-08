# Project Recommendations

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (research/reference)
>
> **Platform/Backend Scope:** All platforms (production-readiness infrastructure)

## Overview

A gap analysis targeting the distance between "feature-rich engine" and "engine you can
ship games with," followed by implementation of 13 production-infrastructure systems
plus 3 bonus systems. The focus is shipping concerns: packaging, asset validation,
format versioning, accessibility, cross-platform input, performance regression testing,
project templates, telemetry, achievements, shader hot-reload, and community
contribution files.

## New Systems Implemented (13 + 3 Bonus)

All systems are present in the current tree (verified 2026-06-08).

| # | System | File | Approx. Lines | Tests |
|---|--------|------|---------------|-------|
| 1 | Game Packaging Pipeline | `Core/GamePackager.h` | ~586 | TestGamePackager.cpp |
| 2 | Asset Validation | `Core/AssetValidator.h` | ~582 | TestAssetValidator.cpp |
| 3 | Asset Format Versioning | `Core/AssetMigration.h` | ~469 | TestAssetMigration.cpp |
| 4 | Accessibility | `Engine/Accessibility/AccessibilitySystem.h` | ~329 | TestAccessibility.cpp |
| 5 | Cross-Platform Input | `Input/PlatformInput.h` | ~437 | TestPlatformInput.cpp |
| 6 | Benchmark Framework | `Utils/BenchmarkFramework.h` | ~611 | TestBenchmarkFramework.cpp |
| 7 | Templates (4 new) | `Templates/FPS,RPG,Platformer,Multiplayer` | — | — |
| 8 | UI Layout Extensions | `Engine/UI/UILayoutExtensions.h` | ~857 | TestUILayoutExtensions.cpp |
| 9 | Telemetry | `Utils/Telemetry.h` | ~438 | TestTelemetry.cpp |
| 10 | Achievements | `Engine/Gameplay/AchievementSystem.h` | ~463 | TestAchievementSystem.cpp |
| 11 | Shader Hot-Reload | `Graphics/ShaderHotReload.h` | ~394 | TestShaderHotReload.cpp |
| 12 | Community Files | `CHANGELOG.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md` | — | — |
| 13 | API Changelog | `tools/api-changelog.py` | ~293 | — |
| B1 | Runtime Prefabs | `Engine/ECS/RuntimePrefab.h` | ~478 | TestRuntimePrefab.cpp |
| B2 | Editor Tutorials | `SparkEditor/Source/Core/TutorialSystem.h` | ~531 | TestTutorialSystem.cpp |
| B3 | Golden Image Tests | `Utils/GoldenImageTest.h` | ~559 | TestGoldenImageTest.cpp |

> **Path note (2026-06-08):** A `GamePackager.h` exists both at
> `Core/GamePackager.h` (original) and at `Engine/Build/GamePackager.h`. Confirm which
> is canonical before extending packaging code.

## CLI Extensions

`spark-cli` supports `package`, `validate`, `migrate`, and `templates` subcommands.

## Key Patterns Used

- New systems follow the singleton pattern with `GetInstance()`.
- Header-only implementations, consistent with many existing systems.
- `Console_GetStatus()` for debug output.
- Doxygen comments on all public APIs.
- Each system has a dedicated test file.

## Context

At the time of the original analysis SparkEngine had 447K+ lines of C++, 30+ working
subsystems, and ~3,119 tests, but lacked production infrastructure: no game packaging,
no asset validation, no format versioning, no accessibility, limited cross-platform
input, no performance regression testing, only one project template, basic UI widgets,
no telemetry, undocumented achievements, no shader hot-reload, and no community
contribution files. The 16 systems above close those gaps.

## Source & Freshness

- **Original entry date:** 2026-04-04 (`.claude/knowledge/project-recommendations-2026-04-04.md`)
- **Verified against codebase 2026-06-08.**

Updates / status changes since the original:

- **All 16 systems (13 + 3 bonus) still present** at the stated paths — Implemented.
- New observation: a second `GamePackager.h` exists under `Engine/Build/` alongside the
  original `Core/GamePackager.h` — flagged above to avoid duplicate-system drift.
- Test totals referenced in the original ("3311+ tests") are superseded — the suite has
  grown to ~6,000 tests; the per-system test files remain in place.
- No regressions or removals detected.

## Related Pages

- [Engine Feature Recommendations](Engine-Feature-Recommendations.md)
- [Engine Viability Evaluation](Engine-Viability-Evaluation.md)
- [Third-Party Library Evaluation](Third-Party-Library-Evaluation.md)
- [Mac Compatibility Analysis](Mac-Compatibility-Analysis.md)
