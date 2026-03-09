# Engine Core & ECS

Context: `#prompt:copilot-instructions` for project overview. Console commands: see `console-scripting` prompt.

## ECS Architecture (EnTT)

Entity = `uint32_t` ID. Components = plain data structs. Systems = logic iterating component groups.

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
class MySystem {
public:
    void Update(World& world, float dt) {
        world.GetRegistry().view<Transform, MyComponent>().each(
            [&](auto entity, Transform& t, MyComponent& c) { /* process */ });
    }
};
sysManager.AddSystem<MySystem>(); // Register in correct execution order
```

### Adding a New Component

```cpp
struct MyComponent {
    float value = 0.0f;
    bool active = true;
};
// Register serialization in SaveSystem if persistence needed
```

## Engine Lifecycle

`SparkEngine.cpp` main:
1. `EngineContext` created → subsystems init: Graphics → Input → Audio → Physics → Console
2. Game module loaded via `GameModuleLoader` → `CreateGameModule()`
3. Main loop: Input → Update(dt) → Render → Console.Update()
4. Shutdown: Game module → subsystems (reverse order)

## Key Source Files

| File | Purpose |
|------|---------|
| `Core/SparkEngine.h` | Main header, global pointers (deprecated — use EngineContext) |
| `Core/EngineContext.h` | Service locator |
| `Core/IGameModule.h` | DLL interface: `Initialize`, `Update`, `Render`, `Shutdown` |
| `Core/Platform.h` | `SPARK_PLATFORM_WINDOWS`, `SPARK_PLATFORM_LINUX` |
| `Utils/Assert.h` | `ASSERT`, `ASSERT_MSG`, `ASSERT_ALWAYS_MSG` |
