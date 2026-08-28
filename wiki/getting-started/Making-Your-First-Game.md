# Making Your First Game

This guide walks through representative source-development patterns for a game module -- from an empty module to gameplay, asset, and editor experiments. It is not an end-to-end packaging or production-game recipe.

> **Stable-v1 boundary:** Stable-v1 is blocked and uncertified. Its only intended product shape is Windows 11 x64 with MSVC v143, D3D11 (or Windows NullRHI), C++ game modules, and one first-party single-player `SparkGameFPS` vertical slice. Everything in this tutorial remains development guidance until the applicable installed-package, editor, and gameplay gates pass; other platforms, backends, scripting, collaboration, and multiplayer are experimental or unsupported.

> **Prerequisites:** Complete the [Getting Started](Getting-Started.md) guide first. You need a working build before proceeding.

## Step 1: Create a Game Module

SparkEngine loads one selected Game-kind module as a DLL/shared library. There are two distinct setup routes:

- **Installed template:** materialize or copy `Templates/EmptyProject` outside the engine checkout, install the engine SDK, and configure that package with `SparkEngine_DIR`. A stock template is a standalone installed-SDK project.
- **In-tree module:** create `GameModules/MyGame` with a CMake target that the repository root auto-discovers. Do not copy a stock `Templates/EmptyProject` package directly under `GameModules/`: its `find_package(SparkEngine CONFIG REQUIRED)` setup expects an installed SDK rather than the source-tree build.

For the in-tree route, start with this layout:

```
MyGame/
  CMakeLists.txt         # Build configuration (auto-discovered by root CMake)
  Source/
    GameModule.h         # IModule implementation
    GameModule.cpp       # DLL entry point + factory exports
```

`GameModules/MyGame/CMakeLists.txt` can use the helper already included by the root build:

```cmake
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS "Source/*.cpp" "Source/*.h")
spark_add_game_module(MyGame ${GAME_SOURCES})
target_include_directories(MyGame PRIVATE "Source")
```

### The Module Interface

Your module implements `Spark::IModule` from `<Spark/SparkSDK.h>`:

```cpp
#include <Spark/SparkSDK.h>

class MyGameModule : public Spark::IModule
{
public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        return {"My Game", "1.0.0", SPARK_SDK_VERSION, 1000};
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        // Called once when the DLL is loaded.
        // Store the context -- it's your gateway to all engine services.
        m_context = context;
        return true;
    }

    void OnUnload() override { /* cleanup */ }

    void OnUpdate(float deltaTime) override
    {
        // Called every frame. Put your game logic here.
    }

    void OnRender() override
    {
        // Called after OnUpdate. Custom rendering (HUD, debug) goes here.
    }

private:
    Spark::IEngineContext* m_context{nullptr};
};
```

Use the canonical module macro in exactly one `.cpp` file rather than hand-writing the exports:

```cpp
#include "GameModule.h"
#include <Spark/ModuleDllMain.h>

SPARK_IMPLEMENT_MODULE(MyGameModule)
```

### Build and Run

```powershell
# From the repository root, for the in-tree route above.
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target SparkEngine MyGame
.\build\bin\Release\SparkEngine.exe -game .\build\bin\Release\MyGame.dll
```

For Debug or another multi-config configuration, replace both `Release` path segments with the same configuration name. For the standalone-template route, use the installed-SDK commands in [Project Templates](../gameplay-tools/Project-Templates.md) instead.

## Step 2: Access Engine Services

The `IEngineContext` exposes service accessors provided by the active build; availability varies by configuration:

```cpp
bool MyGameModule::OnLoad(Spark::IEngineContext* context)
{
    m_context   = context;
    m_graphics  = context->GetGraphics();     // Rendering
    m_input     = context->GetInput();        // Keyboard, mouse, gamepad
    m_audio     = context->GetAudio();        // Sound playback
    m_physics   = context->GetPhysics();      // Jolt physics
    m_eventBus  = context->GetEventBus();     // Event system
    m_save      = context->GetSaveSystem();   // Save/load
    return true;
}
```

## Step 3: Set Up a Player

Create a first-person player with camera and movement:

