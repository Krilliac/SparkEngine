# Engine Core & ECS

Context: `#prompt:copilot-instructions` for project overview.

## ECS Architecture (EnTT)

Entity = `uint32_t` ID. Components = plain data structs. Systems = logic that iterates component groups.

### Component Headers (`SparkEngine/Source/Engine/ECS/Components/`)

| Header | Components |
|--------|-----------|
| `CoreComponents.h` | `NameComponent`, `Transform`, `MeshRenderer`, `Camera`, `Script` |
| `PhysicsComponents.h` | `RigidBodyComponent`, `ColliderComponent` |
| `AudioComponents.h` | `AudioSourceComponent` |
| `LightComponents.h` | `LightComponent` |
| `AnimationComponents.h` | `AnimationController`, `ParticleEmitterComponent` |
| `AIComponents.h` | `AIComponent`, `NetworkIdentity` |
| `GameplayComponents.h` | `TagComponent`, `ActiveComponent`, `HealthComponent`, weather, inventory, quests |

Prefer specific includes over umbrella `Components.h` for faster compilation.

### System Execution Order (`ECSystems.h`)

```
1. PhysicsUpdateSystem  — Runs Bullet simulation, writes Transform
2. AnimationUpdateSystem — Evaluates skeletal animation, produces bone matrices
3. AIUpdateSystem        — Reads Transform, runs behavior trees, writes velocity
4. AudioUpdateSystem     — Reads Transform, updates 3D source positions
5. LifecycleSystem       — Processes health, death, active/inactive
6. RenderSystem          — Reads Transform + MeshRenderer, submits draw calls
```

Systems communicate through shared components, never by calling each other.

### Adding a New System

```cpp
// 1. Create system class in Engine/ECS/Systems/
class MySystem {
public:
    void Update(World& world, float dt) {
        world.GetRegistry().view<Transform, MyComponent>().each(
            [&](auto entity, Transform& t, MyComponent& c) {
                // process
            });
    }
};

// 2. Register in SystemManager (in correct execution order)
sysManager.AddSystem<MySystem>();
```

### Adding a New Component

```cpp
// 1. Add data struct to appropriate Components/ header
struct MyComponent {
    float value = 0.0f;
    bool active = true;
};

// 2. Register serialization in SaveSystem if persistence is needed
```

## Engine Lifecycle

`SparkEngine.cpp` main:
1. `EngineContext` created with subsystem pointers
2. Subsystems initialized: Graphics → Input → Audio → Physics → Console
3. Game module loaded via `GameModuleLoader` → `CreateGameModule()`
4. Main loop: Input → Update(dt) → Render → Console.Update()
5. Shutdown: Game module → subsystems (reverse order)

## Key Source Files

| File | Purpose |
|------|---------|
| `Core/SparkEngine.h` | Main header, global pointers (deprecated — use EngineContext) |
| `Core/EngineContext.h` | Service locator: `GetGraphics()`, `GetAudio()`, `GetPhysics()`, etc. |
| `Core/IGameModule.h` | DLL interface: `Initialize`, `Update`, `Render`, `Shutdown` |
| `Core/Platform.h` | Platform detection macros: `SPARK_PLATFORM_WINDOWS`, `SPARK_PLATFORM_LINUX` |
| `Utils/Assert.h` | `ASSERT`, `ASSERT_MSG`, `ASSERT_ALWAYS_MSG` macros |

## Console Commands

| Command | Description |
|---------|-------------|
| `engine_status` | Show engine state and subsystem status |
| `entity_count` | Number of active entities |
| `component_list` | List registered component types |
| `memory_info` | Memory usage statistics |
| `thread_status` | Thread utilization info |
| `performance_profile` | Start/stop performance profiling |
