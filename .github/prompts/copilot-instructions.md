# SparkEngine — Shared Project Context

## Identity

SparkEngine is a C++20 open-source 3D game engine targeting first-person shooters. It uses DirectX 11 for rendering, Bullet Physics 3 for simulation, XAudio2 for spatial audio, EnTT for ECS, AngelScript for scripting, and Dear ImGui for the editor. Primary platform is Windows 10+ (MSVC); Linux/macOS are experimental.

## Architecture

```
SparkEngine/         ← Executable host (like Unreal's runtime)
  Source/
    Core/            ← SparkEngine.h, EngineContext.h, IGameModule.h, Platform.h
    Graphics/        ← GraphicsEngine.h (DX11), Shader.h, TemporalEffects.h
    Audio/           ← AudioEngine.h (XAudio2), SoundEffect.h
    Physics/         ← PhysicsSystem.h (Bullet3), CollisionSystem.h, PhysicsTypes.h
    Input/           ← InputManager.h
    Camera/          ← SparkEngineCamera.h
    SceneManager/    ← Scene/level management
    Engine/
      ECS/           ← Components.h (umbrella), Components/{Core,Physics,Audio,Light,Animation,AI,Gameplay}Components.h
                       Systems/ECSystems.h (PhysicsUpdate, Animation, AI, Audio, Lifecycle, Render)
      AI/            ← AISystem.h, BehaviorTree.h, NavMesh.h, PerceptionSystem.h, SteeringBehaviors.h
      Animation/     ← AnimationSystem.h (skeletal, IK, state machines, blending)
      Scripting/     ← AngelScriptEngine.h (hot-reload scripting)
      Networking/    ← NetworkManager.h (UDP client/server — DISABLED in default build)
      Procedural/    ← Noise, erosion, mesh generation, WFC
      SaveSystem/    ← ECS serialization with miniz compression
      Cinematic/     ← Sequencer system
    Utils/           ← SparkConsole.h, Logger, Profiler, CrashHandler, Assert.h

SparkEditor/         ← ImGui-based editor (22 subsystems)
  Source/            ← Animation, AssetBrowser, BuildSystem, Gizmos, LevelStreaming,
                       MaterialEditor, Profiler, VersionControl, etc.

SparkGame/           ← Example game module (DLL loaded at runtime via IGameModule)
  Source/Game/       ← Player, weapons, HUD, terrain, inventory, quests
  Source/Projectiles/← Bullet, rocket, grenade with object pooling

SparkConsole/        ← External debug console app (named pipe communication)

Shaders/HLSL/        ← DirectX shaders (PBR, post-processing, compute)
Shaders/GLSL/        ← OpenGL shaders (experimental)
Tests/               ← 35 unit tests, CTest integration
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

- **C++20**: `constexpr`, `enum class`, structured bindings, `std::format`, concepts where useful
- **Ownership**: `std::unique_ptr` for owning, raw pointers for non-owning references. No `new`/`delete`.
- **RAII**: All resources (D3D11 objects via `ComPtr`, file handles, physics bodies) released in destructors
- **Const-correctness**: `const` on all non-mutating methods and parameters
- **Error handling**: `ASSERT` / `ASSERT_MSG` for dev; `LOG_TO_CONSOLE_IMMEDIATE` for runtime; `HRESULT` for D3D11
- **Naming**: PascalCase classes/methods, camelCase locals, m_ prefix members, UPPER_SNAKE macros
- **Headers**: `#pragma once`, forward-declare where possible, specific component headers over umbrella `Components.h`

## Thread Safety

- `Spark::SimpleConsole` — thread-safe (mutex-protected)
- `PhysicsSystem` — NOT thread-safe, main thread only
- `GraphicsEngine` — main thread render, `std::atomic` frame state
- Document thread guarantees in Doxygen for all public APIs

## Build

- CMake 3.16+, 30+ toggles (`ENABLE_EDITOR`, `ENABLE_GRAPHICS`, `ENABLE_PHYSX`, `ENABLE_AI`, `ENABLE_ANIMATION`, etc.)
- Zero warnings: `/W4` MSVC, `-Wall -Wextra` GCC/Clang
- Targets: SparkEngine (exe), SparkEditor (exe), SparkGame (DLL), SparkConsole (exe)
- CI: GitHub Actions — Windows MSVC, Linux GCC, Linux Clang (Debug + Release)

## NOT Yet Implemented

Do not describe these as working:
- **Networking** — `NetworkManager.h` disabled via `ENABLE_NETWORKING=OFF`
- **VR/AR, DXR ray tracing, DLSS/FSR** — No implementation
- **Mobile/Console** — Build targets defined but untested
