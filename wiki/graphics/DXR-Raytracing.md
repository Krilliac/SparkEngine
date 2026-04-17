# DXR Raytracing

## Overview

SparkEngine supports hardware-accelerated ray tracing via DirectX Raytracing (DXR) 1.1. Features include ray-traced reflections, soft shadows, ambient occlusion, and global illumination.

## Architecture

- **Namespace:** `Spark::Graphics`
- **Files:** `Graphics/RHI/DXRSupport.h/.cpp`
- **Dependencies:** D3D12 backend, DXR-capable GPU

## DXRManager

Singleton manager that handles the full DXR pipeline:

```cpp
auto& dxr = DXRManager::GetInstance();
dxr.Initialize(d3d12Device);  // Queries DXR tier

// Build acceleration structures
uint32_t blas = dxr.CreateBLAS(blasDesc);
dxr.BuildTLAS(instances);

// Dispatch ray tracing
dxr.TraceReflections(viewProj, cameraPos);
dxr.TraceShadows(lightDir);
dxr.TraceAmbientOcclusion(viewProj, cameraPos);
dxr.TraceGlobalIllumination(viewProj, cameraPos);
```

## Feature Flags

```cpp
enum class RTFeature : uint32_t {
    Reflections       = 1 << 0,
    Shadows           = 1 << 1,
    AmbientOcclusion  = 1 << 2,
    GlobalIllumination = 1 << 3,
    All               = 0xFFFFFFFF
};
```

## Acceleration Structures

- **BLAS (Bottom-Level):** Per-mesh geometry, supports update/refit for animation
- **TLAS (Top-Level):** Scene-wide instance list with transforms and hit group indices
- Memory tracked via `DXRStats::accelerationStructureMemory`

## Quality Presets

| Preset | Reflections SPP | Bounces | Shadows SPP | Render Scale |
|--------|----------------|---------|-------------|--------------|
| Low | 1 | 1 | 1 | 0.5x |
| Medium | 1 | 1 | 2 | 0.75x |
| High | 2 | 2 | 4 | 1.0x |
| Ultra | 4 | 3 | 8 | 1.0x |

## Inline Shaders

The implementation includes HLSL ray tracing shaders:
- **Reflection RayGen:** Traces reflection rays from G-Buffer normals
- **Shadow RayGen:** Soft shadow rays with jittered sampling
- **AO RayGen:** Cosine-weighted hemisphere sampling
- **GI RayGen:** Multi-bounce diffuse global illumination

## GPU Profiling

Timestamp queries measure per-feature timing:
```cpp
auto stats = dxr.GetStats();
stats.rtReflectionsTimeMs;
stats.rtShadowsTimeMs;
stats.rtAOTimeMs;
stats.rtGITimeMs;
```

## BLAS Build Process

Bottom-Level Acceleration Structures contain the actual triangle geometry for a single mesh. The build process is:

1. **Geometry Description:** Each BLAS is described by one or more `D3D12_RAYTRACING_GEOMETRY_DESC` entries. SparkEngine sets the vertex buffer, index buffer, vertex stride, vertex count, index count, and transform for each sub-mesh.
2. **Prebuild Info Query:** `GetRaytracingAccelerationStructurePrebuildInfo()` returns the required scratch and result buffer sizes.
3. **Buffer Allocation:** The `DXRManager` allocates GPU buffers for the scratch space (temporary) and the result (persistent BLAS data) from a dedicated acceleration structure memory pool.
4. **Build Command:** `BuildRaytracingAccelerationStructure()` is recorded on the command list with `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE` for static geometry or `PREFER_FAST_BUILD` for dynamic geometry.
5. **UAV Barrier:** A UAV barrier is issued after the build to ensure the BLAS is ready before TLAS construction.

```cpp
BLASDesc desc;
desc.vertexBuffer = mesh->GetVertexBufferGPUAddress();
desc.indexBuffer  = mesh->GetIndexBufferGPUAddress();
desc.vertexStride = sizeof(Vertex);
desc.vertexCount  = mesh->GetVertexCount();
desc.indexCount   = mesh->GetIndexCount();
desc.isOpaque     = true;
desc.allowUpdate  = mesh->IsAnimated();  // Enable refit for skeletal meshes

uint32_t blasHandle = dxr.CreateBLAS(desc);
```

## TLAS Build Process

The Top-Level Acceleration Structure references all BLAS instances in the scene with per-instance transforms, hit group indices, and instance masks.

