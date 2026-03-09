# Graphics & Rendering

Context: `#prompt:copilot-instructions` for project overview.

## Architecture

`GraphicsEngine` (`SparkEngine/Source/Graphics/GraphicsEngine.h`) manages the full DX11 pipeline. Uses `ComPtr<T>` for all D3D11 objects. Frame state tracked with `std::atomic`.

### Render Paths

```cpp
enum class RenderPath { Forward, Deferred, ForwardPlus, Clustered };
```

### Quality & AA

```cpp
enum class QualityPreset { Low, Medium, High, Ultra, Custom };
enum class MSAALevel { None = 1, MSAA2x = 2, MSAA4x = 4, MSAA8x = 8 };
```

### Post-Processing (`SSAOSettings`, `TAASettings` in `TemporalEffects.h`)

- Bloom, HDR tone mapping (Reinhard, ACES, Uncharted 2)
- SSAO (screen-space ambient occlusion)
- SSR (screen-space reflections)
- TAA (temporal anti-aliasing), FXAA
- Volumetric lighting, fog
- `Spark::Graphics::PostProcessingPipeline` orchestrates the chain

### Key Classes

| Class | File | Purpose |
|-------|------|---------|
| `GraphicsEngine` | `Graphics/GraphicsEngine.h` | D3D11 device, swap chain, render loop |
| `Shader` | `Graphics/Shader.h` | HLSL compilation, constant buffers |
| `RenderTarget` | `Graphics/` | Off-screen render targets |
| `MaterialSystem` | `Graphics/` | PBR materials (metallic/roughness) |
| `LightManager` | `Graphics/` | Point, directional, spot lights |
| `TextureSystem` | `Graphics/` | Texture loading, streaming |
| `AssetPipeline` | `Graphics/` | Asset loading coordination |

### Constant Buffer Pattern

```cpp
// PerFrameConstants and PerObjectConstants defined in Shader.h
struct PerFrameConstants {
    XMMATRIX viewProjection;
    XMFLOAT3 cameraPosition;
    float    time;
    // ...
};
```

## Shader System

### File Locations

| Directory | Contents |
|-----------|----------|
| `Shaders/HLSL/` | DirectX shader source (.hlsl) |
| `Shaders/GLSL/` | OpenGL shader source (experimental) |
| `Shaders/Compiled/` | Pre-compiled DirectX bytecode (.cso) |

### PBR Pipeline (Cook-Torrance BRDF)

- Metallic/roughness workflow
- IBL (Image-Based Lighting) via environment maps
- Normal mapping, parallax occlusion mapping
- Compute shaders for GPU particles

### Adding a New Shader

1. Create `.hlsl` in `Shaders/HLSL/`
2. Define constant buffers matching C++ structs
3. Register with shader compilation system
4. Hot-reload: modify and save — engine detects changes and recompiles

## D3D11 Conventions

- All D3D11 calls return `HRESULT` — check with `FAILED()` macro
- Device lost/reset handling implemented in `GraphicsEngine`
- Use `ComPtr<ID3D11Buffer>`, never raw `Release()` calls
- DirectXMath (`XMFLOAT3`, `XMMATRIX`, `XMVector*` functions) for all math

## Console Commands

| Command | Description |
|---------|-------------|
| `graphics_vsync` | Toggle VSync on/off |
| `graphics_wireframe` | Toggle wireframe rendering |
| `shader_reload` | Hot-reload all shaders |
| `shader_list` | List loaded shaders |
| `shader_compile_all` | Recompile all shaders |
| `render_debug` | Toggle debug render views |
| `graphics_stats` | Show FPS, draw calls, triangle count |
| `graphics_screenshot` | Capture screenshot |
