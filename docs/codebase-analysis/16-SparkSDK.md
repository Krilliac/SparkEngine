# 16 — SparkSDK

**Location:** `SparkSDK/Include/Spark/`

Public interface headers for game modules. Game code links against these headers and communicates with the engine exclusively through `IEngineContext`.

---

## Header Overview

| Header | Purpose |
|--------|---------|
| `SparkSDK.h` | Master include (aggregates all SDK headers) |
| `IEngineContext.h` | Service locator (26+ subsystem getters) |
| `IModule.h` | Module lifecycle interface |
| `ILogger.h` | Abstract logging surface |
| `Version.h` | Version constants and compatibility checks |
| `SparkExport.h` | DLL export/import macros |
| `ModuleRegistry.h` | Module implementation macros |
| `MathTypes.h` | Math structures (Vec3, Quat, Mat4x4, AABB, Ray) |
| `InputTypes.h` | Input enums (MouseButton, GamepadButton, InputAction) |
| `EventTypes.h` | Event type forward declarations |

---

## IEngineContext — Service Locator

**File:** `SparkSDK/Include/Spark/IEngineContext.h`

Game modules receive this at initialization and use it to access all engine systems:

```cpp
class IEngineContext {
public:
    // Core (always available)
    virtual GraphicsEngine* GetGraphics() = 0;
    virtual InputManager* GetInput() = 0;
    virtual Timer* GetTimer() = 0;
    virtual EventBus* GetEventBus() = 0;

    // Optional (return nullptr if disabled)
    virtual AudioEngine* GetAudio() = 0;
    virtual PhysicsSystem* GetPhysics() = 0;
    virtual AnimationManager* GetAnimation() = 0;
    virtual AISystem* GetAI() = 0;
    virtual NetworkManager* GetNetwork() = 0;
    virtual SaveSystem* GetSaveSystem() = 0;
    virtual UISystem* GetUI() = 0;
    virtual VRSystem* GetVR() = 0;
    virtual SparkEngineCamera* GetCamera() = 0;
    // ... 20+ more subsystem getters

    // State queries
    virtual bool IsHeadless() const = 0;
    virtual uint32_t GetEngineVersion() const = 0;  // 0xMMmmpp
    virtual uint32_t GetSDKVersion() const = 0;     // ABI version
    virtual float GetElapsedTime() const = 0;
    virtual uint64_t GetFrameNumber() const = 0;

    // Lifecycle
    virtual void InitializeAll() = 0;
    virtual void ShutdownAll() = 0;
};
```

---

## IModule — Module Lifecycle

**File:** `SparkSDK/Include/Spark/IModule.h`

```cpp
struct ModuleInfo {
    const char* name;
    const char* version;
    uint32_t sdkVersion;
    int32_t loadOrder;
    const char** dependencies;
    uint32_t dependencyCount;
};

class IModule {
public:
    virtual ModuleInfo GetModuleInfo() const = 0;
    virtual bool OnLoad(IEngineContext* context) = 0;
    virtual void OnUnload() = 0;
    virtual void OnUpdate(float deltaTime) = 0;
    virtual void OnFixedUpdate(float fixedDeltaTime) = 0;
    virtual void OnRender() = 0;
    virtual void OnResize(int width, int height) = 0;
    virtual void OnPause() = 0;
    virtual void OnResume() = 0;
    virtual void OnImGui() = 0;  // Debug UI
};
```

### Creating a Module

```cpp
// MyGame.h
#include <Spark/SparkSDK.h>

class MyGame : public Spark::IModule {
    Spark::IEngineContext* m_context = nullptr;

public:
    Spark::ModuleInfo GetModuleInfo() const override {
        return {"My Game", "1.0.0", SPARK_SDK_VERSION, 0, nullptr, 0};
    }

    bool OnLoad(Spark::IEngineContext* ctx) override {
        m_context = ctx;
        auto* physics = ctx->GetPhysics();
        auto* audio = ctx->GetAudio();
        // Initialize game systems...
        return true;
    }

    void OnUpdate(float dt) override {
        // Game logic
    }

    void OnRender() override {
        // Custom rendering
    }

    void OnUnload() override {
        // Cleanup
    }
};

SPARK_IMPLEMENT_MODULE(MyGame)
```

### Module Dependencies

```cpp
Spark::ModuleInfo MyGame::GetModuleInfo() const {
    static const char* deps[] = {"BaseModule", "UIModule"};
    SPARK_MODULE_DEPENDENCIES(info, "BaseModule", "UIModule");
    return {"My Game", "1.0.0", SPARK_SDK_VERSION, 1, info.dependencies, info.dependencyCount};
}
```

---

## Version & Compatibility

**File:** `SparkSDK/Include/Spark/Version.h`

```cpp
constexpr uint32_t SPARK_ENGINE_VERSION_MAJOR = 1;
constexpr uint32_t SPARK_ENGINE_VERSION_MINOR = 0;
constexpr uint32_t SPARK_ENGINE_VERSION_PATCH = 0;
constexpr uint32_t SPARK_SDK_VERSION = 2;  // ABI version

bool Spark::IsSDKCompatible(uint32_t moduleSDKVersion) {
    return moduleSDKVersion == SPARK_SDK_VERSION;
}
```

---

## Math Types

**File:** `SparkSDK/Include/Spark/MathTypes.h`

Layout-compatible with DirectXMath:

```cpp
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };
struct Quat { float x, y, z, w; };  // Identity: {0,0,0,1}
struct Color { float r, g, b, a; };
struct Mat4x4 { float m[4][4]; };    // Row-major
struct AABB { Vec3 min, max; };
struct Ray { Vec3 origin, direction; };
```

---

## Export Macros

**File:** `SparkSDK/Include/Spark/SparkExport.h`

```cpp
// In module DLL (define SPARK_MODULE_DLL before including)
#define SPARK_MODULE_DLL
#include <Spark/SparkExport.h>

// SPARK_MODULE_API resolves to __declspec(dllexport) on Windows
SPARK_MODULE_API Spark::IModule* CreateModule();
SPARK_MODULE_API void DestroyModule(Spark::IModule* module);
```

---

## Legacy Support

The `IGameModule` interface (older API) is still supported. `ModuleManager` detects legacy exports and wraps them via `LegacyModuleAdapter`:

```cpp
// Legacy (still works)
class OldGame : public IGameModule {
    bool Initialize(GraphicsEngine* gfx, InputManager* input) override;
    void Update(float dt) override;
    void Render() override;
};
```