1. **Instance Buffer:** An array of `D3D12_RAYTRACING_INSTANCE_DESC` is filled, one per visible mesh instance. Each entry contains the 3x4 world transform, instance ID, instance mask (for selective ray tracing), and BLAS address.
2. **Build:** The TLAS is rebuilt every frame to account for moving objects. The `ALLOW_UPDATE` flag enables incremental refit when only transforms change.
3. **Performance:** TLAS build is typically under 0.5ms for scenes with fewer than 10,000 instances. SparkEngine uses GPU-driven instance culling to reduce the TLAS instance count.

```cpp
std::vector<TLASInstance> instances;
for (auto [entity, transform, meshRT] : registry.view<Transform, MeshRTComponent>().each())
{
    TLASInstance inst;
    inst.blasHandle   = meshRT.blasHandle;
    inst.transform    = transform.GetWorldMatrix3x4();
    inst.instanceMask = meshRT.rayMask;
    inst.hitGroupIndex = meshRT.hitGroupIndex;
    instances.push_back(inst);
}
dxr.BuildTLAS(instances);
```

## Acceleration Structure Update and Refit

For animated meshes (skeletal animation, vertex deformation), rebuilding the BLAS every frame is expensive. SparkEngine uses a two-tier strategy:

- **Refit:** For meshes where the topology does not change (skeletal animation, blend shapes), the BLAS is updated in-place using the `PERFORM_UPDATE` flag. This recomputes bounding boxes without rebuilding the BVH tree. Refitting is 5-10x faster than a full rebuild but can degrade traversal performance over time.
- **Full Rebuild:** When refit quality degrades beyond a threshold (measured by tracking traversal step count per ray), or after a configurable number of refit frames (default: 60), a full BLAS rebuild is triggered.

```cpp
// Automatic refit/rebuild management
dxr.UpdateBLAS(blasHandle, updatedVertexBuffer);
// Internally tracks refit count and triggers rebuild when needed
```

## Shader Table Layout

The DXR shader table organises shader records into three sections:

| Section | Contents | Record Size |
|---------|----------|-------------|
| **Ray Generation** | One record per ray type (reflections, shadows, AO, GI) | 64 bytes (shader ID + root constants) |
| **Miss** | One record per ray type (return background/zero) | 32 bytes (shader ID only) |
| **Hit Group** | One record per material/geometry combination | 96 bytes (shader ID + material CBV + texture SRVs) |

The hit group table is the largest and is indexed by: `instanceContributionToHitGroupIndex + (rayType * geometryCount) + geometryIndex`. SparkEngine pre-computes this mapping when materials are assigned.

```cpp
// Shader table construction
ShaderTableBuilder builder;
builder.AddRayGenRecord("ReflectionRayGen", reflectionConstants);
builder.AddRayGenRecord("ShadowRayGen", shadowConstants);
builder.AddMissRecord("ReflectionMiss");
builder.AddMissRecord("ShadowMiss");

for (auto& material : materials)
{
    builder.AddHitGroupRecord("DefaultHitGroup",
        material.constantBufferView,
        material.albedoSRV,
        material.normalSRV);
}
auto shaderTable = builder.Build(device);
```

## Shader Descriptions

### Ray Generation Shaders

- **ReflectionRayGen:** Reads the G-buffer normal and roughness. For each pixel with roughness below a threshold, traces a reflection ray along the mirror direction with importance-sampled roughness perturbation. Multiple samples per pixel (SPP) are averaged for smoother results at higher quality presets.
- **ShadowRayGen:** For each light source, traces a ray toward the light with jittered offsets for soft shadows. Uses `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER` for maximum performance since only occlusion is needed.
- **AORayGen:** Traces short-range rays in a cosine-weighted hemisphere above the surface normal. The AO value is the ratio of unoccluded rays. Uses a blue noise texture to distribute ray directions across frames for temporal stability.
- **GIRayGen:** Traces multi-bounce diffuse rays. First bounce uses the G-buffer, subsequent bounces use simplified material evaluation. Irradiance is accumulated in a half-resolution probe grid and filtered spatially.

### Miss Shaders

- **ReflectionMiss:** Returns the sky/environment map colour sampled along the ray direction.
- **ShadowMiss:** Returns 1.0 (fully lit) since no occluder was found.
- **AOMiss:** Returns 1.0 (unoccluded).
- **GIMiss:** Returns the sky irradiance for the ray direction.

### Closest-Hit Shaders

- **DefaultClosestHit:** Evaluates the material at the hit point (albedo, normal, roughness, metallic) using bindless texture access. For GI bounces, traces secondary rays recursively up to the configured bounce limit.
- **AlphaMaskedClosestHit:** Same as default but performs alpha testing. If the alpha value is below the threshold, the ray is continued via `IgnoreHit()` (effectively acting as an any-hit rejection).

### Any-Hit Shaders