```cpp
#include "Camera/SparkEngineCamera.h"
#include "Input/InputManager.h"

// In your game class:
void MyGame::Initialize()
{
    m_camera = std::make_unique<SparkEngineCamera>();
    m_camera->SetPosition({0.0f, 2.0f, -5.0f});
    m_camera->SetNearFarPlanes(0.1f, 1000.0f);
}

void MyGame::Update(float dt)
{
    auto* input = m_context->GetInput();

    // WASD movement
    float moveX = 0.0f, moveZ = 0.0f;
    if (input->IsKeyDown('W')) moveZ += 1.0f;
    if (input->IsKeyDown('S')) moveZ -= 1.0f;
    if (input->IsKeyDown('A')) moveX -= 1.0f;
    if (input->IsKeyDown('D')) moveX += 1.0f;

    float speed = 10.0f * dt;
    m_camera->MoveForward(moveZ * speed);
    m_camera->MoveRight(moveX * speed);

    // Mouse look
    auto [dx, dy] = input->GetMouseDelta();
    m_camera->RotateYaw(dx * 0.002f);
    m_camera->RotatePitch(dy * 0.002f);
}
```

For a source reference containing an FPS controller with jumping, sprinting, crouching, health, and weapons, inspect `GameModules/SparkGameFPS/Source/Game/Player.h`.

## Step 4: Add Objects to the Scene

### Procedural Primitives

```cpp
#include "Graphics/Mesh.h"

auto cube = std::make_unique<Mesh>();
cube->CreateCube(device, context);
cube->SetPosition({5.0f, 0.5f, 0.0f});

auto sphere = std::make_unique<Mesh>();
sphere->CreateSphere(device, context);
sphere->SetPosition({-5.0f, 1.0f, 0.0f});
```

### Load 3D Models (OBJ / glTF)

```cpp
auto model = std::make_unique<Mesh>();
model->LoadOBJ(device, context, "Assets/Models/crate.obj");    // Wavefront OBJ
model->LoadGLTF(device, context, "Assets/Models/robot.glb");   // glTF binary
```

## Step 5: Add AI Enemies

SparkEngine has a full AI system with behavior trees, perception, and pathfinding. The easiest way is using the `Enemy` class:

```cpp
#include "Game/Enemy.h"

// Spawn an enemy
auto enemy = std::make_unique<Enemy>();
enemy->Initialize(device, context, EnemyType::Grunt, m_player.get());
enemy->SetPosition({15.0f, 1.0f, 15.0f});

// Give it patrol waypoints
enemy->SetPatrolPoints({{15, 1, 15}, {15, 1, -5}, {-5, 1, -5}});
```

### Enemy Types

| Type | HP | Speed | Behavior |
|------|-----|-------|----------|
| `Grunt` | 100 | 5.0 | Patrols and attacks at medium range |
| `Guard` | 150 | 3.0 | Stationary sentry with long detection range |
| `Scout` | 60 | 8.0 | Fast flanker, strafes during combat |
| `Heavy` | 300 | 2.5 | Slow tank, high damage, short range |
| `Sniper` | 70 | 3.5 | Long-range, retreats if player gets close |
| `Medic` | 120 | 5.5 | Heals nearby allies, moderate combat |

### Custom Behavior Trees

For advanced AI, build your own behavior tree:

```cpp
#include "Engine/AI/BehaviorTree.h"

using namespace Spark::AI;

auto root = std::make_unique<SelectorNode>("Root");

// Combat branch: if enemy visible, engage
auto combat = std::make_unique<SequenceNode>("Combat");
combat->AddChild(std::make_unique<ConditionNode>("SeePlayer",
    [](const Blackboard& bb) { return bb.Get<bool>("enemyVisible", false); }));
combat->AddChild(std::make_unique<ActionNode>("Attack",
    [this](float dt, Blackboard& bb) -> NodeStatus {
        // Your attack logic here
        return NodeStatus::Running;
    }));

root->AddChild(std::move(combat));
// Fallback: idle
root->AddChild(std::make_unique<ActionNode>("Idle",
    [](float, Blackboard&) { return NodeStatus::Running; }));

auto tree = std::make_unique<BehaviorTree>("MyEnemyAI");
tree->SetRoot(std::move(root));
```

### Wave Spawner

For escalating waves of enemies:

