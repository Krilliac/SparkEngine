---
name: sparkengine-rendering-rhi-rendergraph-and-shaders
description: >-
  Canonical reference for SparkEngine's render backend layer: RHI backend ownership and
  selection (D3D11/D3D12/Vulkan/OpenGL/Metal/NullRHI), RHI resource lifetime rules,
  RenderGraph version/dependency/alias contracts and its exception-throwing validation,
  NullRHIDevice headless behavior, DXR and HiZ capability truth, and the shader compile
  workflow (runtime cross-compile, disk cache, shader hot reload, offline tools,
  SparkShaderCompiler CLI flags and internals).
  TRIGGER when: writing or debugging render-graph passes, "RenderGraph throws
  logic_error", "multiple producers", resource aliasing, adding an RHI backend call,
  "which backend runs where", "why does headless/NullRHI do X", forcing a backend
  (SPARK_RHI_BACKEND), DXR / ray tracing availability, HiZ occlusion culling, shader
  compilation failures at runtime, SparkShaderCompiler CLI usage or extending the tool,
  or asking what rendering claims are actually certified.
  DO NOT TRIGGER when: importing meshes/textures or package integrity
  (use sparkengine-assets-import-and-package-integrity), or running/registering tests
  in general (use sparkengine-validation-and-qa).
---

# Rendering: RHI, RenderGraph, and Shaders

Ground truth as of **2026-08-23**, verified against the working tree of branch
`claude/whole-nine-yards-20260823` (uncommitted changes ahead of `0e1fe7e7`;
includes the RenderGraph/NullRHI hardening from `71761b0e`). All paths repo-relative.

**Jargon, defined once:**

- **RHI** — Render Hardware Interface: the backend-agnostic GPU abstraction in
  `SparkEngine/Source/Graphics/RHI/`. One `IRHIDevice` interface, five real backends
  (`D3D11/`, `D3D12/`, `Vulkan/`, `OpenGL/`, `Metal/`) plus the headless `NullRHIDevice`.
- **RenderGraph** — a declarative frame graph (`SparkEngine/Source/Graphics/RenderGraph.h`):
  you declare passes and their resource reads/writes, it computes execution order, culls
  dead passes, and aliases transient memory.
- **Transient resource** — a texture/buffer created and destroyed inside one graph
  execution. **Imported resource** — externally owned (e.g. the back buffer); the graph
  never allocates or frees it.
- **SSA versioning** — every write to a graph resource produces a new `{index, version}`
  handle. "SSA" (single static assignment) means each version has exactly one producer pass.
- **DXR** — DirectX Raytracing. **HiZ** — hierarchical Z-buffer pyramid used for GPU
  occlusion culling.

## 1. Capability truth (do not oversell)

From the generated readiness ledger `docs/readiness/ENGINE_READINESS_HANDOFF.md`
(regenerate/verify: `python3 tools/site-data/render_handoff.py --check`). Gate **G09
"Renderer parity and visual correctness" is `blocked`** — *no* backend is certified yet.
Never describe a backend as "production-ready"; the readiness contract forbids it while
gates are open.

| Capability | Implementation | Support | Release state | Open work item |
|---|---|---|---|---|
| `rendering.d3d11` | complete | **primary** | candidate | `RHI-210` (golden scenes, device-loss, budgets) |
| `runtime.nullrhi` | complete | supported | candidate | `HEAD-220` (packaged headless/server cert) |
| `rendering.d3d12` | functional | experimental | blocked | `RHI-225` (sync/pass/shader/driver parity) |
| `rendering.vulkan` | functional | experimental | blocked | `RHI-230` (GPU-backed parity, shader toolchain) |
| `rendering.opengl` | functional | experimental | blocked | `RHI-240` (translation/visual/software paths) |
| `rendering.metal` | partial | experimental | blocked | `RHI-220` (backend completion) |

Rules of thumb:

- New rendering features land on **D3D11 first** (Windows primary path); other backends
  get the RHI-level plumbing plus an honest "experimental" label.
