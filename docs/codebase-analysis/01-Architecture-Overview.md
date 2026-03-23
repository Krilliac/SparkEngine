# 01 — Architecture Overview

## Engine Architecture

SparkEngine follows a **modular, layered architecture** inspired by commercial engines like Unreal Engine (module loading), HeroEngine (area server design), CryEngine (dynamic response system), and Godot/Redot (path caching). The engine executable acts as a host that loads game modules (DLLs/shared libraries) at runtime.

### Layer Diagram

```
Layer 5: Game Modules      SparkGame.dll, SparkGameMMO.dll (user code)
Layer 4: Editor            SparkEditor (ImGui, 32+ panels, collaborative)
Layer 3: Engine Systems    ECS, AI, Animation, Audio, Networking, Scripting, etc.
Layer 2: Core Services     EngineContext, Bootstrap, ModuleManager, Settings
Layer 1: Infrastructure    Logger, Console, Profiler, Assert, Timer, Math, FileUtils
Layer 0: Platform          Platform.h (compiler/OS detection, DirectXMath stubs)
```

Each layer depends only on layers below it. Game modules access engine systems exclusively through `IEngineContext` (service locator pattern).

## Startup Sequence

The engine initializes subsystems in dependency order using `EngineBootstrap` (topological sort via Kahn's algorithm):

```
1. Platform initialization (Windows API, console codepage)
2. Logger setup (async writer thread, file sink with rotation)
3. Crash handler installation (SEH on Windows, signal handlers on Linux)
4. EngineContext creation (service locator)
5. EngineSettings load (INI file: graphics, audio, physics, AI, etc.)
6. Window creation + Graphics device (D3D11 primary, RHI bridge optional)
7. Input system (keyboard, mouse, gamepad)
8. Audio engine (XAudio2 on Windows, stubs on Linux)
9. Physics system (Jolt Physics world, allocators, job system)
10. Animation system (manager, evaluator, blend spaces)
11. ECS world (EnTT registry, system manager with 11 systems)
12. AI system (behavior tree templates, NavMesh manager)
13. Streaming system (SeamlessAreaManager, area definitions)
14. Networking (NetworkManager, transport layer)
15. Scripting engine (AngelScript VM, Lua VM optional)
16. Remaining systems (UI, dialogue, destruction, events, save, etc.)
17. ModuleManager: load game DLLs from manifest or directory scan
18. Module initialization: OnLoad(context) for each module
19. Console process: launch SparkConsole.exe subprocess
20. Main loop begins
```

Shutdown occurs in reverse order.

## Main Loop

```cpp
while (running)
{
    // Input
    inputManager->ProcessMessages();

    // Fixed timestep (physics)
    accumulator.Advance(deltaTime);
    for (int i = 0; i < accumulator.GetFixedStepCount(); ++i)
    {
        physicsSystem->Update(fixedDt);
        modules->FixedUpdateAll(fixedDt);
    }

    // Variable timestep systems (ECS execution order)
    // Physics sync → Animation → AI → Audio → Lifecycle → Render
    systemManager->UpdateAll(world, deltaTime);

    // Module updates
    modules->UpdateAll(deltaTime);

    // Rendering
    graphicsEngine->BeginFrame();
    graphicsEngine->RenderScene();
    modules->RenderAll();
    graphicsEngine->EndFrame();

    // Console IPC
    consoleProcessManager->ProcessCommands();
}
```

## Key Design Patterns

### 1. Service Locator (EngineContext)

All subsystems are registered in a central type-indexed registry. Game modules receive `IEngineContext*` and query systems by type:

```cpp
// Engine-side registration
context->RegisterSubsystem<PhysicsSystem>(physics);
context->RegisterSubsystem<AudioEngine>(audio);

// Game-module-side access
auto* physics = context->GetPhysics();
auto* audio = context->GetAudio();
auto* network = context->GetNetwork();
```

Named getters (`GetGraphics()`, `GetInput()`, etc.) wrap the generic `GetSystem<T>()` for convenience. 35+ subsystems are registered.

### 2. Dependency-Ordered Bootstrap (EngineBootstrap)

Subsystems declare dependencies by name. `EngineBootstrap` performs topological sort to determine safe initialization order:

```cpp
EngineBootstrap bootstrap;
bootstrap.Register({"Logger", InitLogger, ShutdownLogger, {}});
bootstrap.Register({"Graphics", InitGfx, ShutdownGfx, {"Logger"}});
bootstrap.Register({"Physics", InitPhys, ShutdownPhys, {"Logger", "Graphics"}});
bootstrap.Initialize();  // Logger → Graphics → Physics
bootstrap.Shutdown();     // Physics → Graphics → Logger
```

Failed dependencies cascade: if Logger fails, Graphics and Physics are skipped.

### 3. Module System (ModuleManager)

Game code lives in DLLs loaded at runtime. Two interfaces are supported:

```cpp
// New interface (Spark::IModule)
class MyGame : public Spark::IModule {
    void OnLoad(Spark::IEngineContext* ctx) override;
    void OnUpdate(float dt) override;
    void OnRender() override;
};
SPARK_IMPLEMENT_MODULE(MyGame)

// Legacy interface (IGameModule) — auto-wrapped via LegacyModuleAdapter
class OldGame : public IGameModule {
    bool Initialize(GraphicsEngine*, InputManager*) override;
    void Update(float dt) override;
};
```

Modules are discovered via `spark.modules.json` manifest or directory scan. Hot-reload is supported: `ReloadModule()` shuts down, unloads, reloads, and reinitializes without restarting the engine.

### 4. ECS (Entity-Component-System via EnTT)

Entities are lightweight IDs. Components are pure data structs. Systems contain all logic:

```cpp
// Component: pure data
struct Transform {
    XMFLOAT3 position{0, 0, 0};
    XMFLOAT3 rotation{0, 0, 0};
    XMFLOAT3 scale{1, 1, 1};
};

// System: logic operating on components
class RenderSystem : public ISystem {
    void Update(entt::registry& world, float dt) override {
        auto view = world.view<MeshRenderer, Transform>();
        for (auto entity : view) { /* submit draw call */ }
    }
};
```

**Execution order**: Physics → Animation → AI → Audio → Lifecycle → Render

### 5. Console as IPC (ConsoleProcessManager)

The engine and SparkConsole.exe are separate processes communicating via stdin/stdout pipes. The engine writes log messages to the child's stdin; the child writes commands to its stdout. A background reader thread prevents blocking.

### 6. RAII and Ownership

- `std::unique_ptr` for owning pointers; raw pointers for non-owning references
- `ComPtr<>` for D3D11 COM objects
- `ScopeGuard` (ScopeExit/ScopeSuccess/ScopeFail) for cleanup
- No naked `new`/`delete`

### 7. Console Variable System (CVar)

Typed CVars with automatic console command generation:

```cpp
static CVar<float> cv_gravity("physics.gravity", -9.81f, CVarFlags::Save);
// Auto-generates: "physics.gravity" query command and set command
```

## Module Boundaries

| Module | Type | Purpose |
|--------|------|---------|
| SparkEngineLib | Static library | All engine systems (reusable) |
| SparkEngine | Executable | Host that loads game modules |
| SparkGame | Shared library | Example FPS game |
| SparkGameMMO | Shared library | MMO game module |
| SparkEditor | Executable | ImGui-based visual editor |
| SparkConsole | Executable | External debug console |
| SparkShaderCompiler | Tool | Offline shader compilation |
| SparkSDK | Headers | Public API for game modules |
| Tests | Executable | 146 unit tests |

## Thread Safety Model

| System | Thread Safety | Notes |
|--------|--------------|-------|
| Logger | Thread-safe | Async writer thread, message queue |
| SimpleConsole | Thread-safe | Mutex-protected public methods |
| ConsoleProcessManager | Thread-safe | Background reader thread, queues |
| NetworkManager | Thread-safe | Queue mutex, socket ops on dedicated thread |
| GraphicsEngine | Main thread + atomic | Draw list mutex, atomic frame state |
| PhysicsSystem | Main thread | Jolt internal multithreading for simulation |
| InputManager | Thread-safe | Mutex + atomic counters |
| EventBus | Thread-safe | Per-type mutexes for publishing |
| ECS Systems | Main thread only | All system updates on main thread |
| Audio | Main thread | XAudio2 callbacks on internal thread |

## Platform Support

| Platform | Compiler | Status | Notes |
|----------|----------|--------|-------|
| Windows 10+ | MSVC v143 (VS 2022) | Primary | Full feature support |
| Windows 10+ | MSVC v144 (VS 2026) | Experimental | CI tested, continue-on-error |
| Linux | GCC 13+ | Supported | CI tested, SDL2 windowing |
| Linux | Clang 17+ | Supported | CI tested |
| macOS | Apple Clang | Experimental | Metal backend (optional) |

## Configuration

All engine settings centralized in `EngineSettings` (INI format):

- **Graphics**: Resolution, fullscreen, MSAA, shadows, HDR, render scale
- **Audio**: Master/SFX/music/voice volumes
- **Physics**: Gravity, timestep, friction, restitution
- **Rendering**: Pipeline (Forward/Deferred/Forward+/Clustered), LOD, culling
- **PostProcess**: Bloom, tonemapping, SSAO, SSR, TAA, motion blur
- **AI**: Detection ranges, speeds, accuracy, reaction times
- **Player**: Health, armor, speed, jump height
- **GameMode**: Score limits, respawn, friendly fire
