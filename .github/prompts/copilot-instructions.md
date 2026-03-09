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

## Key APIs

### EngineContext (service locator)
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

### Game Module Interface
```cpp
class IGameModule {
    virtual const char* GetGameName() const = 0;
    virtual bool Initialize(GraphicsEngine*, InputManager*) = 0;
    virtual void Update(float deltaTime) = 0;
    virtual void Render() {}
    virtual void Shutdown() = 0;
};
// DLL exports: CreateGameModule() and DestroyGameModule()
```

### Console Command Registration
```cpp
auto& console = Spark::SimpleConsole::GetInstance();
console.RegisterCommand("mycommand", [](const std::vector<std::string>& args) {
    return "Result string shown in console";
}, "Description of command", "Category");
console.Log("Message text", "INFO");  // Types: INFO, WARNING, ERROR, CRITICAL, TRACE, DEBUG, SUCCESS
```

### ECS Pattern
```cpp
// Components are pure data structs (CoreComponents.h, PhysicsComponents.h, etc.)
// Systems process components each frame via SystemManager
SystemManager sysManager;
sysManager.AddSystem<PhysicsUpdateSystem>(physicsSystem);
sysManager.AddSystem<AnimationUpdateSystem>();
sysManager.AddSystem<AIUpdateSystem>();
sysManager.AddSystem<AudioUpdateSystem>(audioEngine);
sysManager.AddSystem<LifecycleSystem>();
sysManager.AddSystem<RenderSystem>(graphicsEngine);
sysManager.UpdateAll(world, deltaTime);
```
System execution order matters: Physics → Animation → AI → Audio → Lifecycle → Render.

### Physics Body Creation
```cpp
PhysicsBodyDesc desc;
desc.type             = PhysicsBodyType::Dynamic;  // Static, Kinematic, Dynamic
desc.position         = {0, 5, 0};
desc.mass             = 10.0f;
desc.shape.type       = CollisionShapeType::Box;   // Box, Sphere, Capsule, Cylinder, Cone, Mesh, ConvexHull, Heightfield, Compound
desc.shape.dimensions = {1, 1, 1};
auto body = physics.CreateBody(desc);
```

## Coding Standards

- **C++20**: `constexpr`, `enum class`, structured bindings, `std::format`, concepts where useful
- **Ownership**: `std::unique_ptr` for owning, raw pointers for non-owning references. No `new`/`delete`.
- **RAII**: All resources (D3D11 objects via `ComPtr`, file handles, physics bodies) released in destructors
- **Const-correctness**: `const` on all non-mutating methods and parameters
- **Error handling**: `ASSERT` / `ASSERT_MSG` for dev validation; `LOG_TO_CONSOLE_IMMEDIATE` for runtime; return `HRESULT` for D3D11 calls
- **Naming**: PascalCase for classes/methods, camelCase for local variables, m_ prefix for members, UPPER_SNAKE for macros
- **Headers**: `#pragma once`, forward-declare where possible, include specific component headers over umbrella `Components.h`

## Thread Safety

- `Spark::SimpleConsole` is thread-safe (mutex-protected logging)
- `PhysicsSystem` is NOT thread-safe — call from main thread only
- `GraphicsEngine` renders on main thread; uses `std::atomic` for frame state
- Document thread guarantees in Doxygen comments for all public APIs

## Build

- CMake 3.16+, 30+ toggles (`ENABLE_EDITOR`, `ENABLE_GRAPHICS`, `ENABLE_PHYSX`, `ENABLE_AI`, `ENABLE_ANIMATION`, etc.)
- Zero warnings policy (`/W4` MSVC, `-Wall -Wextra` GCC/Clang)
- Targets: SparkEngine (exe), SparkEditor (exe), SparkGame (DLL), SparkConsole (exe)
- CI: GitHub Actions — Windows MSVC, Linux GCC, Linux Clang (Debug + Release matrix)

## NOT Yet Implemented

These features exist as code stubs or are disabled. Do not describe them as working:
- **Networking/Multiplayer** — `NetworkManager.h` exists but disabled via `ENABLE_NETWORKING=OFF` (avoids CURL dependency)
- **VR/AR** — No implementation
- **Ray tracing (DXR)** — No implementation
- **DLSS/FSR** — No implementation
- **Mobile/Console platforms** — Build targets defined but untested