```cpp
#include "Game/WaveSpawner.h"

m_waveSpawner = std::make_unique<Spark::WaveSpawner>();
m_waveSpawner->Initialize(spawnPoints);
m_waveSpawner->SetTotalWaves(20);
m_waveSpawner->SetRestDuration(8.0f);
m_waveSpawner->GetCallbacks().onWaveStart = [](int wave, const std::string& msg) {
    // Show wave announcement
};
m_waveSpawner->Start();

// In Update():
m_waveSpawner->Update(dt, GetAliveEnemyCount(), this);
```

## Step 6: Add Weapons and Projectiles

```cpp
#include "Projectiles/ProjectilePool.h"

m_projectilePool = std::make_unique<ProjectilePool>(100);
m_projectilePool->Initialize(device, context);

// Fire a bullet
m_projectilePool->FireProjectile(ProjectileType::BULLET, position, direction, speed);
m_projectilePool->FireRocket(position, direction, 40.0f);
m_projectilePool->FireGrenade(position, direction, 25.0f);

// In Update():
m_projectilePool->Update(dt);
// In Render():
m_projectilePool->Render(viewMatrix, projMatrix);
```

## Step 7: Add Sound

```cpp
// Load sounds (once, during initialization)
auto* audio = m_context->GetAudio();
audio->LoadSound("gunshot", L"Assets/Sounds/gunshot.wav");
audio->LoadSound("explosion", L"Assets/Sounds/explosion.wav");
audio->LoadSound("ambient", L"Assets/Sounds/forest.wav");

// Play 2D sound (UI, music)
audio->PlaySound("gunshot", 0.8f);

// Play 3D positional sound
audio->PlaySound3D("explosion", explosionPos, 1.0f);

// Set listener position (usually from camera, every frame)
audio->Console_SetListenerPosition(camPos.x, camPos.y, camPos.z);

// Volume controls
audio->SetMasterVolume(0.8f);
audio->SetSFXVolume(1.0f);
audio->SetMusicVolume(0.5f);
```

## Step 8: Physics

Physics uses the Jolt integration. Add physics bodies via ECS components:

```cpp
#include "Engine/ECS/Components/PhysicsComponents.h"

// In your ECS world:
auto entity = world.CreateEntity("Box");
auto& transform = world.AddComponent<TransformComponent>(entity);
transform.position = {0, 10, 0};

auto& body = world.AddComponent<RigidBodyComponent>(entity);
body.type = BodyType::Dynamic;
body.mass = 10.0f;

auto& collider = world.AddComponent<ColliderComponent>(entity);
collider.shape = ColliderShape::Box;
collider.halfExtents = {0.5f, 0.5f, 0.5f};
```

Or use the physics system directly:

```cpp
auto* physics = m_context->GetPhysics();
physics->RayCast(origin, direction, maxDistance, hitResult);
```

## Step 9: Save and Load

```cpp
auto& save = Spark::SaveSystem::GetInstance();
save.Initialize("Saves");

// Save current state to slot 1
save.Save(1, world);

// Load from slot 1
save.Load(1, world);
```

Register custom component serializers for your game data:

```cpp
save.GetSerializerRegistry().Register<MyComponent>(
    [](const MyComponent& c) { /* serialize to JSON */ },
    [](MyComponent& c, const json& data) { /* deserialize */ });
```

## Step 10: Vehicles

SparkEngine includes a full vehicle system with 9 vehicle types:

```cpp
#include "Game/VehicleSystem.h"

auto* vehicle = m_vehicleSystem->SpawnVehicle(
    SparkEditor::VehicleType::BUGGY, {25.0f, 0.5f, 0.0f}, device, context);

// Vehicles support multiple seats
vehicle->EnterSeat(player, -1);  // -1 = first available
vehicle->ExitSeat(player);

// Vehicle weapons fire through the projectile pool automatically
vehicle->SetProjectilePool(m_projectilePool.get());
```

| Vehicle | Type | Seats | Weapon |
|---------|------|-------|--------|
| Buggy | Ground | Driver + Gunner | Machine Gun |
| Tank | Ground | Driver (cannon) + Gunner | Railgun + MG |
| APC | Ground | Driver + 2 Gunners | Machine Guns |
| Motorcycle | Ground | Driver | Pistol |
| Helicopter | Aerial | Pilot + Gunner | Minigun |
| Jet | Aerial | Pilot | Rockets |
| Dropship | Aerial | Pilot + 2 Gunners + 4 Passengers | Machine Guns |

---

## For Artists: Working Without Code

You don't need to write C++ to create content for SparkEngine.

### Importing Assets