- **AlphaMaskedAnyHit:** Used for foliage and fences. Samples the alpha texture at the hit point and calls `IgnoreHit()` if below threshold. This allows rays to pass through transparent portions of alpha-tested geometry.

## Denoising Passes

Raw ray-traced output at low SPP is noisy. SparkEngine applies a multi-pass denoising pipeline:

1. **Temporal Accumulation:** Reproject the previous frame's denoised result using motion vectors. Blend with the current noisy result using an exponential moving average (alpha=0.1). Reject history samples where motion vectors indicate disocclusion.
2. **Spatial Filter (A-Trous Wavelet):** A 5-iteration edge-aware spatial filter that progressively increases its kernel radius (1, 2, 4, 8, 16 pixels). The filter weights are guided by depth, normal, and luminance edges to preserve geometric detail.
3. **Firefly Suppression:** Clamp outlier pixels that exceed 3x the local neighbourhood median. Applied before spatial filtering to prevent fireflies from spreading.

```cpp
struct DenoiserSettings
{
    float temporalAlpha        = 0.1f;   // Blend factor for temporal accumulation
    int   spatialIterations    = 5;      // A-Trous filter iterations
    float depthSensitivity     = 1.0f;   // Edge-stopping on depth
    float normalSensitivity    = 128.0f; // Edge-stopping on normals
    float luminanceSensitivity = 4.0f;   // Edge-stopping on luminance
    bool  fireflySuppress      = true;
};
```

## Integration with the Lighting System

DXR ray tracing integrates with the existing deferred lighting pipeline:

- **Reflections** replace or supplement screen-space reflections (SSR). The system blends between SSR and RT reflections based on the confidence of the screen-space result and available GPU budget.
- **Shadows** replace shadow maps for the primary directional light and optionally for point/spot lights. RT shadows provide correct contact-hardening soft shadows without cascaded shadow map artefacts.
- **Ambient Occlusion** replaces or blends with SSAO/HBAO. RT AO captures large-scale occlusion that screen-space methods miss.
- **Global Illumination** provides single- or multi-bounce indirect lighting, replacing baked lightmaps for dynamic scenes.

```cpp
// In the lighting pass
if (dxr.IsFeatureEnabled(RTFeature::Reflections))
    reflectionSRV = dxr.GetReflectionOutput();
else
    reflectionSRV = ssrSystem.GetOutput();

if (dxr.IsFeatureEnabled(RTFeature::Shadows))
    shadowMask = dxr.GetShadowOutput();
else
    shadowMask = shadowMapSystem.GetShadowMask();
```

## Hybrid Rendering (Raster + RT)

SparkEngine uses a hybrid approach where rasterisation handles primary visibility (G-buffer) and ray tracing handles secondary effects. This is more efficient than full path tracing and allows graceful degradation:

| Effect | DXR Available | DXR Unavailable |
|--------|--------------|-----------------|
| Primary visibility | Rasterised G-buffer | Rasterised G-buffer |
| Reflections | RT reflections | Screen-space reflections |
| Shadows | RT soft shadows | Cascaded shadow maps |
| Ambient Occlusion | RT AO | HBAO+ |
| Global Illumination | RT GI | Baked lightmaps + light probes |

The fallback is automatic: `DXRManager::Initialize()` queries the DXR tier. If DXR is unsupported or the user disables it, all rendering paths seamlessly revert to screen-space and raster-based alternatives.

## Fallback to Screen-Space Effects

When DXR is unavailable (no D3D12 support, DXR Tier 0, or user preference), the engine automatically switches to screen-space alternatives:

```cpp
// Automatic fallback in RenderSystem
if (!dxr.IsAvailable() || !dxr.IsEnabled())
{
    ssrSystem.Execute(gBuffer, depthBuffer);
    ssaoSystem.Execute(gBuffer, depthBuffer);
    shadowMapSystem.RenderCascades(lightDir);
}
else
{
    dxr.TraceReflections(viewProj, cameraPos);
    dxr.TraceShadows(lightDir);
    dxr.TraceAmbientOcclusion(viewProj, cameraPos);
}
```

No game code changes are needed; the render system handles the selection internally based on `DXRManager` state.

## Memory Management for Acceleration Structures

Acceleration structures can consume significant GPU memory. SparkEngine manages this through:

- **Memory Pool:** A dedicated GPU memory allocator for BLAS and TLAS buffers, using placed resources in a large heap to reduce allocation overhead.
- **Compaction:** After initial BLAS builds, the engine queries the post-build compacted size and copies the BLAS to a tighter buffer. This typically saves 40-60% memory.
- **Budget Tracking:** `DXRStats::accelerationStructureMemory` tracks total AS memory. The system warns when memory exceeds a configurable budget (default: 512MB).
- **LOD-Based BLAS Selection:** Only the current LOD mesh is included in the BLAS. When an object's LOD changes, the old BLAS is released and a new one is built.
- **Streaming:** Distant objects can be excluded from the TLAS entirely using the instance mask, reducing both memory and traversal cost.

