# SparkEngine — Shared Project Context

## Identity

SparkEngine is a C++23 open-source 3D game engine. Originally built for first-person shooters, it is evolving into a general-purpose engine supporting FPS, RPG, MMO, open-world, and other genres. It uses DirectX 11 for rendering (with experimental D3D12/Vulkan/OpenGL/Metal backends via RHI abstraction), Jolt Physics for simulation, XAudio2 for spatial audio, EnTT for ECS, AngelScript for scripting, and Dear ImGui for the editor. Primary platform is Windows 10+ (MSVC); Linux/macOS are experimental.

## Architecture

```
SparkEngine/         ← Executable host (like Unreal's runtime)
  Source/
    Core/            ← SparkEngine.h, EngineContext.h, IGameModule.h, Platform.h
    Graphics/        ← GraphicsEngine.h (DX11), Shader.h, TemporalEffects.h
    Audio/           ← AudioEngine.h (XAudio2), SoundEffect.h
    Physics/         ← PhysicsSystem.h (Jolt Physics), CollisionSystem.h, PhysicsTypes.h
    Input/           ← InputManager.h
    Camera/          ← SparkEngineCamera.h
    SceneManager/    ← Scene/level management
    Engine/
      ECS/           ← Components.h (umbrella), Components/{Core,Physics,Audio,Light,Animation,AI,Gameplay}Components.h
                       Systems/ECSystems.h (PhysicsUpdate, Animation, AI, Audio, Lifecycle, Render)
      AI/            ← AISystem.h, BehaviorTree.h, NavMesh.h, PerceptionSystem.h, SteeringBehaviors.h
      Animation/     ← AnimationSystem.h (skeletal, IK, state machines, blending)
      Scripting/     ← AngelScriptEngine.h (hot-reload scripting)
      Networking/    ← NetworkManager.h (UDP client/server, area servers, replication)
      Procedural/    ← Noise, erosion, mesh generation, WFC
      SaveSystem/    ← ECS serialization with miniz compression
      Cinematic/     ← Sequencer system
    Utils/           ← SparkConsole.h, Logger, Profiler, CrashHandler, Assert.h

SparkEditor/         ← ImGui-based editor (59 panels)
  Source/            ← Animation, AssetBrowser, BuildSystem, Gizmos, LevelStreaming,
                       MaterialEditor, Profiler, VersionControl, etc.

SparkGame/           ← Example game module (DLL loaded at runtime via IGameModule)
  Source/Game/       ← Player, weapons, HUD, terrain, inventory, quests
  Source/Projectiles/← Bullet, rocket, grenade with object pooling

SparkConsole/        ← External debug console app (named pipe communication)

Shaders/HLSL/        ← DirectX shaders (PBR, post-processing, compute)
Shaders/GLSL/        ← OpenGL shaders (experimental)
Tests/               ← 5962 unit tests across 484 files, CTest integration
Templates/           ← Game module templates
Assets/              ← Demo scenes, models, scripts
```

## Key API: EngineContext (service locator)

```cpp
class EngineContext : public Spark::IEngineContext {
    GraphicsEngine* GetGraphics();
    InputManager*   GetInput();
    Timer*          GetTimer();
    AudioEngine*    GetAudio();
    PhysicsSystem*  GetPhysics();
    Spark::EventBus* GetEventBus();
    bool IsHeadless() const;
};
```
Use `EngineContext` — the old `g_graphics`/`g_input`/`g_timer` globals are `[[deprecated]]`.

ECS execution order: Physics → Animation → AI → Audio → Lifecycle → Render. See `engine-core` prompt for details.

## Coding Standards

- **C++23**: `constexpr`, `enum class`, structured bindings, `std::format`, `std::expected`, `std::print`, concepts, deducing `this`
- **Ownership**: `std::unique_ptr` for owning, raw pointers for non-owning references. No `new`/`delete`.
- **RAII**: All resources (D3D11 objects via `ComPtr`, file handles, physics bodies) released in destructors
- **Const-correctness**: `const` on all non-mutating methods and parameters
- **Error handling**: `ASSERT` / `ASSERT_MSG` for dev; `LOG_TO_CONSOLE_IMMEDIATE` for runtime; `HRESULT` for D3D11
- **Naming**: PascalCase classes/methods, camelCase locals, m_ prefix members, UPPER_SNAKE macros
- **Headers**: `#pragma once`, forward-declare where possible, specific component headers over umbrella `Components.h`

## Thread Safety

- `Spark::SimpleConsole` — thread-safe (mutex-protected)
- `PhysicsSystem` — Jolt Physics; supports multithreaded job dispatch
- `GraphicsEngine` — main thread render, `std::atomic` frame state
- Document thread guarantees in Doxygen for all public APIs

## Build

- CMake 3.25+, 30+ toggles (`ENABLE_EDITOR`, `ENABLE_GRAPHICS`, `ENABLE_PHYSX`, `ENABLE_AI`, `ENABLE_ANIMATION`, etc.)
- Zero warnings: `/W4` MSVC, `-Wall -Wextra` GCC/Clang
- Targets: SparkEngine (exe), SparkEditor (exe), SparkGame (DLL), SparkConsole (exe)
- CI: GitHub Actions — Windows MSVC, Linux GCC, Linux Clang (Debug + Release)

## Formatting

- **clang-format** is enforced by CI (`check-format` job). The `.clang-format` config lives at repo root.
- **Before committing**, always run `clang-format -i` on all modified `.cpp`/`.h` files.
- Quick check: `clang-format --dry-run --Werror <files>` — must produce zero warnings.

## Experimental Systems

These systems exist but are not yet production-ready:
- **VR** — OpenXR-ready framework stub; needs OpenXR SDK for actual hardware
- **DXR ray tracing** — Feature-complete, enabled by default with SDFGI software fallback
- **D3D12/Vulkan/OpenGL/Metal** — Experimental RHI backends via abstraction layer
- **Mobile/Console** — Build targets defined but untested