- If you close part of an `RHI-2xx` item, update the readiness contract in the **same
  commit** (see the handoff's promotion rules) — do not just edit prose.

## 2. Backend ownership and selection

Selection lives in `SparkEngine/Source/Graphics/RHI/RHIFactory.cpp`
(`GetRecommendedBackend()` / `CreateDevice()`):

1. **Env override**: `SPARK_RHI_BACKEND=<d3d11|d3d12|vulkan|opengl|null|...>` is honored
   before platform defaults. An unavailable named backend logs a warning and falls back to
   auto. `SPARK_RHI_BACKEND` parsing to `None` forces NullRHI (headless). This is the
   supported way to force a backend without recompiling — use it in CI and headless runs.
2. **gVisor detection**: under gVisor the factory recommends NullRHI (Wine signal-handler
   incompatibility) unless explicitly overridden.
3. **Platform priority**: D3D11 on Windows, Metal on Apple, Vulkan on Linux, otherwise the
   first backend `DetectAvailableBackends()` found.
4. **Last resort**: `CreateDevice(GraphicsBackend::None)` → `std::make_unique<NullRHIDevice>()`.
   The engine never fails to produce a device; it degrades to headless.

`GraphicsBackend` enum (`RHI/RHITypes.h`): `D3D11, D3D12, Vulkan, OpenGL, Metal, Auto, None`.

CMake toggles (root `CMakeLists.txt`): `ENABLE_VULKAN` (ON), `ENABLE_OPENGL` (ON),
`ENABLE_METAL` (ON only on Apple), `ENABLE_DXR` (ON), `ENABLE_HYBRID_RT` (ON).
`SPARK_HARDWARE_RT=1` is defined only when `ENABLE_HYBRID_RT` + `ENABLE_DXR` + Windows
MSVC (**not** MinGW).

**Bridge vs adapter:** `RHI/RHIBridge.*` is the glue letting the legacy D3D11
`GraphicsEngine` run through the RHI so backends can be switched without touching
rendering code; `RHI/RHIAdapter.*` is the graph-side adapter. Do not add direct
`ID3D11*` calls to backend-neutral code — go through `IRHIDevice`/`IRHICommandList`.

## 3. NullRHIDevice behavior (headless truth)

`RHI/NullRHIDevice.h` + `RHI/NullRHIResources.h`. Since the Phase Y hardening,
`Create*` methods return **real stub objects, never `nullptr`**:

- Every `CreateBuffer/CreateTexture/CreateShader/CreateSampler/CreatePipelineState`
  returns a `unique_ptr` to a `Null*` object **and** registers the raw pointer in a
  `HandlePool<T, Tag, 256>` (`kPoolCapacity = 256`) used as a debug/validation counter —
  the caller's `unique_ptr` still owns the object.
- `MapBuffer` returns CPU-backed storage from `NullBuffer`; `UpdateBuffer` writes into it.
- A `TransientBufferAllocator` (64 KB vertex + 32 KB index) is initialized in
  `Initialize()` and pumped from `BeginFrame()/EndFrame()`, so headless tests exercise the
  full per-frame transient lifecycle.
- Reported caps: backend `None`, 0 VRAM, `maxTextureSize` 16384, `maxRenderTargets` 8,
  compute **yes**, tessellation/geometry shaders **no**, MSAA 1.
- `NullCommandList` counts draws/dispatches; `NullSwapChain` keeps a real `NullTexture`
  back buffer and rotates a buffer index on `Present`.
- `Shutdown()` tears down the transient allocator **before** clearing the pools; stats
  reset. Test hooks: `GetNullStats()`, `GetBufferPool()` … `GetTransientBuffers()`.

Tests: `Tests/TestNullRHIDevice.cpp`, `Tests/TestNullRHIDevicePhaseY.cpp`.

## 4. RHI resource lifetime rules

- **Ownership**: `Create*` returns `std::unique_ptr` — caller owns. Raw pointers passed
  into command lists are non-owning (engine-wide rule).
- **Frame-delayed destruction**: `RHI/DeferredDeletionQueue.h` frees resources N frames
  after queueing so in-flight command lists never see a use-after-free. Use it whenever a
  resource may still be referenced by submitted GPU work.
- **Transient per-frame memory**: `RHI/TransientBufferAllocator.h` (RHI level) vs
  `RenderGraph/TransientResourcePool.h` (graph level, age-based recycling by descriptor
  match, GC after `maxIdleFrames`). These are different systems — don't conflate them.
- **Validation**: `RHI/RHIValidationLayer.*` wraps a device for call validation;
  `RHI/PipelineStateCache.*` caches PSOs.
- Graph-transient GPU objects are allocated in `RenderGraph::Execute()` and released at
  the end of the same `Execute()` (and on exception). Do not cache
  `registry.GetTexture(...)` pointers across frames.

## 5. RenderGraph contracts (version / dependency / alias)

Headers: umbrella `Graphics/RenderGraph.h`; types `RenderGraphTypes.h`; pass + builder
DSL `RenderGraphPass.h`; blackboard `RenderGraphBlackboard.h`.

### Lifecycle

```cpp
RenderGraph graph("MainFrame", d3dDevice);   // device optional — see "analysis-only"
graph.AddPass("ToneMap", RenderGraphPassType::Compute, setupLambda, executeLambda);
graph.Compile();     // validate → cull → topo-sort → lifetimes → aliasing → async tags
graph.Execute();     // allocate transients → run passes → release transients
graph.Clear();       // graph is single-use; Clear() (or destroy) before rebuilding
```

- `Execute()` **before** a successful `Compile()` throws `std::logic_error`.
- `AddPass()` invalidates any prior compile, and is **exception-safe**: if your setup
  lambda throws, every resource mutation it made is rolled back and the exception
  re-thrown. `Compile()` likewise rolls back scheduling/alias state on failure.
- `Clear()` preserves the blackboard; call `GetBlackboard().Clear()` separately if needed.

### Handle/version (SSA) rules — these throw on violation

Handles are `{index, version}`. `builder.Write(h)` returns `h` with `version+1`;
subsequent readers **must use the returned handle**. `Compile()` runs
`ValidateResourceVersions()` which throws `std::logic_error` when:

| Violation | Message contains |
|---|---|
| Two passes write the same version (write-after-write on a stale handle) | `resource version has multiple producers` |
| A version has no recorded producer | `missing version producer` |
| Read/write/create uses an invalid handle | `uses an invalid resource` |
| Handle version was never produced | `unknown version` / `unproduced version` |
| A write is not the unique producer of its version | `write is not the unique producer` |
| A create isn't version 0 produced by that pass | `create has an invalid producer` |
| Dependency cycle in the pass DAG | rejected in topo-sort |

Practical rule: **thread the handle returned by `Write()` forward**; never reuse the
pre-write handle for later passes.

### Dead-code elimination

A pass survives culling only if (a) it called `builder.SideEffect()` (e.g. presents,
writes to disk) or (b) it writes an **imported** resource, or (c) something a surviving
pass reads depends on it (backward flood-fill). If your pass "doesn't run", check
`graph.GetStats().culledPasses` and `DebugVisualize()` — you probably forgot
`SideEffect()` or nobody reads your output.

### Aliasing contract

- `builder.Alias(from, to)` is a **hint that is hard-validated**, not best-effort.
  `Compile()` throws `std::logic_error` if: either handle invalid/unknown version, either
  resource is Imported or unused, descriptors are not **exactly** equal (texture: width,
  height, depth, arraySize, mipLevels, sampleCount, format, usage; buffer: sizeBytes,
  stride, usage), lifetimes overlap, the destination already aliases/owns aliases, or the
  alias chain has a cycle.
- Automatic aliasing then groups remaining compatible, lifetime-disjoint transients
  (sorted by first use). Aliased resources share the root's physical
  `RenderTarget`/`ID3D11Buffer` at allocation time.
- Savings are reported in `GetStats().aliasedResources` / `savedByAliasing`.

### Analysis-only mode (important for tests/headless)

The `ID3D11Device*` constructor argument is optional. With no device,
`AllocateTransientResources()` returns early — the graph compiles and executes pass
callbacks, but `registry.GetTexture()` returns `nullptr` for **transient** resources
(imported ones still resolve). This is exactly how `Tests/TestRenderGraph.cpp` runs
graph-contract tests with no GPU. Guard execute lambdas accordingly.

### Debugging a graph

- `graph.DebugVisualize()` — human-readable passes/resources/order/stats.
- `graph.ExportDot()` — Graphviz: `dot -Tpng graph.dot -o graph.png`.
- `graph.Console_GetGraphStats()` — one-line summary.
- `RenderGraph/RenderGraphExporter.h` — standalone pass-list → `.dot` exporter.

### Wiring truth (verify before relying)

- The live path is `RenderingPipeline::RenderGraphBased` in
  `Graphics/GraphicsEngineWindowsFrame.cpp` → `RenderPipeline::ExecuteFrame()`
  (`Graphics/RenderPipeline.cpp`), which **clears, rebuilds, compiles, and executes the
  graph every frame** from registered passes (Shadow → Geometry → Lighting → …).
- `StandardPipelineBuilder` (`RenderGraph/RenderGraphBuilder.h/.cpp`) is a parallel
  canonical-pipeline builder that, at `0e1fe7e7`, is **not referenced outside its own two
  files** — treat it as `candidate`/unwired. If you touch it, either wire it in or delete
  it (project rule: "Wiring Things In").

## 6. DXR and HiZ capability truth

**DXR** (`RHI/DXRSupport.h/.cpp`, wiki `wiki/graphics/DXR-Raytracing.md`):

- Requires D3D12 + DXR-capable GPU + `SPARK_HARDWARE_RT` (Windows MSVC only; MinGW
  builds exclude `DXRSupport.cpp` entirely via `SPARK_NO_D3D12`).
- The pipeline (BLAS/TLAS, per-PSO shader tables, reflections/shadows/AO/GI dispatch) is
  wired end-to-end, but it loads **pre-compiled DXIL** blobs: CMake compiles
  `Shaders/HLSL/RayTracing/{DXRReflections,DXRShadows,DXRAO,DXRGI}.hlsl` as `lib_6_3`
  post-build **only if `dxc.exe` is found** (Windows SDK or Vulkan SDK). Missing `.cso`
  ⇒ warning + that trace method becomes a **no-op**, not a crash.
- Test coverage is API-contract only (`Tests/TestDXRSupport.cpp`: flag ops, uninitialized
  no-op safety, settings round-trip). No device-level DXR test exists — the note inside
  `DXRSupport.h` claiming "no Tests/ files" predates this file and is stale. GPU-level
  certification is `open` (part of `RHI-225`/G09).

**HiZ occlusion culling** (`Graphics/GPUOcclusionCulling.h/.cpp`): previous-frame HiZ
pyramid, coarse-to-fine bounding-box tests (Depth Prepass → Build HiZ → Cull → Draw).
Has a CPU-side HiZ data path used by `Tests/TestOcclusionCulling.cpp`; the GPU/UAV path
exists but has no golden/visual certification (G09 `blocked`). Related: `Graphics/GTAOEffect.h`,
`Graphics/OcclusionCulling.h` (CPU variant), `Graphics/GPUDrivenRenderer.*`.

## 7. Shader compile workflow

Three layers — pick the right one:

| Layer | Where | Use when |
|---|---|---|
| Runtime engine compile | `Graphics/ShaderCompilation{Windows,Linux}*.cpp`, `Shader.*` | Normal engine/editor operation; D3D11 HLSL on Windows, RHI cross-compile elsewhere |
| Runtime services | `ShaderDiskCache.*` (bytecode cached by content-hash of source+defines+target), `ShaderHotReload.*` (dir watcher + auto recompile), `ShaderVariantSystem.h`, `ShaderDaemonBridge.*` (out-of-process compile service) | Iteration speed, avoiding recompiles, live editing |
| Offline tools | `SparkShaderCompiler` CLI (wraps `Spark::RHI::CompileShader`); `Shaders/compile_shaders.sh` / `.bat` (GLSL → SPIR-V via `glslangValidator`, optional `spirv-opt`) | Pre-baking `.cso`/`.spv`, CI, batch validation |

Cross-compile matrix (`RHI/RHIFactory.h::CompileShader`): HLSL→DXBC (D3D11/12),
HLSL→SPIR-V (DXC, Vulkan), GLSL→SPIR-V (glslang), GLSL→native (OpenGL), SPIR-V
passthrough. Reflection via `ReflectSPIRV()`. Shader sources live in `Shaders/HLSL/`,
`Shaders/GLSL/`; compiled output in `Shaders/Compiled/` and `.spv` beside GLSL.

Quick offline check (from repo root, after building the tool):

```bash
./build/windows-release/bin/SparkShaderCompiler Shaders/HLSL/BasicVS.hlsl -stage vertex -backend d3d11 -validate
```

CLI flags, stage inference from filenames, and batch mode are **owned by this skill**:
run the tool with no arguments (or read `SparkShaderCompiler/src/`) for the current
flag set; the tool wraps `Spark::RHI::CompileShader`, so extending it means extending
the cross-compile matrix above and the CLI front-end in `SparkShaderCompiler/src/`.

Tests: `TestShaderCrossCompilerPhaseW.cpp`, `TestShaderDiskCache*.cpp`,
`TestShaderHotReload*.cpp`, `TestShaderVariantSystem.cpp`, `TestShaderGraphCompiler.cpp`.

## 8. Build and run the rendering tests

All tests compile into one executable, `SparkTests`, registered as a single CTest test
named `SparkEngineTests` (`Tests/CMakeLists.txt`). Binaries land in
`build/<preset>/bin/` for every config.

```bash
# Configure + build (Windows, VS 2022 generator)
cmake --preset windows-release
cmake --build --preset windows-release

# All tests via CTest (multi-config generator needs -C)
ctest --test-dir build/windows-release -C Release --output-on-failure

# Only the render-graph tests (env-var filters are built into SparkTests)
SPARK_TEST_FILE=TestRenderGraph.cpp ./build/windows-release/bin/SparkTests.exe

# Only one test by name
SPARK_TEST_NAME=RenderGraph_DependencyCycleIsRejected ./build/windows-release/bin/SparkTests.exe

# Headless smoke of backend selection without a GPU
SPARK_RHI_BACKEND=null <any engine binary>
```

The full selector/flag list is owned by `sparkengine-validation-and-qa` §3.
Key rendering test files: `TestRenderGraph.cpp` (30+ contract tests incl. version-conflict
and cycle rejection), `TestNullRHIDevice*.cpp`, `TestRHIBridgeIntegration.cpp`,
`TestRHICapabilityParity.cpp`, `TestRHIHandlePool*.cpp`, `TestGoldenImageTest.cpp`,
`TestMeshShaderPipeline.cpp`.

On Windows MSVC, prefer building through presets as above; per the user's global build
notes, never let a stray `CC` env var poison CMake configure.

## 9. When NOT to use this skill

| You're doing… | Use instead |
|---|---|
| Mesh/texture/audio import, `AssetPipeline*`, FBX/glTF loading, package integrity | `sparkengine-assets-import-and-package-integrity` |
| Registering/running/filtering tests generally, sanitizers, coverage, golden-image process | `sparkengine-validation-and-qa` |
| Build/configure failures | `sparkengine-build-ci-and-dependencies` |
| Launching/operating the built engine binaries | `sparkengine-run-package-and-release` |
| General runtime debugging not specific to rendering | `sparkengine-debugging-playbook` |
| ECS render-component queries | `sparkengine-ecs-lifecycle-threading-and-memory` |

Wiki deep-dives (human-oriented, may lag code): `wiki/graphics/Render-Graph.md`,
`wiki/graphics/RHI-Abstraction-Layer.md`, per-backend pages (`D3D11-Backend.md`,
`D3D12-Backend.md`, `Vulkan-Backend.md`, `OpenGL-Backend.md`, `Metal-Backend.md`),
`DXR-Raytracing.md`, `Hybrid-Ray-Tracing.md`.

## Provenance and maintenance

Facts verified 2026-08-23 against the working tree (uncommitted changes ahead of
`0e1fe7e7`) by reading source and the generated readiness handoff — not by a full
build/CI run at this exact tree. Python commands: `python3` on Linux, `python` / `py -3`
on Windows. Re-verify before trusting:

```bash
# RenderGraph contracts (throws, SSA, aliasing) still as described
grep -n "ValidateResourceVersions\|hasVersionConflict\|explicit alias" SparkEngine/Source/Graphics/RenderGraph.h

# NullRHI stub-object + pool + transient-allocator wiring
grep -n "kPoolCapacity\|TransientBufferAllocator\|GetCpuPointer" SparkEngine/Source/Graphics/RHI/NullRHIDevice.h

# Backend selection order + SPARK_RHI_BACKEND override
grep -n "SPARK_RHI_BACKEND\|GetRecommendedBackend\|NullRHIDevice" SparkEngine/Source/Graphics/RHI/RHIFactory.cpp

# Capability/gate states (source of truth; regenerate, don't hand-edit)
python3 tools/site-data/render_handoff.py --check
grep -n "rendering.d3d11\|rendering.vulkan\|G09" docs/readiness/ENGINE_READINESS_HANDOFF.md

# StandardPipelineBuilder wiring status (unwired candidate as of 2026-08-23)
grep -rln "StandardPipelineBuilder" SparkEngine SparkEditor Tests GameModules

# DXR dxc post-build step and no-op-on-missing-cso behavior
grep -n "DXC_EXECUTABLE\|lib_6_3" CMakeLists.txt

# Test filters still supported
./build/windows-release/bin/SparkTests.exe --help
```
