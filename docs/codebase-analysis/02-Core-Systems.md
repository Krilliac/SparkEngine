# 02 — Core Systems

**Location:** `SparkEngine/Source/Core/`

The Core layer provides platform abstraction, the service locator, dependency-ordered initialization, game module loading, engine settings, and fixed timestep management.

---

## Platform.h — Cross-Platform Abstraction

**File:** `SparkEngine/Source/Core/Platform.h`

Single point of entry for platform detection and C++ feature availability. Every source file includes this header.

### Compiler Detection
```cpp
#define SPARK_COMPILER_MSVC    // MSVC detected
#define SPARK_COMPILER_GCC     // GCC detected
#define SPARK_COMPILER_CLANG   // Clang detected
#define SPARK_COMPILER_APPLE_CLANG  // Apple Clang detected
```

### C++ Standard Detection
```cpp
#define SPARK_CPP26  // C++26 or later
#define SPARK_CPP23  // C++23 or later
#define SPARK_CPP20  // C++20 or later
```

### Library Feature Flags
```cpp
// C++23 features
#define SPARK_HAS_EXPECTED      // std::expected available
#define SPARK_HAS_PRINT         // std::print available
#define SPARK_HAS_STACKTRACE    // std::stacktrace available
#define SPARK_HAS_MDSPAN        // std::mdspan available
#define SPARK_HAS_GENERATOR     // std::generator available

// C++26 forward-compat
#define SPARK_HAS_CONTRACTS     // Contracts support
#define SPARK_HAS_REFLECTION    // Static reflection
#define SPARK_HAS_PACK_INDEXING // Pack indexing
```

### Platform Flags
```cpp
#define SPARK_PLATFORM_WINDOWS  // Windows OS
#define SPARK_PLATFORM_LINUX    // Linux OS
#define SPARK_PLATFORM_MACOS    // macOS
```

On Windows, native DirectX headers are included. On non-Windows platforms, stub headers provide DirectXMath type compatibility.

---

## EngineContext — Service Locator

**Files:** `SparkEngine/Source/Core/EngineContext.h`, `EngineContext.cpp`

Central registry where all subsystems are registered and accessed by type. This is the single source of truth for system references.

### Architecture

Generic type-indexed registry (`TypeId → void*`) with 30+ named accessors:

```cpp
template <typename T>
T* GetSystem() const
{
    auto it = m_systems.find(GetTypeId<T>());
    return it != m_systems.end() ? static_cast<T*>(it->second) : nullptr;
}
```

### Named Accessors (subset)

| Method | Returns |
|--------|---------|
| `GetGraphics()` | `GraphicsEngine*` |
| `GetInput()` | `InputManager*` |
| `GetPhysics()` | `PhysicsSystem*` |
| `GetAudio()` | `AudioEngine*` |
| `GetAnimation()` | `AnimationManager*` |
| `GetAI()` | `AISystem*` |
| `GetNetwork()` | `NetworkManager*` |
| `GetCamera()` | `SparkEngineCamera*` |
| `GetUI()` | `UISystem*` |
| `GetSaveSystem()` | `SaveSystem*` |
| `GetScriptEngine()` | `AngelScriptEngine*` |
| `GetVR()` | `VRSystem*` |

### Dependency-Aware Registration

```cpp
context->RegisterSubsystem<AudioEngine>(
    audio,
    DependsOn<GraphicsEngine, Logger>{},
    []() { audio->Initialize(); },    // init callback
    []() { audio->Shutdown(); }        // shutdown callback
);

context->InitializeAll();   // topological sort → ordered init
context->ShutdownAll();     // reverse order teardown
```

### Singleton Access

```cpp
EngineContext* ctx = EngineContext::Get();        // non-owning pointer
EngineContext::SetOwned(std::make_unique<EngineContext>());  // transfer ownership
EngineContext::ResetOwned();                       // destroy
```

---

## EngineBootstrap — Dependency-Ordered Initialization

**Files:** `SparkEngine/Source/Core/EngineBootstrap.h`, `EngineBootstrap.cpp`

Descriptor-based subsystem initialization using topological sort.

### SubsystemDescriptor

```cpp
struct SubsystemDescriptor {
    std::string name;
    std::function<bool()> initCallback;
    std::function<void()> shutdownCallback;
    std::vector<std::string> dependencies;  // names of required subsystems
};
```

### Usage

```cpp
EngineBootstrap bootstrap;
bootstrap.Register({"Logger", InitLogger, ShutdownLogger, {}});
bootstrap.Register({"Graphics", InitGfx, ShutdownGfx, {"Logger"}});
bootstrap.Register({"Physics", InitPhys, ShutdownPhys, {"Logger"}});
bootstrap.Register({"AI", InitAI, ShutdownAI, {"Logger", "Physics"}});

if (bootstrap.Initialize())  // topological sort → Logger, Graphics, Physics, AI
    RunEngine();

bootstrap.Shutdown();  // AI → Physics → Graphics → Logger
```

### States
`Registered → Initialized → Failed → ShutDown`

If a subsystem's dependency fails or is missing, it cascades to `Failed` and is skipped.

---

## ModuleManager — Dynamic Game Module Loader

**Files:** `SparkEngine/Source/Core/ModuleManager.h`, `ModuleManager.cpp`

