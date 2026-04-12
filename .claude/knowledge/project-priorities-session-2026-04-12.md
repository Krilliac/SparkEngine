# Project Priorities Session (2026-04-12)

**Type:** Observation
**Status:** Active
**Scope:** OpenGL rendering pipeline (5 engine fixes) + 106 integration tests across 8 critical systems

---

## Context

Session focused on two highest-impact priorities identified in a project
analysis: (1) enabling OpenGL rendering on Linux end-to-end, and (2) adding
integration tests for critical systems with zero orchestration coverage.
The engine had 5,305 tests before this session.

## What Was Done

### 1. OpenGL Rendering Pipeline (Commits 1, 5, 6)

Three layers of fixes to make the OpenGL backend render on Linux:

**Layer 1 — Infrastructure (Commit 1):**

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| No backend fallback | RHIBridge tried Vulkan, failed, went headless | Iterate all available backends |
| GLSwapChain headless-only | Linux always created FBO, never swapped | Detect windowed mode, use FBO 0 + SDL_GL_SwapWindow |
| Invalid glDrawBuffers on FBO 0 | GL_COLOR_ATTACHMENT0 invalid on default framebuffer | Use GL_BACK for FBO 0 |

**Layer 2 — Shader Source Retention (Commit 5):**

Shader::LoadVertexShader/LoadPixelShader compiled GLSL via the RHI but
discarded the result. Added `m_compiledVertexSource` / `m_compiledPixelSource`
members to store the compiled GLSL text after compilation. Accessors:
`GetCompiledVertexSource()` / `GetCompiledPixelSource()`.

**Layer 3 — Full Pipeline Wiring (Commit 6):**

| Component | Before | After |
|-----------|--------|-------|
| Shader::Initialize | No RHI device | Acquires IRHIDevice* from LinuxRHIState |
| SetShaders() | No-op | Binds RHI pipeline state + constant buffers |
| UpdatePerFrameConstants() | `(void)constants` — discarded | `device->UpdateBuffer(perFrameCB)` |
| UpdatePerObjectConstants() | `(void)constants` — discarded | `device->UpdateBuffer(perObjectCB)` |
| CreateConstantBuffers() | No-op | Creates 2 RHI dynamic CBs (binding 0, 1) |
| CreateRHIPipelineIfReady() | Did not exist | Lazily creates VS + PS + PSO from stored GLSL |
| Shutdown() | No RHI cleanup | Releases pipeline, shaders, CBs |

**Full pipeline now connected:**
```
GLSL file → LoadShader() → CompileShader() → store source
    → CreateRHIPipelineIfReady() → device->CreateShader(VS/PS)
    → device->CreatePipelineState() → SetShaders() binds PSO + CBs
    → UpdateConstants() writes UBOs → DrawIndexed() renders geometry
```

### 2. Integration Tests (Commits 2–4, 5)

Added 106 new tests across 8 test files:

| Test File | Tests | System | Key Coverage |
|-----------|-------|--------|-------------|
| TestRHIBridgeIntegration.cpp | 18 | RHIBridge | Lifecycle, fallback, headless frames, resources, shader cache |
| TestNetworkManagerIntegration.cpp | 14 | NetworkManager | Init/shutdown, state, server ops, console |
| TestSystemManagerIntegration.cpp | 11 | SystemManager | Execution order, enable/disable, lookup, world |
| TestAssetPipelineIntegration.cpp | 16 | AssetPipeline | Lifecycle on Linux, asset type detection, cache LRU |
| TestMaterialSystemIntegration.cpp | 14 | MaterialSystem | Material CRUD, PBR props, render state, PersistentCB |
| TestEngineLifecycle.cpp | 11 | GraphicsEngine | Full init→tick→shutdown via NullRHI, subsystems |
| TestFPSGameplayIntegration.cpp | 10 | GameMode (FPS) | Init, scoring, teams, spawn points, player lifecycle |
| TestGLSLPipelineIntegration.cpp | 12 | GLSL Pipeline | Compile, passthrough, shader creation, full draw pipeline, cross-compile |

### Test Count: 5,305 → 5,411 (+106)

## Key Findings

1. **OpenGL backend was fully implemented** (1,978 lines, 251 GL calls)
   but never connected to SDL2's window for presentation. The fix was
   ~130 lines of infrastructure code, not a new implementation.

2. **Shader class was a dead end on Linux** — compiled GLSL correctly
   via `RHI::CompileShader()` but discarded the result. SetShaders()
   was a no-op. Constant buffer updates were silently dropped. Three
   commits fixed the entire path.

3. **Linux AssetPipeline::Initialize accepts nullptr device** — no assert,
   just stores it. Makes the full pipeline testable on Linux CI.

4. **MaterialSystem::Initialize has SPARK_EXPECTS(device != nullptr)** —
   cannot be tested with nullptr. But Material objects, PBR validation,
   render state, and PersistentMaterialCBManager all work standalone.

5. **GLSL shaders are production-quality** — BasicVS.glsl (90 lines,
   proper vertex attributes, dual UBO blocks) and BasicPS.glsl (246
   lines, full PBR with GGX/Schlick) are complete and compilable.

6. **HLSL→GLSL cross-compilation works** for basic type translation
   (float4→vec4, mul→*, saturate→clamp, etc.) but complex shaders
   need a proper SPIRV-Cross pipeline.

## Files Modified

```
SparkEngine/Source/Graphics/RHI/RHIBridge.cpp          — Backend fallback
SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.cpp — Windowed mode
SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.h   — SDL window member
SparkEngine/Source/Graphics/Shader.h                    — RHI members, forward decls
SparkEngine/Source/Graphics/ShaderLinux.cpp              — RHI pipeline wiring
SparkEngine/Source/Graphics/ShaderCompilationLinux.cpp   — Store compiled source
Tests/TestRHIBridgeIntegration.cpp                      — NEW (18 tests)
Tests/TestNetworkManagerIntegration.cpp                  — NEW (14 tests)
Tests/TestSystemManagerIntegration.cpp                   — NEW (11 tests)
Tests/TestAssetPipelineIntegration.cpp                   — NEW (16 tests)
Tests/TestMaterialSystemIntegration.cpp                  — NEW (14 tests)
Tests/TestEngineLifecycle.cpp                            — NEW (11 tests)
Tests/TestFPSGameplayIntegration.cpp                     — NEW (10 tests)
Tests/TestGLSLPipelineIntegration.cpp                    — NEW (12 tests)
Tests/CMakeLists.txt                                     — Added 8 test files
```

## Remaining Priorities

1. ~~OpenGL backend + shader pipeline~~ ✓
2. Playable FPS test arena (needs real display for visual verification)
3. Terrain heightfield renderer (major feature)
4. ~~Critical-path integration tests~~ ✓ (7 systems + FPS GameMode)
5. Cross-platform audio validation (needs audio hardware)

## Next Session Recommendations

- **Visual verification**: Run the engine with SDL2 + Mesa llvmpipe to verify
  pixels actually appear. Use `Xvfb :99 -screen 0 1280x720x24 &` then
  `DISPLAY=:99 ./SparkEngine --test-frames 10` and capture a screenshot.
- **Shader loading test**: Write a test that loads BasicVS.glsl + BasicPS.glsl
  through the Shader class and verifies `GetCompiledVertexSource()` is non-empty.
- **Remaining untested**: Editor panels (3.7% coverage), game modules (13.7%).
