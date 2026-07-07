---
name: sparkengine-shader-compiler-internals
description: >-
  Architecture and internals of SparkShaderCompiler, SparkEngine's in-house
  offline shader-compilation CLI tool (SparkShaderCompiler/src/main.cpp).
  Covers what input languages it accepts (HLSL and GLSL, SPIR-V passthrough),
  its CLI flags, the RHI CompileShader pipeline stages, output artifacts
  (.cso / .spv / .glsl) and where they land, filename-based stage inference,
  and how to add a new backend/stage or extend the tool. TRIGGER when the user
  asks "how does the shader compiler tool work", "how do I run
  SparkShaderCompiler", "what flags does the shader compiler take", "how do I
  batch-compile shaders", "where do compiled shaders go", "how do I add a new
  shader stage/backend to the compiler", or "why is my .cso just HLSL text".
  DO NOT TRIGGER for runtime shader debugging (why a shader renders wrong,
  pixel/HLSL debugging) — use the global shader-debug skill instead.
trilobite_compatible: true
trilobite_role: reference
---

# SparkShaderCompiler internals

`SparkShaderCompiler` is SparkEngine's **standalone offline shader-compilation
CLI tool**. It is a thin command-line front-end over the engine's RHI (Render
Hardware Interface — the multi-API graphics abstraction) cross-compilation
functions. This skill documents the *tool's own architecture*: its inputs,
CLI, pipeline stages, output format, and how to extend it.

Everything here is verified against source as of **2026-07-07**. See
`## Provenance and maintenance` for re-verification commands.

## When to use this skill / when NOT to

| Use this skill for | Use something else |
|---|---|
| "How do I invoke `SparkShaderCompiler`?" | Runtime "my shader looks wrong" -> global `shader-debug` skill |
| "What input languages / flags does it accept?" | Pixel/HLSL step-through debugging -> `shader-debug` |
| "Where do compiled artifacts go?" | The engine's *runtime* shader loader (`Shader.h`) -> read that header directly |
| "How do I add a stage/backend to the tool?" | The **Slang** compiler interface (`SlangShaderInterface.h`) — a *separate*, parallel system NOT used by this tool |

**Critical scope note:** this tool does **not** use `SlangShaderInterface.h`.
Slang is a separate, target-agnostic compiler interface in the engine. The CLI
tool routes exclusively through `Spark::RHI::CompileShader` (see below).

## The two load-bearing files

| File | Role |
|---|---|
| `SparkShaderCompiler/src/main.cpp` | The entire CLI tool: arg parsing, stage inference, single + batch compile loops. ~578 lines, one translation unit. |
| `SparkEngine/Source/Graphics/RHI/RHIFactory.{h,cpp}` | Defines `ShaderCompileOptions`, `ShaderCompileResult`, `CompileShader()`, `SaveCompiledShader()`, `ReflectSPIRV()`, `GetShaderExtension()`. **This is where the real work is delegated.** |

The tool builds *only* `main.cpp` plus a hand-picked list of RHI `.cpp` files
(see `SparkShaderCompiler/CMakeLists.txt`) — it does not link the whole engine.

## CLI reference (verified against `main.cpp` PrintUsage + ParseArgs)

```
SparkShaderCompiler <input> [options]
```

| Flag | Meaning | Default |
|---|---|---|
| `-o <path>` | Output file path (in `-batch` mode, an output **directory**) | inferred from input name + backend |
| `-stage <stage>` | Shader stage (see table below) | inferred from filename |
| `-backend <api>` | `d3d11` / `d3d12` / `vulkan` / `opengl` / `auto` | `auto` |
| `-entry <name>` | Entry-point function name | `main` |
| `-D<DEFINE>` | Add a preprocessor define (no space: `-DFOO=1`) | — |
| `-I<path>` | Add an include search path (no space: `-IShaders/inc`) | — |
| `-O` | Enable optimization | on |
| `-Od` | Disable optimization | — |
| `-Zi` | Enable debug info | off |
| `-validate` | Compile but do **not** write output | off |
| `-reflect` | Print shader reflection data (see caveat) | off |
| `-batch <dir>` | Recursively compile every shader file under `<dir>` | — |
| `-v` | Verbose (prints stage/backend/entry/timing) | off |
| `-h`, `--help` | Print usage and exit | — |