Loads game DLLs at runtime, supporting both the new `Spark::IModule` and legacy `IGameModule` interfaces.

### Module Discovery

1. **Manifest file** (`spark.modules.json`) — preferred
2. **Directory scan** — fallback, searches for DLL/SO files
3. **Individual path** — manual loading

### Lifecycle

```cpp
ModuleManager modules;

// Discovery
modules.LoadModulesFromManifest();       // JSON manifest
// or
modules.LoadModule("SparkGame.dll");     // individual

// Runtime
modules.InitializeAll(context);          // OnLoad(context) for each
modules.UpdateAll(deltaTime);            // OnUpdate(dt) for each
modules.RenderAll();                     // OnRender() for each
modules.ResizeAll(width, height);        // OnResize(w, h) for each

// Hot-reload
modules.ReloadModule("SparkGame", context);  // shutdown → unload → reload → init

// Shutdown
modules.ShutdownAll();                   // reverse order
```

### Module Interface Support

| Export Function | Interface | Detection |
|----------------|-----------|-----------|
| `CreateModule()` / `DestroyModule()` | `Spark::IModule` | New modules |
| `CreateGameModule()` / `DestroyGameModule()` | `IGameModule` | Legacy (wrapped via `LegacyModuleAdapter`) |

---

## EngineSettings — Centralized Configuration

**Files:** `SparkEngine/Source/Core/EngineSettings.h`, `EngineSettings.cpp`

All engine settings in one INI file. No scattered parsers.

### Settings Groups

| Group | Key Settings |
|-------|-------------|
| Graphics | Resolution, fullscreen, MSAA (1/2/4/8), shadow quality, HDR, render scale |
| Audio | Master/SFX/music/voice volumes, mute-on-focus-loss |
| Physics | Gravity, timestep, friction, restitution, damping |
| Rendering | Pipeline (Forward/Deferred/ForwardPlus/Clustered), LOD, culling |
| PostProcess | Bloom, tonemapping, color grading, SSAO, SSR, TAA, motion blur |
| AI | Detection/attack ranges, move speed, accuracy, reaction time |
| Player | Health, armor, speed, jump height, sprint multiplier |
| GameMode | Score/round limits, respawn delay, friendly fire, headshot bonus |

### Usage

```cpp
auto& settings = EngineSettings::GetInstance();
settings.Load("engine.ini");           // load or create defaults

float gravity = settings.GetValue("Physics", "Gravity");
settings.SetValue("Graphics", "MSAA", "4");
settings.Save();

settings.ResetToDefaults();
```

### Console Integration

Settings are readable/writable via console commands. Changes trigger `OnSettingsChanged()` callbacks for live updates.

---

## FixedTimestepAccumulator — Deterministic Physics

**File:** `SparkEngine/Source/Core/FixedTimestepAccumulator.h`

Ensures physics runs at a fixed rate (default 60 Hz) regardless of frame rate.

```cpp
auto& acc = FixedTimestepAccumulator::GetInstance();
acc.Initialize(1.0f / 60.0f);  // 60 Hz fixed step

// Each frame:
acc.Advance(frameDeltaTime);
for (int i = 0; i < acc.GetFixedStepCount(); ++i)
{
    physicsSystem->Update(acc.GetFixedDeltaTime());
}
float alpha = acc.GetInterpolationAlpha();  // for render interpolation
```

Safety: Clamped to max 0.25s per frame and max 8 steps to prevent spiral-of-death.

---

## PluginRegistry — Static Plugin Linking

**File:** `SparkEngine/Source/Core/PluginRegistry.h`

Plugins self-register at static initialization time via macro:

```cpp
static bool MyPluginInit() { /* ... */ return true; }
static void MyPluginUpdate(float dt) { /* ... */ }
static void MyPluginShutdown() { /* ... */ }
SPARK_DECLARE_PLUGIN(MyPlugin, "My Plugin", MyPluginInit, MyPluginUpdate, MyPluginShutdown);
```

The registry walks a singly-linked list of descriptors:

```cpp
PluginRegistry::InitializeAll();   // call all init functions
PluginRegistry::UpdateAll(dt);     // per-frame updates
PluginRegistry::ShutdownAll();     // reverse-order teardown
```

No manual wiring required — plugins register themselves.

---

## EngineConsoleCommands — Centralized Command Registration

**File:** `SparkEngine/Source/Core/EngineConsoleCommands.h`

Single function that registers all engine-level console commands:

```cpp
void RegisterEngineConsoleCommands(
    ModuleManager* modules,
    AudioEngine* audio,
    ModuleHotReloadManager* hotReload
);
```

Categories: physics, audio, save, modules, architecture commands. Extracted from SparkEngine.cpp to keep the entry point clean.

---

## SparkEngine.h/cpp — Executable Entry Point

**Files:** `SparkEngine/Source/Core/SparkEngine.h`, `SparkEngine.cpp`

The main executable that hosts the engine runtime:

1. Initializes all systems (see startup sequence in Architecture Overview)
2. Loads game modules via ModuleManager
3. Runs the main loop (input → fixed update → variable update → render)
4. Handles shutdown in reverse order

100+ headers included, indicating comprehensive subsystem coverage.
