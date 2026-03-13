# SparkEngine — Codebase Analysis & Next Steps

> Comprehensive codebase analysis performed March 2026. Covers architecture,
> feature completeness, code quality, and prioritized recommendations.

---

## Status Summary

| Area | Completeness | Quality | Notes |
|------|-------------|---------|-------|
| Graphics (DX11) | 95% | **DONE** | Production-ready primary backend |
| ECS (EnTT) | 95% | **DONE** | 50+ components, full system pipeline |
| Animation | 95% | **DONE** | Skeletal, IK, retargeting, state machines |
| AI / NavMesh | 90% | **DONE** | Behavior trees, perception, steering, NavMesh |
| Physics (Bullet) | 90% | **DONE** | Rigid bodies, raycasts, constraints |
| Editor (ImGui) | 85% | **DONE** | 22 subsystems, 20+ panels |
| Audio (XAudio2) | 85% | **DONE** | 3D spatial, mixing |
| Scripting (AngelScript) | 85% | **DONE** | Hot-reload, entity bindings |
| Testing | 95% | **DONE** | 76 test files, CTest, CI |
| OpenGL backend | 95% | **PARTIAL** | Experimental, GL 4.6 Core |
| Vulkan backend | 90% | **PARTIAL** | Experimental, full skeleton |
| D3D12 backend | 85% | **PARTIAL** | Experimental |
| Networking (UDP) | 50% | **PARTIAL** | Disabled by default, known gaps |
| Metal backend | 20% | **STUB** | macOS header-only stub |
| Steam integration | 5% | **STUB** | 4 TODOs in SteamTransport.h |
| Upscaling (DLSS/FSR) | 10% | **STUB** | UpscalingSystem shell only |
| DXR / Raytracing | 0% | **MISSING** | Not implemented |
| VR / AR | 0% | **MISSING** | Not implemented |

**Totals:** 10 DONE, 4 PARTIAL, 3 STUB, 2 MISSING

---

## Recommendations (Priority Order)

### 1. Harden Networking & Enable by Default — HIGH

**Why:** Networking is the biggest feature gap for an FPS engine. The UDP
implementation exists but is disabled (`ENABLE_NETWORKING=OFF`) with known
thread-safety and reliability gaps.

**What to do:**
- Add reliable message delivery (ACK/sequence-number system) over UDP
- Add connection timeout and reconnection logic
- Fix thread safety: `NetworkManager::m_localClientID` and `m_connectionState`
  modified without mutex in handler callbacks
- Complete client-side prediction and server reconciliation
- Stress-test under packet loss / latency conditions
- Enable `ENABLE_NETWORKING=ON` by default once stable

**Key files:**
- `SparkEngine/Source/Engine/Networking/NetworkManager.h`
- `SparkEngine/Source/Engine/Networking/ClientPrediction.h`
- `SparkEngine/Source/Engine/Networking/UDPTransport.h`

---

### 2. Complete RHI Integration — HIGH

**Why:** The RHI abstraction (`IRHIDevice`, `RHIAdapter`, `RHIFactory`) exists
but `GraphicsEngine` (~5345 LOC combined) still directly manages DX11 state,
bypassing the RHI layer. This blocks true multi-backend support.

**What to do:**
- Route all GPU calls in `GraphicsEngine` through `IRHIDevice`
- Break `GraphicsEngine` god class into focused managers (device, swap chain,
  resource, pipeline state)
- Validate OpenGL and Vulkan backends work end-to-end through RHI
- Remove direct D3D11 calls from engine-level code (only in D3D11Device backend)

**Key files:**
- `SparkEngine/Source/Graphics/GraphicsEngine.h`
- `SparkEngine/Source/Graphics/RHI/RHI.h`
- `SparkEngine/Source/Graphics/RHI/RHIAdapter.h`
- `SparkEngine/Source/Graphics/RHI/RHIFactory.h`

---

### 3. Add ECS System Parallelism — MEDIUM

**Why:** `JobSystem` (thread pool with `ParallelFor`) exists but ECS systems
run serially in fixed order: Physics → Animation → AI → Audio → Lifecycle →
Render. Parallelizing independent systems is a significant performance win.

**What to do:**
- Build a system dependency graph (dependencies are already partially declared)
- Run independent systems concurrently via `JobSystem`
- Add `ParallelFor` over entity queries for data-parallel systems (e.g., AI
  perception updates, animation evaluation)
- Ensure thread-safe component access (read-only views for parallel reads)

**Key files:**
- `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h`
- `SparkEngine/Source/Utils/JobSystem.h`

---

### 4. Address Defensive Programming Gaps — MEDIUM

