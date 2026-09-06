# Hybrid Ray Tracing

SparkEngine's hybrid ray tracing system provides Lumen-style multi-tier global illumination, reflections, and soft shadows across all hardware tiers — from GTX 1070 Ti to RTX 4090.

## Architecture

```
HybridRTManager (coordinator)
  ├── SDFSceneManager     (software SDFGI sphere tracing — CS 5.0+; not reachable on the Windows D3D11 path)
  ├── DXRManager          (hardware DXR 1.1 — DX12 Ultimate)
  ├── RTCompositor        (blends screen-space + RT results)
  └── ProbeSystem         (irradiance probe grid for cached GI)
```

### Backend Selection (automatic)

| GPU | Backend | Method |
|-----|---------|--------|
| RTX 20xx+ (DX12) | `HardwareDXR` | DXR 1.1 DispatchRays, TLAS/BLAS |
| RDNA2+ (Vulkan) | `HardwareVKRT` | VK_KHR_ray_tracing_pipeline |
| GTX 10xx / DX11 | *none on Windows* | `HybridRTManager` is only constructed behind an RHI bridge, and the Windows D3D11 path has no RHI bridge (`GraphicsEngine::GetRHIBridge()` is `nullptr` on Windows), so no hybrid RT -- including `Software_SDFGI` -- runs on the D3D11 path regardless of the `SPARK_HYBRID_RT` default. The software SDFGI backend is reachable only on the Linux RHI path. |
| Integrated | `Disabled` | Screen-space effects only |

## Quality Presets

| Preset | Resolution | Max Steps | Bounces | Probes | Denoising |
|--------|-----------|-----------|---------|--------|-----------|
| Low | 25% | 32 | 1 | No | No |
| Medium | 50% | 64 | 2 | Yes | No |
| High | 100% | 96 | 2 | Yes | Yes |
| Ultra | 100% | 128 | 3 | Yes | Yes |

## Console Commands

| Command | Description |
|---------|-------------|
| `rt.status` | Display current RT backend, quality, and statistics |
| `rt.quality <off\|low\|medium\|high\|ultra>` | Set quality preset |
| `rt.mode <auto\|sdfgi\|hardware\|off>` | Override RT backend |
| `rt.reflections <0\|1>` | Toggle RT reflections |
| `rt.gi <0\|1>` | Toggle RT global illumination |
| `rt.shadows <0\|1>` | Toggle RT soft shadows |

## SDFGI Software Path

The software fallback represents the scene as SDF primitives (spheres, boxes, capsules) approximating mesh bounding volumes. A compute shader sphere-traces through this representation for:

- **Reflections**: Trace reflected rays from GBuffer surfaces
- **Global Illumination**: Cosine-weighted hemisphere sampling for indirect light
- **Soft Shadows**: Sphere trace toward light with penumbra estimation

### Performance (Software SDFGI)

| GPU | Low | Medium | High |
|-----|-----|--------|------|
| GTX 1070 Ti | ~2ms | ~5ms | ~12ms |
| RTX 3060 | ~1ms | ~3ms | ~6ms |

## Build Configuration

```cmake
# Enable hybrid RT (default ON)
cmake -B build -DENABLE_HYBRID_RT=ON

# Enable hardware DXR path (requires DX12)
cmake -B build -DENABLE_HYBRID_RT=ON -DENABLE_DXR=ON
```

## File Structure

```
SparkEngine/Source/Graphics/HybridRT/
  ├── HybridRTTypes.h       — Shared types (SDFPrimitive, trace params)
  ├── HybridRTManager.h/cpp — Central coordinator
  ├── SDFSceneManager.h/cpp — SDF scene & compute dispatch
  ├── RTCompositor.h/cpp    — SS + RT result compositing
  └── ProbeSystem.h/cpp     — Irradiance probe grid

Shaders/HLSL/RayTracing/
  ├── SDFCommon.hlsl        — SDF distance functions
  ├── SDFTrace.hlsl         — SDFGI sphere tracing (CS 5.0)
  ├── SDFGenerate.hlsl      — Mesh-to-SDF generation
  ├── RTComposite.hlsl      — Result compositing
  ├── ProbeUpdate.hlsl      — Probe SH coefficient update
  ├── ProbeInterpolate.hlsl — Probe grid interpolation
  ├── DXRReflections.hlsl   — DXR reflection ray gen/hit/miss
  ├── DXRShadows.hlsl       — DXR soft shadow rays
  └── DXRAO.hlsl            — DXR ambient occlusion
```