Backend aliases accepted by `ParseBackend`: `dx11`=`d3d11`, `dx12`=`d3d12`,
`vk`=`vulkan`, `gl`=`opengl`. Unknown backend -> warns, falls back to `auto`.

### Stage names (verified in `ParseStage`)

Each stage has aliases. Unknown stage -> warns, defaults to `vertex`.

| Canonical | Aliases |
|---|---|
| `vertex` | `vert`, `vs` |
| `pixel` | `frag`, `ps` |
| `geometry` | `geom`, `gs` |
| `hull` | `hs` |
| `domain` | `ds` |
| `compute` | `cs` |
| `raygen` | `rgen`, `raygeneration` |
| `closesthit` | `rchit`, `chs` |
| `miss` | `rmiss` |
| `anyhit` | `rahit` |
| `intersection` | `rint` |
| `callable` | `rcall` |

### Examples (from the tool's own `--help`, paths verified to exist)

```bash
# Single file, explicit stage + backend, explicit output
SparkShaderCompiler Shaders/HLSL/BasicVS.hlsl -stage vertex -backend d3d11 -o BasicVS.cso

# Pixel shader to a Vulkan target
SparkShaderCompiler Shaders/HLSL/BasicPS.hlsl -stage pixel -backend vulkan -o BasicPS.spv

# Validate only (no output written)
SparkShaderCompiler Shaders/HLSL/BasicVS.hlsl -backend opengl -validate

# Batch-compile a whole directory for one backend
SparkShaderCompiler -batch Shaders/HLSL -backend vulkan
```

Real sample shaders live in `Shaders/HLSL/` (e.g. `BasicVS.hlsl`,
`BasicPS.hlsl`, `Compute/GPUCull.hlsl`).

## Pipeline: what actually happens on `CompileSingleShader`

1. **Arg parse** (`ParseArgs`) builds a `CompilerConfig`.
2. **Stage inference** (`InferStageFromFilename`) if `-stage` was not given —
   see the inference rules below.
3. Config is copied into a `Spark::RHI::ShaderCompileOptions` struct.
4. `Spark::RHI::CompileShader(options)` does the real work
   (`RHIFactory.cpp`):
   - **Source language** is auto-detected from extension: `.hlsl`/`.fx` -> HLSL,
     `.glsl`/`.vert`/`.frag` -> GLSL, `.spv` -> SPIR-V. Default HLSL.
   - **Target language** is derived from `-backend`: D3D11/D3D12 -> HLSL,
     Vulkan -> SPIR-V, OpenGL -> GLSL. Unknown backend -> passthrough
     (target = source).
   - The source file is read from disk (binary).
   - It then dispatches on the (source -> target) pair (see the truth table).
5. On success, unless `-validate`, the bytecode is written with
   `Spark::RHI::SaveCompiledShader` (a raw binary `ofstream` write).
6. Output path, if `-o` omitted, comes from `InferOutputPath`.

### Output artifact naming (`InferOutputPath`)

| Backend | Extension |
|---|---|
| Vulkan | `.spv` |
| D3D11 / D3D12 | `.cso` |
| OpenGL / other | `GetShaderExtension(backend)` -> `.glsl` (D3D fallback `.hlsl`) |

The output is written to **wherever `-o` points, or next to the input file**
(same directory, extension swapped). The tool has **no built-in default asset
directory** — it writes relative to CWD / the input path. (The `bin/Shaders`
directory you may see under `build/` is populated by the CMake build, not by
this tool.)

### Filename-based stage inference (`InferStageFromFilename`)

Used when `-stage` is omitted, and always in `-batch` mode. Non-obvious
conventions worth knowing:

- The **bare stem** (no dir, no extension) is examined, lowercased.
- Distinctive full words match **anywhere** in the stem: `vert`, `frag`,
  `pixel`, `geom`, `hull`, `domain`, `compute`, `raygen`, `closesthit`, etc.
- The **two-letter codes** (`vs`/`ps`/`gs`/`hs`/`ds`/`cs`) are deliberately
  restrictive to avoid false hits (e.g. "Physics" contains "cs"). They match
  **only** as an uppercase suffix in the original name (`BasicPS`) or as a
  whole `_`/`-` delimited final token (`blur_cs`).