SparkEngine loads these formats directly:
- **3D Models:** `.obj` (Wavefront), `.glb` / `.gltf` (glTF 2.0)
- **Textures:** `.png`, `.jpg`, `.bmp`, `.tga`, `.dds`
- **Audio:** `.wav`

Place assets in the `Assets/` directory:
```
Assets/
  Models/    -- 3D models
  Textures/  -- Texture files
  Sounds/    -- Audio files
  Scenes/    -- Scene files (.scene)
  Scripts/   -- AngelScript files (.as)
```

### Using the Editor

Start `SparkEditor.exe` as a separate executable from the matching multi-config output (for example, `build\bin\Release\SparkEditor.exe`); it is not toggled with F1 from `SparkEngine`. The editor product is in the intended stable-v1 set but remains blocked and uncertified. Current development panels include:

| Panel | What it does |
|-------|-------------|
| **Scene Hierarchy** | View and select all objects in the scene |
| **Properties** | Inspect and edit available entity and component properties |
| **Asset Browser** | Browse and drag-drop assets into the scene |
| **Material Editor** | Create and edit PBR materials (albedo, metallic, roughness, normal) |
| **Terrain Editor** | Sculpt and paint terrain |
| **Lighting** | Place and configure lights (directional, point, spot) |
| **Particle Editor** | Create and tune particle effects |
| **Gizmo System** | Implemented translation path; rotation, scale, and complete undo remain incomplete |

### Workflow

1. **Model:** Create your 3D model in Blender/Maya/3ds Max, export as `.glb`
2. **Place in Assets/Models/** and they appear in the Asset Browser
3. **Start SparkEditor separately** from the matching build output, then drag models into the scene
4. **Position** using the implemented translation gizmo or the Properties panel
5. **Save the scene** -- it's stored as a `.scene` file in `Assets/Scenes/`
6. **Preview** with a separately launched engine and an explicit `-game <module-path>` selection

### Collaborative Editing

Collaborative editing is experimental and outside stable-v1. Do not treat the current development surfaces as a supported multi-user editing workflow.

---

## For Designers: Scripting and Configuration

### AngelScript

AngelScript work is experimental and outside stable-v1. The following is an API-oriented development example, not a stable scripting-runtime or hot-reload promise:

```angelscript
// Assets/Scripts/my_script.as

void OnStart()
{
    Print("Hello from AngelScript!");
    Entity@ player = CreateEntity("ScriptedNPC");
}

void OnUpdate(float dt)
{
    if (IsKeyDown("E"))
    {
        Print("Interaction triggered!");
    }
}
```

Do not rely on saving a script as a stable hot-reload contract; validate the active scripting path for the current development build.

### Console Commands

Press **` (backtick)** to open the console. Useful commands:

```
help                    -- List all commands
spawn_enemy grunt 10 1 10  -- Spawn an enemy at position
spawn_vehicle buggy 0 1 0  -- Spawn a vehicle
wave_start              -- Start enemy waves
wave_skip 5             -- Skip to wave 5
teleport 0 10 0         -- Teleport player
timescale 0.5           -- Slow motion
god                     -- Toggle invincibility
noclip                  -- Toggle fly mode
fps                     -- Show framerate
physics_info            -- Physics system stats
audio_info              -- Audio system stats
```

### Wave Spawner Configuration

The wave spawner can be configured via console or code:

```
wave_set_total 30       -- Total waves
wave_set_rest 5.0       -- Rest period between waves (seconds)
wave_set_difficulty 1.5 -- Difficulty multiplier
```

---

## Reference Slice: SparkGameFPS

`GameModules/SparkGameFPS/` is the declared first-party **single-player** vertical-slice reference for stable-v1. It is in scope but blocked and uncertified; it is not a complete game, a production claim, or multiplayer evidence. Study its source as a reference while independently validating the features your own module uses.

---

## Next Steps

- [Architecture Overview](Architecture-Overview.md) -- Understand the engine design
- [Entity Component System](../subsystems/Entity-Component-System.md) -- ECS in depth
- [AI and Navigation](../subsystems/AI-and-Navigation.md) -- Behavior trees, NavMesh, perception
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Rendering pipeline details
- [Networking](../subsystems/Networking.md) -- Multiplayer implementation
- [Scripting with AngelScript](../subsystems/Scripting-with-AngelScript.md) -- Script API reference
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- Editor panel reference