```cpp
auto stats = dxr.GetStats();
stats.accelerationStructureMemory; // Total AS memory in bytes
stats.blasCount;                    // Number of active BLAS
stats.tlasInstanceCount;           // Instances in current TLAS
stats.compactionSavingsBytes;      // Memory saved by compaction
```

## ECS Integration

DXR integrates with the ECS through dedicated components:

```cpp
// MeshRTComponent — attached to entities that should appear in ray tracing
struct MeshRTComponent
{
    uint32_t blasHandle     = 0;        // Handle from DXRManager::CreateBLAS()
    uint32_t hitGroupIndex  = 0;        // Index into the hit group shader table
    uint8_t  rayMask        = 0xFF;     // Instance mask for selective tracing
    bool     castsShadows   = true;     // Include in shadow rays
    bool     receivesGI     = true;     // Include in GI calculations
    bool     visible        = true;     // Include in TLAS this frame
};

// RTSettingsComponent — attached to the camera entity
struct RTSettingsComponent
{
    uint32_t enabledFeatures = static_cast<uint32_t>(RTFeature::All);
    RTQualityPreset quality  = RTQualityPreset::High;
};
```

The `DXRSystem` (an ECS system) runs after the `RenderSystem` culling pass. It iterates entities with `MeshRTComponent` and `Transform` to build the TLAS instance list, then dispatches ray tracing workloads based on the active camera's `RTSettingsComponent`.

## Editor UI Panel

The **DXR Raytracing** panel is available in SparkEditor under **Window > Rendering > DXR Raytracing**:

- **DXR Status:** Shows the detected DXR tier, GPU name, driver version, and supported features.
- **Feature Toggles:** Individual checkboxes for Reflections, Shadows, AO, and GI with per-feature enable/disable.
- **Quality Preset Selector:** Dropdown for Low/Medium/High/Ultra presets, with detailed SPP and bounce settings shown below.
- **Denoiser Controls:** Sliders for temporal alpha, spatial iterations, and edge-stopping sensitivities.
- **Memory Budget:** Bar graph showing current AS memory usage versus the configured budget.
- **GPU Timings:** Per-feature GPU timing bars (reflections, shadows, AO, GI, denoising, TLAS build).
- **Debug Visualisation:** Overlay modes to display raw noisy output (pre-denoise), denoised output, heat map of ray traversal steps, and acceleration structure wireframes.

```cpp
// Editor panel registration
EditorPanelRegistry::Register<DXRSettingsPanel>("DXR Raytracing",
    EditorPanelCategory::Rendering);
```

## Performance Tips

- Enable only the RT features you need. RT reflections + RT shadows is a good starting point; add AO and GI only if the GPU budget allows.
- Use the lowest quality preset that looks acceptable. The visual difference between Medium and High is often subtle, but the performance cost doubles.
- TLAS rebuild is cheap; prefer rebuilding over refitting the TLAS since the instance count typically fits in cache.
- For BLAS, prefer refit for animated meshes and full rebuild for newly spawned geometry.
- Shadow rays with `ACCEPT_FIRST_HIT` are 2-5x faster than closest-hit rays. Always use this flag for shadow and AO rays.
- Compaction saves significant memory but requires an extra copy. Schedule compaction during loading screens or when the scene is idle.
- On GPUs with limited RT hardware (e.g., GTX 1060 with software DXR fallback), consider disabling RT entirely and using screen-space effects.

## Console Commands

```
dxr.status          — Show DXR state
dxr.enable <feature> — Enable reflections/shadows/ao/gi
dxr.quality <preset> — Set low/medium/high/ultra
dxr.denoise <on|off> — Toggle denoising passes
dxr.debug <mode>     — Visualise raw/denoised/heatmap/bvh
dxr.budget <MB>      — Set AS memory budget
dxr.stats            — Print detailed GPU timing and memory stats
```

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Main rendering pipeline and deferred shading
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) -- How HLSL shaders are compiled and managed
- [Post-Processing](Post-Processing.md) -- Screen-space fallback effects (SSR, SSAO)
- [Upscaling System](Upscaling-System.md) -- DLSS/FSR upscaling that pairs with RT
- [Entity-Component-System](../subsystems/Entity-Component-System.md) -- ECS component reference
- [Physics](../subsystems/Physics.md) -- Mesh collision data shared with BLAS construction