- No match -> defaults to **Vertex**.

So `BasicPS.hlsl` -> Pixel, `blur_cs.hlsl` -> Compute, but `Physics.hlsl` -> Vertex.

## Truth table — what compiles vs. what is a stub (VERIFY-DO-NOT-OVERSELL)

The RHI layer these functions live in is **partly a shim**. Do not assume the
tool produces real GPU bytecode for every backend. Verified in
`RHIFactory.cpp`:

| Source -> Target | Behaviour | Real? |
|---|---|---|
| HLSL -> HLSL (D3D11/D3D12 target) | **Passthrough**: source text copied verbatim into the output bytes | NO — the `.cso` is literally the HLSL **text**, not compiled DXBC/DXIL |
| GLSL -> GLSL (OpenGL, matching) | Passthrough: source copied verbatim | (text copy) |
| HLSL -> GLSL (OpenGL target) | Naive keyword substitution (`float4`->`vec4`, `lerp`->`mix`, `SV_Position`->`gl_Position`, ...) | Partial — simple shaders only; `mul()`/`clip()` left as comment markers |
| HLSL -> SPIR-V (Vulkan target) | `CrossCompileHLSLtoSPIRV` logs "DXC not integrated" and returns **empty** -> compile **fails** | NO — DXC (`dxcompiler.dll`) not linked |
| GLSL -> SPIR-V (Vulkan target) | `CompileGLSLtoSPIRV` logs "glslang not integrated" and returns **empty** -> compile **fails** | NO — glslang not linked |

Consequences you must communicate to users:

- **`-backend vulkan` currently fails** for HLSL/GLSL inputs (empty bytecode)
  until DXC / glslang are integrated at build time.
- A `.cso` produced by this tool for a D3D backend is **HLSL source text**, not
  a compiled D3D blob. Anything downstream expecting real DXBC must not assume
  this tool produced it.
- `-reflect` runs `ReflectSPIRV`, which only checks the SPIR-V magic number
  (`0x07230203`) and then logs "SPIRV-Reflect not integrated" and returns
  **empty** reflection. So `-reflect` prints `Inputs: 0 / Resources: 0`
  regardless of shader content.

