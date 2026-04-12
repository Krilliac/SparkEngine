# Engine Next-Steps Session (2026-04-12) — Themes 1-6

- **Type**: Observation
- **Status**: Active
- **Created**: 2026-04-12

## Summary

Comprehensive hardening session across 6 themes. No new features — purely
testing, quality, documentation, and integration.

## Theme 1: Real-Class Tests for 5 Critical Subsystems
- 5 new `Test*Real.cpp` files: EngineSettings, ECSystems, NetworkManager,
  MaterialSystem, AssetPipeline — 77 tests
- Key finding: NetBuffer, LagCompensator, all ECS ISystem interface, PBR
  validation, AssetCache are fully portable on Linux

## Theme 2: Category A Fake-Coverage Batch
- 11 more real-class test files (ConstantBufferDiff, SpringArm, DirtyRectTracker,
  AchievementSystem, SceneConfigDatabase, AbilitySystem, AIDebugRenderer,
  AssertSuppression, LockFreeRingAllocator, GameMode, ContainerUtils) — ~110 tests
- Pattern: `EXPECT_EQ` fails on enum class (no `operator<<`); use `EXPECT_TRUE(a == b)` or cast to int

## Theme 3: Bloat Resolution
- 8 over-threshold files baselined (NullRHIDevice.h, FoliageRenderer.cpp/h,
  PostProcessingPipeline.h, EditorWindowManager.h, EditorLayoutManager.cpp,
  FoliageImpostorBaker.cpp/h)
- FoliageRenderer.cpp (923 lines) has natural CPU/GPU split seam at line ~450
- validate-all.sh: 10/10 passing

## Theme 4: SelectionManager Panel Migration
- HierarchyPanel and InspectorPanel ALREADY use SelectionManager (prior session)
- SceneViewPanel now wired (this session) — tracks m_selectedEntityId
- 4 "unregistered" panel headers (AssetAuditGraph, BuildPipeline, EditorAutomation,
  ProjectBrowserPanel) are correctly NOT panels — they're utilities/singletons
- ID mismatch: SelectionManager uses uint64_t, ECS uses entt::entity (32-bit).
  HierarchyPanel does static_cast. No width validation exists.

## Theme 5: Shallow-Wiring Audit
- 12/18 Tier 2 orphans (Phases I-T) are fully wired with active runtime calls
- 6 lifecycle-only: GTAOEffect, BVHAccelerator, ShaderVariantSystem,
  SoftwareDenoiser, NoiseGraph, VCTSystem — all documented with @note

## Theme 6: Windows CI Parity
- All new tests are portable (zero platform guards)
- VS 2022 runs same test suite (Release only, vs Debug+Release on Linux)
- VS 2026 and MinGW are continue-on-error (deliberate)
- No actionable changes needed from Linux side

## Test Count
- Previous: ~5198 tests
- After: ~5291+ tests (93+ net new)
- 16 new real-class test files total
