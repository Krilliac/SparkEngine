# Deep Project Optimizations — 2026-04-17

**Type:** Optimization
**Status:** Active
**Last updated:** 2026-04-17

## Description

Hot-path optimizations applied across rendering, ECS, and asset subsystems. Each change is small, measurable, and avoids regression risk (no API churn, no thread-model changes, no invasive refactors). All 5867 tests pass unchanged.

## Context

The project had already completed many wire-up and coverage phases (A–LL). A targeted sweep for per-frame / per-entity / per-draw-call inefficiencies was overdue. A background survey subagent enumerated candidates; the safe, high-impact ones were applied here. Invasive candidates (camera mutex → atomic, lock-free asset queue, std::function → template callbacks) were deferred.

## Changes

### 1. Transparent string hashing utility (`Utils/Hash.h`)

Added `Spark::TransparentStringHash` and `Spark::TransparentStringEqual` structs with `is_transparent = void`. Both route through `std::hash<std::string_view>`, which is guaranteed by the standard to hash the same byte sequence identically whether the caller passes a `std::string`, `std::string_view`, or `const char*`.

Enables any `std::unordered_map<std::string, T, TransparentStringHash, TransparentStringEqual>` to call `.find(string_view)` / `.contains(const char*)` without constructing a temporary `std::string`.

### 2. `AssetPipeline::m_assets` heterogeneous lookup (`Graphics/AssetPipeline.h`)

Switched from `std::unordered_map<std::string, std::shared_ptr<Asset>>` to the transparent-hash variant.

`AssetPipeline::BindMesh` and `AssetPipeline::BindMaterial` now accept `std::string_view` instead of `const std::string&`. The corresponding call sites in `GraphicsEngine::ProcessDrawList` (Windows) were previously constructing a `std::string` from `cmd.meshPath` (a `string_view`) on every draw call — that allocation is now gone.

Impact: for a 1000-draw frame, this removes ~2000 short-string heap allocations per frame from the per-draw-call inner loop.

### 3. `GraphicsEngine::ProcessDrawList` double-buffered draw list (`GraphicsEngineWindows.cpp`, `GraphicsEngine.h`)

Previously:
```cpp
localDrawList = std::move(m_drawList);   // moves capacity OUT of m_drawList
m_drawList.clear();                       // no-op after move
if (m_drawList.capacity() == 0 && !localDrawList.empty())
    m_drawList.reserve(localDrawList.size()); // allocates every frame
```

Now:
```cpp
std::swap(m_drawList, m_processingDrawList); // preserves capacity on both sides
```

Added `m_processingDrawList` as a persistent member. After draining, the processing buffer is `.clear()`'d (preserves capacity) and becomes the empty target of next frame's swap. Steady-state: zero heap allocations on the draw-list management path.

### 4. Sprite animator frame-count caching (`Engine/ECS/Systems/Systems2D.h`)

Previously called `clip->frames.size()` four times inside the per-entity animator update loop (and a `static_cast<int>` each time). Hoisted to `const int frameCount = ...` once per entity, with `lastFrameIdx` cached for the non-loop end case. Eliminates three redundant size queries per animated sprite per frame.

### 5. `MaterialSystem::m_materials` and `m_textureCache` heterogeneous lookup (`MaterialSystem.h`)

Same treatment as `AssetPipeline::m_assets` — transparent hash/equal on both maps. Public API (`GetMaterial(const std::string&)`) unchanged; the win is on internal paths and future `string_view` callers.

## Deferred / Rejected Candidates

| Candidate | Why deferred |
|-----------|--------------|
| Camera mutex → `std::atomic<XMFLOAT3>` | Atomic float triples aren't lock-free on most x86-64 ABIs; changing thread sync model needs dedicated race testing |
| `AssetPipeline` async callback `std::function` → template `Callback&&` | Major API change, ripples to every async-loading caller |
| `m_loadQueue` lock-free replacement | Significant rearchitecture; wrong scope for a drive-by optimization |
| LRU cache on `MaterialPropertyHandle::FindProperty` | No evidence per-property lookups dominate; premature |
| Cache `ClipmapTerrain::GetInstance()` singleton | Already a Meyers singleton — single atomic init check, compiler inlines the rest |
| Physics2D `FindBody` "redundant" lookup | False positive: the two loops process disjoint body types (kinematic/dynamic); dynamic bodies are looked up once per phase by design |

## Verification

- `cmake --build build/linux-gcc-release --config Release --parallel $(nproc)` → clean build (ODR warnings pre-existing, unrelated)
- `ctest --output-on-failure -j $(nproc)` → 5867/5867 tests pass
- No public API signature changes (only internal storage types + one internal helper pair `BindMesh`/`BindMaterial` whose only callers already passed `string_view`)

## Related

- [Build optimizations](build-optimizations.md)
- [Codebase observations](codebase-observations.md)
- Engine architecture per `CLAUDE.md`
