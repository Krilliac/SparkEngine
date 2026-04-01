# Making Your First Game

This guide walks you through building a playable game with SparkEngine -- from an empty module template to a combat arena with AI enemies, weapons, vehicles, and audio. It covers workflows for **programmers**, **artists**, and **gameplay designers**.

> **Prerequisites:** Complete the [Getting Started](Getting-Started) guide first. You need a working build before proceeding.

## Step 1: Create a Game Module

SparkEngine loads game logic as a DLL/shared library. Start from the template:

```bash
cp -r Templates/EmptyProject GameModules/MyGame
```

This gives you:

```
MyGame/
  CMakeLists.txt         # Build configuration (auto-discovered by CMake)
  Source/
    GameModule.h         # IModule implementation
    GameModule.cpp       # DLL entry point + factory exports
  spark.project.json     # Project metadata
  spark.modules.json     # Module manifest
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

The engine exports are in `GameModule.cpp` (already provided by the template):

```cpp
extern "C" SPARK_GAME_EXPORT Spark::IModule* CreateModule()  { return new MyGameModule(); }
extern "C" SPARK_GAME_EXPORT void DestroyModule(Spark::IModule* m) { delete m; }
```

### Build and Run

```bash
cmake --preset linux-gcc-release
cmake --build build/linux-gcc-release
./build/bin/SparkEngine -game build/bin/MyGame.dll
```

## Step 2: Access Engine Services

The `IEngineContext` gives you access to every engine subsystem:

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

For a full FPS controller with jumping, sprinting, crouching, health, and weapons, see `GameModules/SparkGameFPS/Source/Game/Player.h`.

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

Press **F1** during gameplay to open SparkEditor. Key panels:

| Panel | What it does |
|-------|-------------|
| **Scene Hierarchy** | View and select all objects in the scene |
| **Properties** | Edit position, rotation, scale, and component values |
| **Asset Browser** | Browse and drag-drop assets into the scene |
| **Material Editor** | Create and edit PBR materials (albedo, metallic, roughness, normal) |
| **Terrain Editor** | Sculpt and paint terrain |
| **Lighting** | Place and configure lights (directional, point, spot) |
| **Particle Editor** | Create and tune particle effects |
| **Gizmo System** | Translate, rotate, scale objects with visual handles |

### Workflow

1. **Model:** Create your 3D model in Blender/Maya/3ds Max, export as `.glb`
2. **Place in Assets/Models/** and they appear in the Asset Browser
3. **Open SparkEditor** (F1), drag models into the scene
4. **Position** using the gizmo handles or the Properties panel
5. **Save the scene** -- it's stored as a `.scene` file in `Assets/Scenes/`
6. **Preview** by closing the editor (F1 again) to return to gameplay

### Collaborative Editing

SparkEditor supports multi-user editing. Multiple people can edit the same scene simultaneously -- see [Collaborative Editing](Collaborative-Editing).

---

## For Designers: Scripting and Configuration

### AngelScript

Write gameplay logic in AngelScript without recompiling:

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

Scripts hot-reload when saved -- no need to restart the engine.

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

## Complete Example: SparkGameFPS

The `GameModules/SparkGameFPS/` module is a complete FPS arena game demonstrating every system above:

- **Player:** Full FPS controller with 3 class types (Scout, Recon, Titan)
- **Enemies:** 12 AI enemies with behavior trees, 6 archetype types
- **Weapons:** 18 weapon types with projectile physics
- **Vehicles:** 9 vehicle types with mounted weapons
- **HUD:** Health bars, ammo counter, crosshair, kill feed, minimap
- **Wave spawner:** 20 escalating waves with boss rounds
- **Progression:** XP, leveling, unlocks
- **Loot:** Enemy drops with rarity tiers
- **Quests:** 3-quest system with tracking

Study this module as a reference for building your own game.

---

## Next Steps

- [Architecture Overview](Architecture-Overview) -- Understand the engine design
- [Entity Component System](Entity-Component-System) -- ECS in depth
- [AI and Navigation](AI-and-Navigation) -- Behavior trees, NavMesh, perception
- [Rendering and Graphics](Rendering-and-Graphics) -- Rendering pipeline details
- [Networking](Networking) -- Multiplayer implementation
- [Scripting with AngelScript](Scripting-with-AngelScript) -- Script API reference
- [SparkEditor](SparkEditor) -- Editor panel reference