**Why:** Existing gap analysis documents 100+ issues: missing null checks after
`.As<T>()` opaque handle casts, unchecked array bounds, unsafe enum casts.

**What to do:**
- Add validation to `.As<T>()` casts (debug-mode type checks)
- Add bounds checking in mesh loading, cascade shadow maps, animation keyframes
- Use existing `ASSERT_ENUM_RANGE` macro before `static_cast` on enums
- Fix `ProjectManager::m_newProjectPath` uninitialized on Linux when
  `getenv("HOME")` returns null
- Fix `ProjectManager::m_recentProjects` race condition between UI and I/O
  threads

**Key files:**
- `docs/gap-analysis/defensive-offensive-programming-analysis.md` (full list)
- `SparkEngine/Source/Utils/Assert.h`
- `SparkEditor/Source/Core/ProjectManager.h`

---

### 5. Async Asset Loading Pipeline — MEDIUM

**Why:** Asset loading is blocking. Large levels cause stutters and long load
times. The infrastructure exists (`JobSystem`, `ThreadSafeQueue`,
`ENABLE_ASSET_STREAMING`) but isn't wired up end-to-end.

**What to do:**
- Build an asset dependency graph for parallel loading
- Use `JobSystem` + `ThreadSafeQueue` for background I/O
- Add priority-based streaming (nearest-first, visible-first)
- Add loading progress callbacks for UI feedback

**Key files:**
- `SparkEngine/Source/Engine/Streaming/`
- `SparkEditor/Source/AssetPipeline/`
- `SparkEngine/Source/Utils/JobSystem.h`

---

### 6. Upscaling System (FSR 2.0) — LOW

**Why:** Modern games rely on temporal upscaling for performance.
`UpscalingSystem` exists as a shell but lacks implementation.

**What to do:**
- Integrate AMD FSR 2.0 (open-source, cross-platform) as first target
- Hook into the existing temporal effects pipeline
- Add quality presets (Ultra Quality, Quality, Balanced, Performance)

**Key files:**
- `SparkEngine/Source/Graphics/` (UpscalingSystem)

---

### 7. Steam SDK Integration — LOW

**Why:** Only 4 TODO items remain in `SteamTransport.h`. Completing these
enables Steam multiplayer.

**What to do:**
- Link Steamworks SDK
- Implement `Initialize()`, `Shutdown()`, `Send()`, `Receive()`
- Add Steam authentication flow
- Test with Steam test app IDs

**Key files:**
- `SparkEngine/Source/Engine/Networking/SteamTransport.h`

---

### 8. Remove Deprecated Globals — LOW

**Why:** `g_graphics` and `g_input` globals remain in `SparkEngine.cpp`. While
isolated to the executable (not exposed to game modules), removing them
completes the `EngineContext` migration.

**What to do:**
- Replace `g_graphics` / `g_input` usages with `EngineContext::Get()` calls
- Remove global declarations

**Key files:**
- `SparkEngine/Source/Core/SparkEngine.cpp`

---

### 9. Expand Example Game (SparkGame) — LOW

**Why:** `SparkGame` is functional (~70%) but could better showcase engine
capabilities as a reference implementation.

**What to do:**
- Add a complete playable level with AI enemies, physics objects, audio
- Demonstrate cinematic sequencer, dialogue system, quest system
- Add multiplayer example (when networking is stable)

**Key files:**
- `SparkGame/Source/`

---

## Quick Wins

These can be addressed in a single PR each:

1. **Fix `NetworkManager` thread safety** — add mutex around `m_localClientID`
   and `m_connectionState` modifications in handler callbacks
2. **Fix `EngineContext::GetOwned()` return type** — return
   `const std::unique_ptr<T>&` to prevent external `std::move()`/`reset()`
3. **Fix `ProjectManager` race condition** — synchronize `m_recentProjects`
   between UI thread and I/O thread
4. **SteamTransport TODOs** — implement 4 stubbed methods (if Steam SDK
   available)

---

## Codebase Strengths

- **C++20 throughout**: `constexpr`, concepts, structured bindings, `std::format`
- **Zero `new`/`delete`**: `std::unique_ptr` owning, `ComPtr` for COM, RAII everywhere
- **Strict CI**: 8+ jobs (GCC, Clang, MSVC, ASan, coverage, clang-format, clang-tidy)
- **76 test files**: Comprehensive coverage across all major subsystems
- **Extensive docs**: Auto-generated API docs, 15+ wiki pages, gap analysis files
- **Clean architecture**: `EngineContext` service locator, modular ECS, RHI abstraction
- **Cross-platform**: Windows primary, Linux/macOS experimental with stubs
