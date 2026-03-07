# Shader Pipeline

SparkEngine supports HLSL and GLSL shader authoring with cross-compilation through the [[Rendering and Graphics|RHI pipeline]]. The `SparkShaderCompiler` tool handles offline compilation.

**Source:** `SparkEngine/Source/Graphics/Shader.h`, `SparkShaderCompiler/src/`

## Shader Languages

| Language | Backend | File Extension |
|----------|---------|---------------|
| HLSL | DirectX 11 | `.hlsl` |
| GLSL | OpenGL | `.glsl` |
| SPIR-V | Vulkan (cross-compiled from HLSL) | `.spv` |

## Directory Structure

```
Shaders/
├── HLSL/           # DirectX shader source files
├── GLSL/           # OpenGL shader source files
└── Compiled/       # Pre-compiled DirectX bytecode (.cso)
```

## Shader Stages

| Stage | Description |
|-------|-------------|
| `vertex` | Vertex shader — transforms vertices |
| `pixel` | Pixel/fragment shader — computes pixel color |
| `geometry` | Geometry shader — processes primitives |
| `hull` | Hull shader — tessellation control |
| `domain` | Domain shader — tessellation evaluation |
| `compute` | Compute shader — general-purpose GPU compute |

## Constant Buffers

Shaders use two standard constant buffers defined in `Shader.h`:

### Per-Frame Constants (b0)

Updated once per frame:
- View matrix
- Projection matrix
- Camera position
- Light data
- Time

### Per-Object Constants (b1)

Updated for each rendered object:
- World matrix
- Material properties
- Object-specific parameters

## SparkShaderCompiler

The standalone offline shader compilation tool:

### Basic Usage

```bash
# Compile HLSL to DirectX 11 bytecode
SparkShaderCompiler BasicVS.hlsl -stage vertex -backend d3d11 -o BasicVS.cso

# Cross-compile HLSL to SPIR-V for Vulkan
SparkShaderCompiler PBR.hlsl -stage pixel -backend vulkan -o PBR.spv

# Compile GLSL for OpenGL
SparkShaderCompiler Water.glsl -stage vertex -backend opengl -o Water.glsl.spv
```

### Options

| Flag | Description |
|------|-------------|
| `-stage <type>` | Shader stage (vertex/pixel/geometry/hull/domain/compute) |
| `-backend <type>` | Target backend (d3d11/vulkan/opengl/auto) |
| `-entry <name>` | Entry point function name |
| `-o <path>` | Output file path |
| `-D <define>` | Preprocessor define |
| `-I <path>` | Include search path |
| `-O` | Enable optimization |
| `-Od` | Disable optimization (debug) |
| `-Zi` | Include debug information |
| `-validate` | Validate without writing output |
| `-reflect` | Print shader reflection data |
| `-v` | Verbose output |

### Batch Compilation

Use the provided scripts to compile all shaders:

```bash
# Windows
.\Shaders\compile_shaders.bat

# Linux
./Shaders/compile_shaders.sh
```

## Embedded Shaders

The engine supports embedded shaders compiled directly into the executable, eliminating external file dependencies for core shaders.

## RHI Cross-Compilation

The RHI layer handles shader format differences between backends:

```
HLSL Source
    │
    ├─── D3D11: Direct compilation → .cso bytecode
    ├─── Vulkan: HLSL → SPIR-V cross-compilation → .spv
    └─── OpenGL: HLSL → GLSL transpilation → .glsl
```

## Hot Reloading

During development, shader modifications can be detected and recompiled at runtime (when `ENABLE_HOT_RELOAD=ON`). See [[Asset Pipeline]] for hot-reload details.

---

## See Also

- [[Rendering and Graphics]] — Shader usage in the rendering pipeline
- [[Asset Pipeline]] — Shader asset management
- [[Build System and CMake Modules]] — Shader compilation integration
- [[SparkEditor]] — Shader editing and preview
