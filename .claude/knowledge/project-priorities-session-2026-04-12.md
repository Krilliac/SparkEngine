# Project Priorities Session (2026-04-12)

**Type:** Observation
**Status:** Active
**Scope:** OpenGL rendering fix + 94 integration tests across 7 critical systems

---

## Context

Session focused on the two highest-impact priorities identified in a
project analysis: (1) enabling OpenGL rendering on Linux, and
(2) adding integration tests for critical systems with zero orchestration
coverage. The engine had 5,305 tests before this session.

## What Was Done

### 1. OpenGL Rendering on Linux (Commit 1)

Fixed three bugs preventing the OpenGL backend from rendering:

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| No backend fallback | RHIBridge tried Vulkan, failed, gave up | Iterate all available backends |
| GLSwapChain headless-only | Linux path always created FBO, never swapped | Detect windowed mode, use FBO 0 + SDL_GL_SwapWindow |
| Invalid glDrawBuffers on FBO 0 | GL_COLOR_ATTACHMENT0 invalid on default framebuffer | Use GL_BACK for FBO 0 |

**Files:** `RHIBridge.cpp`, `OpenGLDevice.cpp`, `OpenGLDevice.h`

### 2. Integration Tests (Commits 2–4)

Added 94 new tests across 7 test files for systems that previously had
zero orchestration coverage:

| Test File | Tests | System | Key Coverage |
|-----------|-------|--------|-------------|
| TestRHIBridgeIntegration.cpp | 18 | RHIBridge | Lifecycle, fallback, headless frames, resources, shader cache |
| TestNetworkManagerIntegration.cpp | 14 | NetworkManager | Init/shutdown, state, server ops, console |
| TestSystemManagerIntegration.cpp | 11 | SystemManager | Execution order, enable/disable, lookup, world |
| TestAssetPipelineIntegration.cpp | 16 | AssetPipeline | Lifecycle on Linux, asset type detection, cache LRU |
| TestMaterialSystemIntegration.cpp | 14 | MaterialSystem | Material CRUD, PBR props, render state, PersistentCB |
| TestEngineLifecycle.cpp | 11 | GraphicsEngine | Full init→tick→shutdown via NullRHI, subsystems |
| TestFPSGameplayIntegration.cpp | 10 | GameMode (FPS) | Init, scoring, teams, spawn points, player lifecycle |

### Test Count Progression

| Point | Tests |
|-------|-------|
| Session start | 5,305 |
| After commit 2 | 5,348 |
| After commit 3 | 5,389 |
| After commit 4 | 5,399 |

## Key Findings

1. **OpenGL backend was fully implemented** (1,978 lines, 251 GL calls)
   but never connected to SDL2's window for presentation. The fix was
   ~130 lines of code, not a new implementation.

2. **Linux AssetPipeline::Initialize accepts nullptr device** — no assert,
   just stores it. This makes the full pipeline testable on Linux CI.

3. **MaterialSystem::Initialize has SPARK_EXPECTS(device != nullptr)** —
   cannot be tested with nullptr. But Material objects, PBR validation,
   render state, and PersistentMaterialCBManager all work standalone.

4. **GameMode.cpp is already linked into the test binary** via CMakeLists.txt.
   WaveSpawner.cpp has too many dependencies (Game.h, Enemy.h) for
   standalone test linking — would need the full FPS module .so.

## Files Modified

```
SparkEngine/Source/Graphics/RHI/RHIBridge.cpp          — Backend fallback
SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.cpp — Windowed mode
SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.h   — SDL window member
Tests/TestRHIBridgeIntegration.cpp                      — NEW
Tests/TestNetworkManagerIntegration.cpp                  — NEW
Tests/TestSystemManagerIntegration.cpp                   — NEW
Tests/TestAssetPipelineIntegration.cpp                   — NEW
Tests/TestMaterialSystemIntegration.cpp                  — NEW
Tests/TestEngineLifecycle.cpp                            — NEW
Tests/TestFPSGameplayIntegration.cpp                     — NEW
Tests/CMakeLists.txt                                     — Added 7 test files
```

## Remaining Priorities (from session analysis)

1. ~~OpenGL backend~~ ✓
2. Playable FPS test arena (needs real display for visual verification)
3. Terrain heightfield renderer (major feature)
4. ~~Critical-path integration tests~~ ✓ (5 systems + GameMode)
5. Cross-platform audio validation (needs audio hardware)