These are labelled in-source as intentional integration points ("Link
dxcompiler.dll to enable" / "Link glslang to enable"), i.e. **candidate**
future work — not bugs to file.

## Batch mode specifics (`main`)

- Recurses `-batch <dir>` with `recursive_directory_iterator`.
- Recognised extensions: `.hlsl .glsl .vert .frag .comp .geom .tesc .tese .vs
  .ps .gs .cs .rgen .rmiss .rchit .rahit .rint .rcall` (case-insensitive).
- Each file's stage is re-inferred from its filename (ignores any `-stage`).
- With `-o <dir>`, that directory is `create_directories`'d up front and each
  output lands inside it (input basename, extension swapped).
- Prints a `=== Batch Compilation Summary ===` with total/success/failed/time;
  process exit code is `1` if any file failed, else `0`.

Exit codes: single-file success `0`, failure `1`; bad args -> `1`.

## Building the tool

Target name is `SparkShaderCompiler`; it builds as part of the normal CMake
configure (its `CMakeLists.txt` is picked up by the top-level build).

```bash
# Configure (pick your preset — see CLAUDE.md build section)
cmake --preset windows-release      # or linux-gcc-release, etc.

# Build just this tool
cmake --build build --config Release --target SparkShaderCompiler
```

Output binary lands in `${CMAKE_BINARY_DIR}/bin` (set via
`RUNTIME_OUTPUT_DIRECTORY`; IDE folder "Tools").

**What the tool links** (from `CMakeLists.txt`) — it does NOT link the whole
engine, only:
`RHIFactory.cpp`, `DebugHookManager.cpp`, `Logger.cpp`, `WineDetection.cpp`,
plus backend device `.cpp`s guarded by platform/availability:
- Windows: `D3D11Device.cpp` always; `D3D12Device.cpp` + `D3D12CommandList.cpp`
  only when **not** MinGW (MinGW's `d3d12.h` is too old -> `SPARK_NO_D3D12`).
- `SPARK_VULKAN_AVAILABLE` -> Vulkan device sources.
- `SPARK_OPENGL_AVAILABLE` -> OpenGL device source.

Windows link libs: `d3d11 dxgi dxguid d3dcompiler dbghelp` (+ `d3d12 opengl32`
when not MinGW). `WineDetection.cpp` is a **hard** dependency because
`GetRecommendedBackend()` calls `IsRunningUnderGvisor()` to pick NullRHIDevice.

## How to extend the tool

Because the CLI is one file, most extensions are local edits to `main.cpp`.

**Add a new CLI stage alias:** edit `ParseStage` (add the string), and if it is
a new *engine* stage also add it to `RHIShaderStage` in
`SparkEngine/Source/Graphics/RHI/RHITypes.h`, plus `StageToString` and the
`InferStageFromFilename` rules in `main.cpp`.

**Add a new backend to the CLI:** edit `ParseBackend` (accept the string) and
`GraphicsBackend` in `RHITypes.h`. Then wire the real behaviour in
`RHIFactory.cpp`'s `CompileShader` target-language switch, `InferOutputPath`,
and `GetShaderExtension`. Add the backend's device `.cpp` to
`SparkShaderCompiler/CMakeLists.txt` under the appropriate `if()` guard.

**Enable real SPIR-V / DXBC output:** integrate DXC (`dxcompiler.dll`, via
`IDxcCompiler3`) into `CrossCompileHLSLtoSPIRV`, and glslang into
`CompileGLSLtoSPIRV`, then link the libs in the CLI's `CMakeLists.txt`. These
are the two documented integration seams. This is **candidate** work — do not
claim the tool emits real SPIR-V until those are wired.

**Add a new shader (asset, not tool code):** drop the `.hlsl`/`.glsl` under
`Shaders/` following the naming convention so stage inference works
(`*VS`/`*PS`/`*_cs`, or a distinctive word like `Compute`). Then compile via
the CLI, or rely on the engine's runtime shader path (`Shader.h`) — that runtime
path is out of scope here (see the `shader-debug` skill for runtime concerns).

## Related but distinct systems (do not confuse)

- `SparkEngine/Source/Graphics/ShaderCrossCompiler.h` — an **in-engine**,
  cache-backed cross-compile manager (`ShaderTarget` DXIL/DXBC/SPIRV/GLSL/MSL/
  WGSL). Its `CompileToDXBC/SPIRV/...` methods are currently stubs returning
  `success=true` with empty bytecode. The **CLI tool does not use this class** —
  it calls the free functions in `RHIFactory`. Header notes it "may be
  superseded by `SlangShaderInterface.h`" (open decision).
- `SparkEngine/Source/Graphics/SlangShaderInterface.h` — a separate
  target-agnostic **Slang** compiler interface (DXIL/SPIRV/MSL/HLSL/CUDA/WGSL).
  Also not used by the CLI tool.

## Provenance and maintenance

Verified 2026-07-07 against SparkEngine `Working` branch. Re-verify with:

```bash
# CLI flags, stage/backend parsers, batch logic
sed -n '69,255p' SparkShaderCompiler/src/main.cpp

# The real compile delegate + stub warnings (DXC/glslang not integrated)
grep -n 'not integrated\|passthrough\|GetShaderExtension\|SaveCompiledShader' \
  SparkEngine/Source/Graphics/RHI/RHIFactory.cpp

# What the tool links / build guards
sed -n '13,122p' SparkShaderCompiler/CMakeLists.txt

# Enum ground truth (stages, backends, languages)
grep -n 'enum class GraphicsBackend\|enum class RHIShaderStage\|enum class ShaderLanguage' \
  SparkEngine/Source/Graphics/RHI/RHITypes.h

# Confirm sample shaders still live here
ls Shaders/HLSL/BasicVS.hlsl Shaders/HLSL/BasicPS.hlsl
```

If DXC or glslang get integrated (the two `"not integrated"` warnings in
`RHIFactory.cpp` disappear), update the truth table above — the "currently
fails for Vulkan" and "`.cso` is text" caveats will no longer hold.

Lint this file with the global skill-linter (see
`~/.claude/skills/skill-linter/`), passing `-Path` pointing at this skill's
folder `.claude/skills/sparkengine-shader-compiler-internals`.
