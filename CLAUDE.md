# SparkEngine — Claude Code Context

## What is this?

SparkEngine is a C++20 open-source 3D game engine targeting first-person shooters.
- **Rendering**: DirectX 11 (Windows), OpenGL stubs (Linux/macOS)
- **Physics**: Bullet Physics 3
- **Audio**: XAudio2
- **ECS**: EnTT
- **Scripting**: AngelScript
- **Editor**: Dear ImGui
- **Primary platform**: Windows 10+ (MSVC); Linux/macOS are experimental

## Build

```bash
# Generate (pick one)
cmake --preset windows-release   # Windows MSVC
cmake --preset linux-release     # Linux GCC

# Build
cmake --build build --config Release

# Test
cd build && ctest --output-on-failure
```

CMake 3.16+, C++20 required. Key toggles: `ENABLE_EDITOR`, `ENABLE_GRAPHICS`, `ENABLE_PHYSX`, `ENABLE_AI`, `ENABLE_ANIMATION`, `ENABLE_NETWORKING` (OFF by default).

## Coding Standards

- **C++20**: `constexpr`, `enum class`, structured bindings, `std::format`, concepts
- **Ownership**: `std::unique_ptr` owning, raw pointers non-owning. No naked `new`/`delete`
- **RAII**: D3D11 via `ComPtr`, all resources released in destructors
- **Const-correctness**: `const` on all non-mutating methods and parameters
- **Naming**: PascalCase classes/methods, camelCase locals, `m_` prefix members, `UPPER_SNAKE` macros
- **Headers**: `#pragma once`, forward-declare where possible
- **Style**: Allman braces, 4-space indent, 120-col limit (see `.clang-format`)
- **Zero warnings**: `/W4` on MSVC, `-Wall -Wextra` on GCC/Clang

## Architecture (key directories)

```
SparkEngine/Source/Core/        — Platform.h, EngineContext.h
SparkEngine/Source/Graphics/    — GraphicsEngine (DX11), Shader, PostProcessing
SparkEngine/Source/Engine/ECS/  — Components.h, Systems/ECSystems.h
SparkEngine/Source/Engine/AI/   — AISystem, BehaviorTree, NavMesh
SparkEngine/Source/Engine/Animation/ — Skeletal animation, IK, state machines
SparkEngine/Source/Engine/Networking/ — NetworkManager (disabled by default)
SparkEngine/Source/Utils/       — Console, Logger, Profiler, Assert
SparkEditor/Source/             — ImGui editor (22 subsystems)
SparkGame/Source/               — Example FPS game module (DLL)
Tests/                          — 35+ unit tests, CTest
```

## ECS execution order

Physics → Animation → AI → Audio → Lifecycle → Render

## Thread safety rules

- `SimpleConsole` — thread-safe (mutex)
- `PhysicsSystem` — main thread only
- `GraphicsEngine` — main thread render, `std::atomic` frame state
- `NetworkManager` — queue mutex for message I/O and handler registration

## Things to know

- Use `EngineContext` service locator, not deprecated `g_graphics`/`g_input` globals
- Cross-platform types live in `Core/Platform.h` (DirectXMath stubs on Linux)
- Networking is disabled in default builds (`ENABLE_NETWORKING=OFF`)
- VR/AR, DXR, DLSS/FSR are not implemented
