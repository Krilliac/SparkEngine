# Graphics & Rendering

Context: `#prompt:copilot-instructions` for project overview. Console commands: see `console-scripting` prompt.

## Architecture

`GraphicsEngine` (`SparkEngine/Source/Graphics/GraphicsEngine.h`) — full DX11 pipeline.

### Render Paths

```cpp
enum class RenderPath { Forward, Deferred, ForwardPlus, Clustered };
enum class QualityPreset { Low, Medium, High, Ultra, Custom };
enum class MSAALevel { None = 1, MSAA2x = 2, MSAA4x = 4, MSAA8x = 8 };
```

### Post-Processing (`TemporalEffects.h`)

Bloom, HDR tone mapping (Reinhard, ACES, Uncharted 2), SSAO, SSR, TAA, FXAA, volumetric lighting/fog. Orchestrated by `Spark::Graphics::PostProcessingPipeline`.

### Key Classes

| Class | File | Purpose |
|-------|------|---------|
| `GraphicsEngine` | `Graphics/GraphicsEngine.h` | D3D11 device, swap chain, render loop |
| `Shader` | `Graphics/Shader.h` | HLSL compilation, constant buffers |
| `RenderTarget` | `Graphics/` | Off-screen render targets |
| `MaterialSystem` | `Graphics/` | PBR materials (metallic/roughness) |
| `LightManager` | `Graphics/` | Point, directional, spot lights |
| `TextureSystem` | `Graphics/` | Texture loading, streaming |

### Constant Buffer Pattern

```cpp
struct PerFrameConstants {
    XMMATRIX viewProjection;
    XMFLOAT3 cameraPosition;
    float    time;
};
```

## Shader System

- `Shaders/HLSL/` — DX shader source; `Shaders/GLSL/` — OpenGL (experimental); `Shaders/Compiled/` — pre-compiled `.cso`
- PBR: Cook-Torrance BRDF, metallic/roughness, IBL, normal/parallax mapping, GPU particle compute shaders

### Adding a New Shader

1. Create `.hlsl` in `Shaders/HLSL/`
2. Define constant buffers matching C++ structs
3. Register with shader compilation system
4. Hot-reload: save file → engine detects and recompiles

## D3D11 Conventions

- DirectXMath (`XMFLOAT3`, `XMMATRIX`, `XMVector*`) for all math
- `ComPtr<T>` for all D3D objects, `HRESULT` checked with `FAILED()` — see coding standards in shared context
