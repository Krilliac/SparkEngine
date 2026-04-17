# Render Graph

SparkEngine uses a declarative render graph (frame graph) system to define the rendering pipeline each frame. Passes declare their resource dependencies, and the graph compiler performs topological sorting, dead-code elimination, lifetime analysis, and resource aliasing automatically.

**Source:** `SparkEngine/Source/Graphics/RenderGraph.h` (umbrella), `SparkEngine/Source/Graphics/RenderGraph/`
**Namespace:** `Spark::Graphics`
**Tests:** `Tests/TestRenderGraph.cpp` (25 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Core Concepts](#core-concepts)
  - [Passes](#passes)
  - [Resources](#resources)
  - [Blackboard](#blackboard)
- [RenderGraph API](#rendergraph-api)
  - [AddPass](#addpass)
  - [Compile](#compile)
  - [Execute](#execute)
- [StandardPipelineBuilder](#standardpipelinebuilder)
  - [Pass Dependency Chain](#pass-dependency-chain)
  - [Pipeline Configuration](#pipeline-configuration)
  - [Frame Data](#frame-data)
  - [Callbacks](#callbacks)
  - [Usage](#usage)
- [Transient Resource Pool](#transient-resource-pool)
- [GraphViz Export](#graphviz-export)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

The render graph replaces a hardcoded render loop with a data-driven pipeline. Instead of manually managing render targets, barriers, and pass ordering, each pass declares what it reads and writes. The graph compiler resolves the optimal execution order and resource lifetimes.

```
┌─────────────────────────────────────────────────────────────────┐
│                    StandardPipelineBuilder                       │
│  (Builds the standard deferred pipeline from configuration)     │
├─────────────────────────────────────────────────────────────────┤
│                         RenderGraph                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │ Shadow   │──│ GBuffer  │──│ Lighting │──│ PostProc │──...   │
│  │ Pass     │  │ Pass     │  │ Pass     │  │ Pass     │        │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘        │
│        |              |             |              |             │
│        v              v             v              v             │
│  [shadowAtlas]  [albedo,norm]  [hdrColor]    [ldrColor]        │
│                 [material,mv]                [bloom,ssao]       │
│                 [depth]                                         │
├─────────────────────────────────────────────────────────────────┤
│                    TransientResourcePool                        │
│          (age-based GPU resource recycling)                     │
├─────────────────────────────────────────────────────────────────┤
│                    RenderGraphBlackboard                        │
│         (type-erased inter-pass data sharing)                   │
└─────────────────────────────────────────────────────────────────┘
```

### Key Files

| File | Responsibility |
|------|---------------|
| `RenderGraph.h` | Umbrella header + `RenderGraph` class (AddPass/Compile/Execute) |
| `RenderGraphTypes.h` | Resource handles, descriptors, enums, registry, stats |
| `RenderGraphPass.h` | Pass and builder classes for declaring dependencies |
| `RenderGraphBlackboard.h` | Type-erased data sharing between passes |
| `RenderGraph/RenderGraphBuilder.h` | `StandardPipelineBuilder` and pass data structs |
| `RenderGraph/TransientResourcePool.h` | Age-based GPU resource pooling |
| `RenderGraph/RenderGraphExporter.h` | GraphViz `.dot` file export for debugging |

---

## Core Concepts

### Passes

A render pass is a unit of GPU work (graphics, compute, copy, or async compute). Each pass declares:
- **Reads**: Resources consumed (e.g., shadow atlas for lighting)
- **Writes**: Resources produced (e.g., GBuffer textures)
- **Creates**: New transient resources allocated for this pass
- **Side effects**: Passes that write to the backbuffer or perform I/O

Passes with no consumers for their outputs are eliminated during compilation (dead-code elimination).

### Resources

Resources are identified by `RenderGraphResource` handles — lightweight IDs that reference textures or buffers within the graph. Resources can be:
- **Transient**: Allocated and released within a single frame by the `TransientResourcePool`
- **Imported**: External resources (e.g., the backbuffer) brought into the graph

Resource descriptors (`RenderGraphTextureDesc`) specify dimensions, format, and usage flags.

### Blackboard

The `RenderGraphBlackboard` provides type-erased data sharing between passes. Each pass can write structured data (e.g., `GBufferPassData`) to the blackboard, and downstream passes read it to access resource handles.

---

## RenderGraph API

### AddPass

```cpp
RenderGraphPass& AddPass(
    const std::string& name,
    RenderGraphPassType type,
    std::function<void(RenderGraphBuilder&)> setup,
    std::function<void(const RenderGraphResourceRegistry&)> execute);
```

The `setup` lambda receives a `RenderGraphBuilder` to declare resource dependencies. The `execute` lambda is called at execution time with a registry to resolve handles to GPU objects.

```cpp
RenderGraph graph("MainFrame", d3dDevice);

graph.AddPass("ToneMapping", RenderGraphPassType::Compute,
    [&](RenderGraphBuilder& builder)
    {
        hdrInput = builder.Read(hdrInput);
        ldrOutput = builder.Write(ldrOutput);
    },
    [=](const RenderGraphResourceRegistry& registry)
    {
        auto* hdr = registry.GetTexture(hdrInput);
        auto* ldr = registry.GetTexture(ldrOutput);
        // dispatch tone mapping compute shader ...
    });
```

### Compile

```cpp
graph.Compile();
```

Compilation performs:
1. **Topological sort** — Orders passes by dependency
2. **Dead-code elimination** — Removes passes with no consumers
3. **Lifetime analysis** — Determines when each resource is first used and last used
4. **Resource aliasing** — Reuses memory for non-overlapping resources
5. **Barrier placement** — Inserts resource transitions between passes

### Execute

```cpp
graph.Execute();
```

Allocates transient resources via the pool, runs passes in compiled order, and releases transient resources. The graph is single-use — call `Clear()` or destroy it after execution.

---

## StandardPipelineBuilder

The `StandardPipelineBuilder` constructs SparkEngine's canonical deferred rendering pipeline as a `RenderGraph`. It is the primary way most rendering code interacts with the graph system.

### Pass Dependency Chain

```
ShadowPass
    |
    v
GBufferPass ─────┐
    |             |
    v             v
LightingPass (reads GBuffer + Shadow)
    |
    v
PostProcessPass (reads HDR color, motion, depth)
    |
    v
UIPass (reads LDR color, writes composited output) [side-effect]
    |
    v
DebugPass (optional, reads depth) [side-effect]
```

### Pipeline Configuration

`PipelineConfig` controls which passes are enabled and their parameters:

| Setting | Default | Description |
|---------|---------|-------------|
| `renderWidth` / `renderHeight` | 1920 x 1080 | Output resolution |
| `renderScale` | 1.0 | Internal resolution multiplier |
| `shadowsEnabled` | true | Enable shadow pass |
| `shadowMapSize` | 2048 | Shadow atlas resolution |
| `shadowCascades` | 3 | Cascade shadow map count |
| `deferredEnabled` | true | Enable deferred GBuffer pass |
| `gBufferCount` | 4 | GBuffer targets (Albedo, Normal, Material, Motion) |
| `hdrEnabled` | true | HDR lighting |
| `hdrFormat` | RGBA16_FLOAT | HDR render target format |
| `bloomEnabled` | true | Post-process bloom |
| `ssaoEnabled` | false | Screen-space ambient occlusion |
| `taaEnabled` | false | Temporal anti-aliasing |
| `motionBlurEnabled` | false | Motion blur |
| `uiEnabled` | true | UI compositing pass |
| `debugPassEnabled` | false | Debug visualization pass |

### Frame Data

`PipelineFrameData` provides per-frame camera and timing data:

```cpp
struct PipelineFrameData
{
    XMMATRIX viewMatrix;
    XMMATRIX projMatrix;
    XMFLOAT3 cameraPosition;
    float nearPlane;
    float farPlane;
    float deltaTime;
};
```

### Callbacks

`PipelineCallbacks` holds user-supplied lambdas that perform actual GPU work in each pass:

```cpp
struct PipelineCallbacks
{
    using ExecuteFn = std::function<void(
        const RenderGraphResourceRegistry&,
        const PipelineFrameData&)>;

    ExecuteFn shadowExecute;
    ExecuteFn gBufferExecute;
    ExecuteFn lightingExecute;
    ExecuteFn postProcessExecute;
    ExecuteFn uiExecute;
    ExecuteFn debugExecute;
};
```

### Usage

```cpp
StandardPipelineBuilder pipelineBuilder;
pipelineBuilder.Configure(config);
pipelineBuilder.SetFrameData(frameData);
pipelineBuilder.SetCallbacks(callbacks);

// Each frame:
RenderGraph graph("MainFrame", d3dDevice);
pipelineBuilder.Build(graph);
graph.Compile();
graph.Execute();
```

### Blackboard Data

Each pass writes structured output to the blackboard for downstream passes:

| Struct | Pass | Contents |
|--------|------|----------|
| `ShadowPassData` | Shadow | `shadowAtlas`, `cascadeCount` |
| `GBufferPassData` | GBuffer | `albedo`, `normals`, `material`, `motion`, `depth` |
| `LightingPassData` | Lighting | `hdrColor` |
| `PostProcessPassData` | PostProcess | `ldrColor`, `bloom`, `ssao` |
| `UIPassData` | UI | `composited` |

---

## Transient Resource Pool

The `TransientResourcePool` manages GPU resources that are allocated and released each frame by render graph passes. Resources are recycled based on descriptor matching and garbage-collected when idle.

```cpp
auto& pool = TransientResourcePool::GetInstance();
pool.Initialize(4);  // destroy resources idle for 4+ frames

pool.BeginFrame(frameIndex);
uint64_t handle = pool.AcquireResource(desc);
// ... use resource ...
pool.ReleaseResource(handle);
pool.GarbageCollect();
```

| Method | Description |
|--------|-------------|
| `Initialize(maxIdleFrames)` | Set up pool with idle frame threshold |
| `BeginFrame(frameIndex)` | Mark start of new frame for age tracking |
| `AcquireResource(desc)` | Get a matching pooled resource or create new |
| `ReleaseResource(handle)` | Return resource to pool (not destroyed) |
| `GarbageCollect()` | Destroy resources idle beyond threshold |
| `GetPooledResourceCount()` | Total resources in pool |
| `GetActiveResourceCount()` | Resources currently in use |
| `GetEstimatedMemoryUsage()` | Approximate VRAM usage in bytes |

Resources are matched by width, height, format, and usage flags. Debug names are ignored during matching.

---

## GraphViz Export

The `RenderGraphExporter` dumps the pass dependency graph as a `.dot` file for visualization:

```cpp
std::vector<RenderPassInfo> passes = { /* ... */ };
RenderGraphExporter::ExportGraphViz(passes, "debug/render_graph.dot");

// Or get the DOT string directly:
std::string dot = RenderGraphExporter::GenerateDotString(passes);
```

Render the output with `dot -Tpng render_graph.dot -o render_graph.png`.

---

## Integration

- **GraphicsEngine**: Owns the `StandardPipelineBuilder` and calls `Build()`/`Compile()`/`Execute()` each frame
- **RHI backends**: The graph uses RHI abstractions for resource creation and barrier management
- **Quality settings**: `PipelineConfig` can be changed between frames when the player adjusts quality
- **Console**: `GetPipelineSummary()` and `TransientResourcePool::Console_GetStatus()` provide debug output

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) — Overall graphics architecture
- [RHI Abstraction Layer](RHI-Abstraction-Layer.md) — Hardware abstraction used by the graph
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) — Shader compilation and management
- [Profiler and Debugging](../advanced/Profiler-and-Debugging.md) — GPU timing and profiling
