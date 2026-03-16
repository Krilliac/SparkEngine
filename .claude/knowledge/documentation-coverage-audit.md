# Documentation Coverage Audit

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** Medium

## Description

53 wiki pages (29,583 lines), 245/246 headers with Doxygen comments, near-complete API coverage. However, critical reference docs are missing: threading model, memory management rules, networking protocol spec, asset format specs. 19 editor subsystems have no dedicated wiki page.

---

## Wiki Coverage (53 pages)

### Statistics
- **Total pages**: 53 (excluding _Sidebar.md)
- **Total lines**: 29,583
- **Average**: 558 lines/page

### Top pages by size

| Page | Lines | Topic |
|------|-------|-------|
| Shader-Pipeline.md | 937 | Shader architecture, compilation |
| Content-Delivery.md | 915 | Streaming, DLC, content updates |
| Networking.md | 899 | UDP, replication, lag compensation |
| AI-and-Navigation.md | 837 | Behavior trees, NavMesh, perception |
| Replay-System.md | 819 | Recording, playback, state snapshots |
| VR-Support.md | 783 | OpenXR, controllers, tracking |
| Visual-Scripting.md | 778 | Node graph, blueprint system |
| Coroutine-System.md | 772 | Async scheduler, patterns |
| Asset-Pipeline.md | 749 | Import, compression, LOD, streaming |
| Save-System.md | 743 | Serialization, compression, slots |

All 25+ engine subsystems have wiki pages. No stale documentation detected — all pages reflect current system state.

---

## API Documentation (Doxygen)

- **245/246 headers** have `@file` + `@brief` (99.6%)
- **Missing**: UISystem.h (1 file, zero Doxygen)
- **Quality**: All sampled headers have comprehensive `@param`, `@return`, `@code` examples
- **Auto-generated API docs**: Infrastructure exists (`generate-api-docs.sh`) but `docs/api/` not yet generated

---

## CLAUDE.md Accuracy

Verified: All listed directories exist, no critical omissions. Architecture section accurately reflects actual structure.

---

## README.md

489 lines covering: project overview, feature matrix, quick start (Windows/Linux), 30+ CMake toggles, CI workflows, platform matrix, dependencies, templates.

**Issue**: README says "35 unit tests" but CLAUDE.md says "82+ unit tests" — inconsistency.

---

## Top 10 Documentation Gaps

| Priority | Gap | Impact |
|----------|-----|--------|
| 1 | **Threading model & concurrency rules** | Deadlocks, race conditions |
| 2 | **Networking protocol wire format** | Can't build external clients |
| 3 | **Asset format specifications** | Can't build external tools |
| 4 | **Memory management patterns** | Leaks, fragmentation |
| 5 | **Plugin ABI stability & versioning** | Breaking module changes |
| 6 | **Shader compilation constraints** | Cross-platform failures |
| 7 | **Save system binary layout** | Version migration unclear |
| 8 | **19 editor subsystem pages** | Can't extend editor |
| 9 | **Physics solver tuning guide** | Instability in complex scenes |
| 10 | **AI behavior tree serialization** | Can't author trees externally |

---

## Editor Subsystems Without Wiki Pages (19)

Animation, AssetBrowser, AssetPipeline (editor), Gizmos, Integration, LevelStreaming, Lighting, MaterialEditor, Panels base, Prefabs, Profiler (editor UI), Reflection, SceneSystem (editor), Search, Terrain (editor tools), UndoRedo, VersionControl, VisualScripting (editor UI)

Only `SparkEditor.md` (504 lines) covers the editor — all 19 subsystems are undocumented.

---

## docs/ Directory

```
docs/
├── README.md (126 lines)
├── ARCHITECTURE_ANALYSIS.md
├── defensive-offensive-programming-analysis.md
└── gap-analysis/
    ├── GAP_ANALYSIS_TEMPLATE.md (735 lines)
    ├── NETWORKING_GAP_ANALYSIS.md (104 lines)
    └── NEW_FEATURES_GAP_ANALYSIS.md (113 lines)
```

Missing: `docs/api/` (auto-generated), `FEATURE_ROADMAP.md`, `PROJECT_STATUS.md` (referenced in README but not found).

---

## What's Done Right

- 53 wiki pages covering all major subsystems
- 99.6% header Doxygen coverage
- No stale documentation detected
- CLAUDE.md accurately reflects codebase
- Comprehensive README with build instructions
- Gap analysis infrastructure exists
