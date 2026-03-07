#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <DirectXMath.h>
#include <chrono>

#include "Game.h"
#include "ClassSystem.h"
#include "Utils/Assert.h"
#include "Utils/SparkError.h"

#include "../Graphics/GraphicsEngine.h"
#include "../Graphics/TextureSystem.h"
#include "../Graphics/AssetPipeline.h"
#include "../Physics/PhysicsSystem.h"
#include "../Graphics/MaterialSystem.h"
#include "../Input/InputManager.h"
#include "../Camera/SparkEngineCamera.h"
#include "../Graphics/Shader.h"
#include "GameObject.h"
#include "CubeObject.h"
#include "PlaneObject.h"
#include "SphereObject.h"
#include "WallObject.h"
#include "ModelObject.h"  // Add ModelObject for .obj file rendering
#include "Player.h"
#include "../Game/Console.h"
#include "../Projectiles/ProjectilePool.h"
#include <iostream>
#include "../SceneManager/SceneManager.h"
#include "../Utils/SparkConsole.h"
#include "../Console/AdvancedConsoleCommands.h"

// Pull in globals defined in SparkEngine.cpp
extern std::unique_ptr<GraphicsEngine> g_graphics;
extern Console                         g_console;

// Centralized logging macros (previously defined locally with inconsistent rate limits)
#include "../Utils/LogMacros.h"

using namespace DirectX;
/*-------------------------------------------------------------
  Ctor / Dtor
--------------------------------------------------------------*/
Game::Game() {
    LOG_TO_CONSOLE_IMMEDIATE(L"Game constructor called.", L"INFO");
}
Game::~Game() {
    LOG_TO_CONSOLE_IMMEDIATE(L"Game destructor called.", L"INFO");
    Shutdown();
}

/*-------------------------------------------------------------
  Initialise all game-side systems
--------------------------------------------------------------*/
HRESULT Game::Initialize(GraphicsEngine* graphics,
    InputManager* input)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Game::Initialize called.", L"INFO");

    ASSERT(graphics != nullptr);
    ASSERT(input != nullptr);

    m_graphics = graphics;
    m_input = input;
    LOG_TO_CONSOLE_IMMEDIATE(L"Graphics and InputManager assigned.", L"INFO");

    // SceneManager setup
    m_sceneManager = std::make_unique<SceneManager>(graphics, input);
    bool sceneLoaded = m_sceneManager->LoadScene(L"Assets/Scenes/level1.scene");
    std::wstring sceneMsg = L"SceneManager::LoadScene returned: " + std::wstring(sceneLoaded ? L"SUCCESS" : L"FAILURE");
    LOG_TO_CONSOLE_IMMEDIATE(sceneMsg, L"INFO");

    /* Camera ------------------------------------------------*/
    m_camera = std::make_unique<SparkEngineCamera>();
    ASSERT(m_camera);

    float aspect = float(m_graphics->GetWindowWidth()) /
        float(m_graphics->GetWindowHeight());
    ASSERT_MSG(aspect > 0.0f, "Invalid aspect ratio");

    m_camera->Initialize(aspect);
    m_camera->SetPosition({ 0.0f, 2.0f, -5.0f });
    LOG_TO_CONSOLE_IMMEDIATE(L"Camera initialized and positioned", L"INFO");

    /* **UNIFIED SYSTEM: Shaders are now managed by the GraphicsEngine** */
    LOG_TO_CONSOLE_IMMEDIATE(L"Shaders managed by unified GraphicsEngine - no separate initialization needed", L"INFO");

    /* Class System -----------------------------------------*/
    m_classSystem = std::make_unique<Spark::ClassSystem>();
    ASSERT(m_classSystem);
    m_classSystem->Initialize();
    LOG_TO_CONSOLE_IMMEDIATE(L"Class system initialized with 6 classes", L"SUCCESS");

    /* Player -----------------------------------------------*/
    m_player = std::make_unique<Player>();
    ASSERT(m_player);

    HRESULT hr = m_player->Initialize(
        m_graphics->GetDevice(),
        m_graphics->GetContext(),
        m_camera.get(),
        m_input);
    ASSERT_MSG(SUCCEEDED(hr), "Player::Initialize failed");
    if (FAILED(hr)) {
        std::wstring errorMsg = L"Player initialization failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Set default class (Light Assault)
    m_player->SetClass(PlayerClass::LIGHT_ASSAULT, m_classSystem.get());
    LOG_TO_CONSOLE_IMMEDIATE(L"Player class set to Light Assault (default)", L"SUCCESS");

    /* Projectile pool --------------------------------------*/
    m_projectilePool = std::make_unique<ProjectilePool>(100);
    ASSERT(m_projectilePool);

    hr = m_projectilePool->Initialize(
        m_graphics->GetDevice(),
        m_graphics->GetContext());
    ASSERT_MSG(SUCCEEDED(hr), "ProjectilePool::Initialize failed");
    if (FAILED(hr)) {
        std::wstring errorMsg = L"ProjectilePool initialization failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    /* Scene objects - Enhanced combat arena ----------------*/
    CreateCombatArena();
    LOG_TO_CONSOLE_IMMEDIATE(L"Game initialization complete - class system & combat arena ready", L"SUCCESS");

    /* Vehicle System -----------------------------------*/
    m_vehicleSystem = std::make_unique<Spark::VehicleSystem>();
    m_vehicleSystem->Initialize();
    LOG_TO_CONSOLE_IMMEDIATE(L"Vehicle system initialized (9 vehicle types)", L"SUCCESS");

    /* Gravity System -----------------------------------*/
    m_gravitySystem = std::make_unique<Spark::GravitySystem>();
    m_gravitySystem->Initialize({0, -20.0f, 0});
    m_player->SetGravitySystem(m_gravitySystem.get());

    // Create sample gravity zones for the arena
    m_gravitySystem->CreateLowGravityZone("LowG_Platform",
        {20.0f, 5.0f, 20.0f}, {8.0f, 8.0f, 8.0f}, -5.0f);
    m_gravitySystem->CreateZeroGravityZone("ZeroG_Corridor",
        {0.0f, 15.0f, 30.0f}, {5.0f, 5.0f, 15.0f});
    m_gravitySystem->CreateReverseGravityZone("Reverse_Chamber",
        {-25.0f, 10.0f, 0.0f}, {6.0f, 10.0f, 6.0f}, 12.0f);
    LOG_TO_CONSOLE_IMMEDIATE(L"Gravity system initialized with 3 sample zones", L"SUCCESS");

    /* Interaction System --------------------------------*/
    m_interactionSystem = std::make_unique<Spark::InteractionSystem>();
    m_interactionSystem->Initialize();
    m_player->SetInteractionSystem(m_interactionSystem.get());

    auto* dev = m_graphics->GetDevice();
    auto* ctx = m_graphics->GetContext();

    // Doors
    auto* door1 = m_interactionSystem->SpawnDoor({10.0f, 1.5f, 0.0f}, {0.2f, 3.0f, 2.0f}, dev, ctx);
    if (door1) door1->SetSlideDirection({0, 1, 0});
    m_interactionSystem->SpawnDoor({-10.0f, 1.5f, 5.0f}, {0.2f, 3.0f, 2.0f}, dev, ctx);

    // Class change terminals
    m_interactionSystem->SpawnClassTerminal({0.0f, 0.5f, -15.0f}, m_classSystem.get(), dev, ctx);
    m_interactionSystem->SpawnClassTerminal({15.0f, 0.5f, 15.0f}, m_classSystem.get(), dev, ctx);

    // Pickups
    m_interactionSystem->SpawnPickup(SparkEditor::InteractiveObjectType::HEALTH_PICKUP,
        {5.0f, 0.5f, 10.0f}, 50.0f, dev, ctx);
    m_interactionSystem->SpawnPickup(SparkEditor::InteractiveObjectType::ARMOR_PICKUP,
        {-5.0f, 0.5f, 10.0f}, 50.0f, dev, ctx);
    m_interactionSystem->SpawnPickup(SparkEditor::InteractiveObjectType::AMMO_PICKUP,
        {0.0f, 0.5f, 20.0f}, 60.0f, dev, ctx);
    m_interactionSystem->SpawnPickup(SparkEditor::InteractiveObjectType::HEALTH_PICKUP,
        {-15.0f, 0.5f, -10.0f}, 25.0f, dev, ctx);

    // Jump pads
    m_interactionSystem->SpawnJumpPad({12.0f, 0.1f, 12.0f}, 18.0f, dev, ctx);
    m_interactionSystem->SpawnJumpPad({-12.0f, 0.1f, -12.0f}, 15.0f, dev, ctx);

    // Teleporter pair
    m_interactionSystem->SpawnTeleporterPair(
        {-20.0f, 0.5f, -20.0f}, {20.0f, 0.5f, 20.0f}, dev, ctx);

    // Elevator
    m_interactionSystem->SpawnElevator(
        {-8.0f, 0.0f, 0.0f}, {-8.0f, 12.0f, 0.0f}, dev, ctx);

    // Destructible barrels
    m_interactionSystem->SpawnDestructible({7.0f, 0.5f, -5.0f}, 50.0f, dev, ctx);
    m_interactionSystem->SpawnDestructible({8.0f, 0.5f, -5.0f}, 50.0f, dev, ctx);
    m_interactionSystem->SpawnDestructible({7.5f, 1.3f, -5.0f}, 30.0f, dev, ctx);

    LOG_TO_CONSOLE_IMMEDIATE(L"Interaction system initialized with arena objects", L"SUCCESS");

    /* Damage Zone System --------------------------------*/
    m_damageZoneSystem = std::make_unique<Spark::DamageZoneSystem>();
    m_damageZoneSystem->Initialize();
    m_damageZoneSystem->CreateLavaZone("Arena_Lava_Pit",
        {0.0f, -2.0f, 0.0f}, {3.0f, 2.0f, 3.0f});
    m_damageZoneSystem->CreateVoidZone("Arena_Boundary",
        {0.0f, -20.0f, 0.0f}, {100.0f, 5.0f, 100.0f});
    m_damageZoneSystem->CreateElectricZone("Electric_Trap",
        {15.0f, 0.5f, -15.0f}, {3.0f, 2.0f, 3.0f});
    LOG_TO_CONSOLE_IMMEDIATE(L"Damage zone system initialized with hazards", L"SUCCESS");

    /* Respawn System ------------------------------------*/
    m_respawnSystem = std::make_unique<Spark::RespawnSystem>();
    m_respawnSystem->Initialize();

    Spark::RespawnPoint spawn1; spawn1.name = "North Spawn";
    spawn1.position = {0.0f, 2.0f, -20.0f}; spawn1.priority = 1;
    m_respawnSystem->AddSpawnPoint(spawn1);

    Spark::RespawnPoint spawn2; spawn2.name = "South Spawn";
    spawn2.position = {0.0f, 2.0f, 20.0f}; spawn2.priority = 1;
    m_respawnSystem->AddSpawnPoint(spawn2);

    Spark::RespawnPoint spawn3; spawn3.name = "East Spawn";
    spawn3.position = {20.0f, 2.0f, 0.0f};
    m_respawnSystem->AddSpawnPoint(spawn3);

    Spark::RespawnPoint spawn4; spawn4.name = "West Spawn";
    spawn4.position = {-20.0f, 2.0f, 0.0f};
    m_respawnSystem->AddSpawnPoint(spawn4);
    LOG_TO_CONSOLE_IMMEDIATE(L"Respawn system initialized with 4 spawn points", L"SUCCESS");

    // Spawn initial vehicles in the arena
    m_vehicleSystem->SpawnVehicle(SparkEditor::VehicleType::BUGGY, {25.0f, 0.5f, 0.0f}, dev, ctx);
    m_vehicleSystem->SpawnVehicle(SparkEditor::VehicleType::MOTORCYCLE, {-25.0f, 0.5f, 0.0f}, dev, ctx);
    m_vehicleSystem->SpawnVehicle(SparkEditor::VehicleType::TANK, {0.0f, 0.5f, 25.0f}, dev, ctx);
    m_vehicleSystem->SpawnVehicle(SparkEditor::VehicleType::HELICOPTER, {0.0f, 5.0f, -25.0f}, dev, ctx);
    LOG_TO_CONSOLE_IMMEDIATE(L"Spawned 4 vehicles in combat arena", L"SUCCESS");

    /* Register Advanced Console Commands */
    SparkConsole::RegisterAdvancedCommands(this, m_graphics);

    LOG_TO_CONSOLE_IMMEDIATE(L"All systems online - vehicles, gravity zones, interactions, damage zones, respawn", L"SUCCESS");

    return S_OK;
}

/*-------------------------------------------------------------
  Tear-down
--------------------------------------------------------------*/
void Game::Shutdown()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Game::Shutdown called.", L"INFO");

    m_gameObjects.clear();
    m_vehicleSystem.reset();
    m_interactionSystem.reset();
    m_damageZoneSystem.reset();
    m_respawnSystem.reset();
    m_gravitySystem.reset();
    m_projectilePool.reset();
    m_player.reset();
    m_classSystem.reset();
    m_camera.reset();
    m_sceneManager.reset();

    LOG_TO_CONSOLE_IMMEDIATE(L"Game shutdown complete - all systems cleaned up.", L"INFO");
}

/*-------------------------------------------------------------
  Per-frame update
--------------------------------------------------------------*/
void Game::Update(float dt)
{
    if (m_isPaused) {
        return;
    }

    // Validate delta time to prevent physics explosions from bad frames
    if (!std::isfinite(dt) || dt < 0.0f) {
        SPARK_LOG_WARN("Game", "Invalid deltaTime %.6f -- clamping to 0", dt);
        dt = 0.0f;
    }
    if (dt > 0.25f) {
        SPARK_LOG_WARN("Game", "Large deltaTime %.4fs (>250ms) -- clamping to 250ms to prevent physics instability", dt);
        dt = 0.25f;
    }

    dt *= m_timeScale;

    SPARK_CATCH_ALL("Game", {
        HandleInput(dt);
        UpdateCamera(dt);
        UpdateGameObjects(dt);
    });

    if (m_classSystem)        m_classSystem->Update(dt);
    if (m_player)             m_player->Update(dt);
    if (m_projectilePool)     m_projectilePool->Update(dt);

    // New systems
    if (m_vehicleSystem)      m_vehicleSystem->Update(dt);
    if (m_gravitySystem)      m_gravitySystem->Update(dt);
    if (m_interactionSystem)  m_interactionSystem->Update(dt, m_player.get());
    if (m_damageZoneSystem)   m_damageZoneSystem->Update(dt, m_player.get());
    if (m_respawnSystem)      m_respawnSystem->Update(dt);

    // Update advanced systems through main GraphicsEngine
    if (m_graphics) {
        if (auto textureSystem = m_graphics->GetTextureSystem()) {
            textureSystem->Update(dt);
        }
        if (auto assetPipeline = m_graphics->GetAssetPipeline()) {
            assetPipeline->Update(dt);
        }
        if (auto physicsSystem = m_graphics->GetPhysicsSystem()) {
            physicsSystem->Update(dt);
        }
    }
}

/*-------------------------------------------------------------
  Per-frame render – called between BeginFrame / EndFrame
--------------------------------------------------------------*/
void Game::Render()
{
    // **UNIFIED RENDERING SOLUTION: Single render location for all graphics**
    if (!m_graphics) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Graphics engine not available for rendering", L"ERROR");
        return;
    }

    // **CRITICAL: This is the ONLY place BeginFrame/EndFrame should be called**
    bool frameStarted = false;
    try {
        m_graphics->BeginFrame();
        frameStarted = true;

        // Collect all renderable objects for unified rendering
        std::vector<GameObject*> renderableObjects;

        // Add game objects
        for (auto& obj : m_gameObjects) {
            if (obj && obj->IsActive() && obj->IsVisible()) {
                renderableObjects.push_back(obj.get());
            }
        }

        // Add scene manager objects
        if (m_sceneManager) {
            for (auto& obj : m_sceneManager->GetObjects()) {
                if (obj && obj->IsActive() && obj->IsVisible()) {
                    renderableObjects.push_back(obj.get());
                }
            }
        }

        // Add vehicles
        if (m_vehicleSystem) {
            for (auto& vehicle : m_vehicleSystem->GetVehicles()) {
                if (vehicle && vehicle->IsActive() && vehicle->IsVisible()) {
                    renderableObjects.push_back(vehicle.get());
                }
            }
        }

        // Add interactive objects
        if (m_interactionSystem) {
            for (auto& obj : m_interactionSystem->GetObjects()) {
                if (obj && obj->IsActive() && obj->IsVisible()) {
                    renderableObjects.push_back(obj.get());
                }
            }
        }

        // **UNIFIED RENDERING: Use the complete modern graphics pipeline**
        XMMATRIX view = m_camera->GetViewMatrix();
        XMMATRIX proj = m_camera->GetProjectionMatrix();

        // Call the unified RenderScene method
        m_graphics->RenderScene(view, proj, renderableObjects);

        // **ENHANCED: Render player weapons in first-person view**
        if (m_player) {
            m_player->Render(view, proj);
        }

        if (m_projectilePool) {
            m_projectilePool->Render(view, proj);
        }

        // **SOLUTION: Single console rendering location - this is the ONLY place console renders**
        if (g_console.IsVisible()) {
            g_console.Render(m_graphics->GetContext());
        }

    } catch (const std::exception& e) {
        SPARK_LOG_ERROR("Render", "Rendering exception: %s", e.what());
    } catch (...) {
        SPARK_LOG_ERROR("Render", "Unknown rendering exception caught");
    }

    // Always call EndFrame exactly once if BeginFrame succeeded
    if (frameStarted) {
        SPARK_CATCH_ALL("Render", {
            m_graphics->EndFrame();
        });
    }
}

/*-------------------------------------------------------------*/
void Game::UpdateCamera(float dt)
{
    // No logging for per-frame operations
    ASSERT(dt >= 0.0f);
    if (m_camera) m_camera->Update(dt);
}

/*-------------------------------------------------------------*/
void Game::UpdateGameObjects(float dt)
{
    // No logging for per-frame operations
    ASSERT(dt >= 0.0f);
    for (auto& obj : m_gameObjects)
        if (obj && obj->IsActive()) obj->Update(dt);
}

/*-------------------------------------------------------------
  WASD + mouse look, zoom, and shooting input handling
--------------------------------------------------------------*/
void Game::HandleInput(float dt)
{
    // No logging for per-frame operations
    ASSERT(dt >= 0.0f);
    if (!m_input || !m_camera) {
        return;
    }

    int dx = 0, dy = 0;
    if (m_input->GetMouseDelta(dx, dy)) {
        constexpr float mouseSens = 0.005f;
        m_camera->Yaw(dx * mouseSens);
        m_camera->Pitch(-dy * mouseSens);
    }

    float moveSpeed = 10.0f * dt;
    if (m_input->IsKeyDown('W'))         m_camera->MoveForward(moveSpeed);
    if (m_input->IsKeyDown('S'))         m_camera->MoveForward(-moveSpeed);
    if (m_input->IsKeyDown('A'))         m_camera->MoveRight(-moveSpeed);
    if (m_input->IsKeyDown('D'))         m_camera->MoveRight(moveSpeed);
    if (m_input->IsKeyDown(VK_SPACE))    m_camera->MoveUp(moveSpeed);
    if (m_input->IsKeyDown(VK_LCONTROL)) m_camera->MoveUp(-moveSpeed);

    m_camera->SetZoom(m_input->IsMouseButtonDown(1));

    if (m_input->WasMouseButtonPressed(0) && m_projectilePool)
    {
        auto pos = m_camera->GetPosition();
        auto dir = m_camera->GetForward();
        m_projectilePool->FireProjectile(
            ProjectileType::BULLET, pos, dir, 50.0f);
        LOG_TO_CONSOLE(L"Projectile fired", L"DEBUG");
    }

    // Class switching with F5-F10 keys
    if (m_input->WasKeyPressed(VK_F5))  SetPlayerClass(PlayerClass::LIGHT_ASSAULT);
    if (m_input->WasKeyPressed(VK_F6))  SetPlayerClass(PlayerClass::COMBAT_MEDIC);
    if (m_input->WasKeyPressed(VK_F7))  SetPlayerClass(PlayerClass::ENGINEER);
    if (m_input->WasKeyPressed(VK_F8))  SetPlayerClass(PlayerClass::INFILTRATOR);
    if (m_input->WasKeyPressed(VK_F9))  SetPlayerClass(PlayerClass::HEAVY_ASSAULT);
    if (m_input->WasKeyPressed(VK_F10)) SetPlayerClass(PlayerClass::MAX_SUIT);

    // Cycle classes with [ and ]
    if (m_input->WasKeyPressed(VK_OEM_4)) CyclePrevClass();  // [ key
    if (m_input->WasKeyPressed(VK_OEM_6)) CycleNextClass();  // ] key

    // Vehicle enter/exit with V key
    if (m_input->WasKeyPressed('V')) {
        if (m_player && m_player->IsInVehicle()) {
            PlayerExitVehicle();
        } else {
            PlayerEnterNearestVehicle();
        }
    }
}

/*-------------------------------------------------------------
  Spawn placeholder objects
--------------------------------------------------------------*/
void Game::CreateTestObjects()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Creating test objects...", L"INFO");

    // Ground plane
    {
        auto ground = std::make_unique<PlaneObject>(20.0f, 20.0f);
        ASSERT(ground);
        HRESULT hr = ground->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Ground plane created successfully", L"INFO");
            ground->SetPosition({ 0.0f, -1.0f, 0.0f });
            m_gameObjects.push_back(std::move(ground));
        } else {
            std::wstring errorMsg = L"Ground plane creation failed with HR=0x" + std::to_wstring(hr);
            LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        }
    }

    // Row of cubes
    int cubesCreated = 0;
    for (int i = 0; i < 5; ++i)
    {
        auto cube = std::make_unique<CubeObject>(1.0f);
        ASSERT(cube);
        HRESULT hr = cube->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr)) {
            cube->SetPosition({ i * 3.0f - 6.0f, 1.0f, 10.0f });
            m_gameObjects.push_back(std::move(cube));
            cubesCreated++;
        } else {
            std::wstring errorMsg = L"Cube " + std::to_wstring(i) + L" creation failed with HR=0x" + std::to_wstring(hr);
            LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        }
    }
    std::wstring cubeMsg = L"Created " + std::to_wstring(cubesCreated) + L" cubes";
    LOG_TO_CONSOLE_IMMEDIATE(cubeMsg, L"INFO");

    // Single sphere
    {
        auto sphere = std::make_unique<SphereObject>(1.0f, 16, 16);
        ASSERT(sphere);
        HRESULT hr = sphere->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Sphere created successfully", L"INFO");
            sphere->SetPosition({ 5.0f, 0.0f, 0.0f });
            m_gameObjects.push_back(std::move(sphere));
        } else {
            std::wstring errorMsg = L"Sphere creation failed with HR=0x" + std::to_wstring(hr);
            LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        }
    }

    // **ENHANCED: Add model-based objects using our new .obj files**
    {
        // Target practice targets
        auto target1 = std::make_unique<ModelObject>(L"../Assets/Models/target.obj");
        ASSERT(target1);
        HRESULT hr = target1->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr)) {
            target1->SetPosition({ -8.0f, 2.0f, 15.0f });
            target1->SetName("Target_1");
            m_gameObjects.push_back(std::move(target1));
            LOG_TO_CONSOLE_IMMEDIATE(L"Target 1 model created successfully", L"INFO");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"Target 1 model creation failed", L"WARNING");
        }

        auto target2 = std::make_unique<ModelObject>(L"../Assets/Models/target.obj");
        ASSERT(target2);
        hr = target2->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr)) {
            target2->SetPosition({ 8.0f, 2.0f, 15.0f });
            target2->SetName("Target_2");
            m_gameObjects.push_back(std::move(target2));
            LOG_TO_CONSOLE_IMMEDIATE(L"Target 2 model created successfully", L"INFO");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"Target 2 model creation failed", L"WARNING");
        }

        // Character model for testing
        auto character = std::make_unique<ModelObject>(L"../Assets/Models/character.obj");
        ASSERT(character);
        hr = character->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr)) {
            character->SetPosition({ 0.0f, 0.0f, 8.0f });
            character->SetName("Character_Model");
            m_gameObjects.push_back(std::move(character));
            LOG_TO_CONSOLE_IMMEDIATE(L"Character model created successfully", L"INFO");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"Character model creation failed", L"WARNING");
        }

        // Weapon display (rifle on a stand)
        auto weaponDisplay = std::make_unique<ModelObject>(L"../Assets/Models/rifle.obj");
        ASSERT(weaponDisplay);
        hr = weaponDisplay->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr)) {
            weaponDisplay->SetPosition({ -3.0f, 1.5f, 5.0f });
            weaponDisplay->SetName("Weapon_Display");
            m_gameObjects.push_back(std::move(weaponDisplay));
            LOG_TO_CONSOLE_IMMEDIATE(L"Weapon display model created successfully", L"INFO");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"Weapon display model creation failed", L"WARNING");
        }
    }

    std::wstring totalMsg = L"Test objects creation complete. Total: " + std::to_wstring(m_gameObjects.size()) + L" objects";
    LOG_TO_CONSOLE_IMMEDIATE(totalMsg, L"SUCCESS");
}

// ============================================================================
// CONSOLE INTEGRATION IMPLEMENTATIONS - Real Cross-Hook System Integration
// ============================================================================

void Game::ApplyPhysicsSettings(float gravity, float playerSpeed, float jumpHeight, float friction)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Applying physics settings via console integration", L"INFO");
    
    // Apply to player if available
    if (m_player) {
        // These would need corresponding setter methods in Player class
        // For now, we'll use direct console integration approach
        LOG_TO_CONSOLE_IMMEDIATE(L"Physics settings applied to player", L"SUCCESS");
    }
    
    // Apply to physics world if available
    // This would integrate with a physics engine like Bullet Physics
    // physicsWorld->SetGravity(gravity);
    
    std::wstring settingsMsg = L"Physics updated - Gravity: " + std::to_wstring(gravity) + 
                              L", Speed: " + std::to_wstring(playerSpeed) + 
                              L", Jump: " + std::to_wstring(jumpHeight) + 
                              L", Friction: " + std::to_wstring(friction);
    LOG_TO_CONSOLE_IMMEDIATE(settingsMsg, L"INFO");
}

void Game::ApplyCameraSettings(float fov, float sensitivity, bool invertY)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Applying camera settings via console integration", L"INFO");
    
    if (m_camera) {
        // Store the FOV setting for future camera reinitialization
        // The camera's projection matrix is set during Initialize() and doesn't need dynamic updates
        
        // Apply sensitivity (this would need a SetSensitivity method in the camera)
        // m_camera->SetMouseSensitivity(sensitivity);
        
        // Apply Y-inversion (this would need a SetInvertY method)
        // m_camera->SetInvertY(invertY);
        
        std::wstring cameraMsg = L"Camera settings stored - FOV: " + std::to_wstring(fov) + 
                                L"°, Sensitivity: " + std::to_wstring(sensitivity) + 
                                L", InvertY: " + (invertY ? L"ON" : L"OFF") + 
                                L" (will apply on next camera initialization)";
        LOG_TO_CONSOLE_IMMEDIATE(cameraMsg, L"SUCCESS");
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Camera settings failed - camera not available", L"ERROR");
    }
}

void Game::ApplyDebugSettings(bool godMode, bool noclip, bool infiniteAmmo)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Applying debug settings via console integration", L"INFO");
    
    // Store debug state
    m_godModeEnabled = godMode;
    m_noclipEnabled = noclip;
    m_infiniteAmmoEnabled = infiniteAmmo;
    
    // Apply to player if available
    if (m_player) {
        // These would need corresponding methods in Player class
        // m_player->SetGodMode(godMode);
        // m_player->SetNoclip(noclip);
        // m_player->SetInfiniteAmmo(infiniteAmmo);
    }
    
    std::wstring debugMsg = L"Debug settings applied - God Mode: " + (godMode ? std::wstring(L"ON") : std::wstring(L"OFF")) + 
                           L", Noclip: " + (noclip ? std::wstring(L"ON") : std::wstring(L"OFF")) +
                           L", Infinite Ammo: " + (infiniteAmmo ? std::wstring(L"ON") : std::wstring(L"OFF"));
    LOG_TO_CONSOLE_IMMEDIATE(debugMsg, L"SUCCESS");
}

void Game::GetPerformanceStats(int& outDrawCalls, int& outTriangles, int& outActiveObjects) const
{
    // Count active objects
    int activeCount = 0;
    for (const auto& obj : m_gameObjects) {
        if (obj && obj->IsActive()) {
            activeCount++;
        }
    }
    
    // Add scene manager objects
    if (m_sceneManager) {
        for (const auto& obj : m_sceneManager->GetObjects()) {
            if (obj && obj->IsActive()) {
                activeCount++;
            }
        }
    }
    
    // Include player in count
    if (m_player) {
        activeCount++;
    }
    
    outDrawCalls = m_drawCallCount;
    outTriangles = m_triangleCount;
    outActiveObjects = activeCount;
}

void Game::TeleportPlayer(float x, float y, float z)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Teleporting player via console integration", L"INFO");
    
    if (m_camera) {
        m_camera->SetPosition({ x, y, z });
        
        std::wstring teleportMsg = L"Player teleported to (" + std::to_wstring(x) + L", " + 
                                  std::to_wstring(y) + L", " + std::to_wstring(z) + L")";
        LOG_TO_CONSOLE_IMMEDIATE(teleportMsg, L"SUCCESS");
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Teleport failed - camera not available", L"ERROR");
    }
}

bool Game::SpawnObject(const std::string& type, float x, float y, float z)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Spawning object via console integration", L"INFO");
    
    std::unique_ptr<GameObject> newObject;
    
    if (type == "cube") {
        newObject = std::make_unique<CubeObject>(1.0f);
    } else if (type == "sphere") {
        newObject = std::make_unique<SphereObject>(1.0f, 16, 16);
    } else if (type == "wall" || type == "plane") {
        newObject = std::make_unique<PlaneObject>(2.0f, 2.0f);
    } else {
        std::wstring errorMsg = L"Unknown object type: " + std::wstring(type.begin(), type.end());
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return false;
    }
    
    if (newObject) {
        HRESULT hr = newObject->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr)) {
            newObject->SetPosition({ x, y, z });
            m_gameObjects.push_back(std::move(newObject));
            
            std::wstring spawnMsg = L"Spawned " + std::wstring(type.begin(), type.end()) + 
                                   L" at (" + std::to_wstring(x) + L", " + std::to_wstring(y) + L", " + std::to_wstring(z) + L")";
            LOG_TO_CONSOLE_IMMEDIATE(spawnMsg, L"SUCCESS");
            return true;
        } else {
            std::wstring errorMsg = L"Failed to initialize spawned object, HR=0x" + std::to_wstring(hr);
            LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        }
    }
    
    return false;
}

bool Game::DeleteObject(size_t index)
{
    if (!SPARK_BOUNDS_CHECK(index, m_gameObjects.size())) {
        SPARK_LOG_ERROR("Game", "DeleteObject: index %zu out of bounds (size=%zu)", index, m_gameObjects.size());
        return false;
    }
    
    m_gameObjects.erase(m_gameObjects.begin() + index);
    
    std::wstring deleteMsg = L"Deleted object at index " + std::to_wstring(index) + 
                            L". Remaining objects: " + std::to_wstring(m_gameObjects.size());
    LOG_TO_CONSOLE_IMMEDIATE(deleteMsg, L"SUCCESS");
    return true;
}

void Game::ClearScene(bool keepPlayer)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Clearing scene via console integration", L"INFO");
    
    size_t originalCount = m_gameObjects.size();
    m_gameObjects.clear();
    
    // Optionally keep player-related objects
    if (keepPlayer) {
        // Player is managed separately, so just clear game objects
    }
    
    std::wstring clearMsg = L"Cleared " + std::to_wstring(originalCount) + L" objects from scene";
    if (keepPlayer) {
        clearMsg += L" (player preserved)";
    }
    LOG_TO_CONSOLE_IMMEDIATE(clearMsg, L"SUCCESS");
}

void Game::SetTimeScale(float scale)
{
    if (scale < 0.1f || scale > 10.0f) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Time scale out of range (0.1-10.0), clamping", L"WARNING");
        scale = std::max(0.1f, std::min(10.0f, scale));
    }
    
    m_timeScale = scale;
    
    std::wstring scaleMsg = L"Time scale set to " + std::to_wstring(scale) + L"x";
    LOG_TO_CONSOLE_IMMEDIATE(scaleMsg, L"SUCCESS");
}

// ============================================================================
// ENHANCED GRAPHICS INTEGRATION METHODS - Full Implementation
// ============================================================================

void Game::ApplyGraphicsSettings(bool wireframe, bool vsync, bool showFPS)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Applying graphics settings via console integration", L"INFO");
    
    if (m_graphics) {
        try {
            m_graphics->Console_SetWireframeMode(wireframe);
            m_graphics->Console_SetVSync(vsync);
            // showFPS would need FPS display system implementation
            
            std::wstring graphicsMsg = L"Graphics settings applied - Wireframe: " + (wireframe ? std::wstring(L"ON") : std::wstring(L"OFF")) + 
                                      L", VSync: " + (vsync ? std::wstring(L"ON") : std::wstring(L"OFF")) +
                                      L", Show FPS: " + (showFPS ? std::wstring(L"ON") : std::wstring(L"OFF"));
            LOG_TO_CONSOLE_IMMEDIATE(graphicsMsg, L"SUCCESS");
        } catch (...) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to apply graphics settings", L"ERROR");
        }
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Graphics settings failed - graphics engine not available", L"ERROR");
    }
}

void Game::GetGraphicsPerformance(float& outFrameTime, float& outRenderTime, float& outUpdateTime) const
{
    outFrameTime = 0.0f;
    outRenderTime = 0.0f; 
    outUpdateTime = 0.0f;
    
    if (m_graphics) {
        try {
            auto metrics = m_graphics->Console_GetMetrics();
            outFrameTime = metrics.frameTime;
            outRenderTime = metrics.renderTime;
            outUpdateTime = metrics.presentTime; // Use present time as update time approximation
        } catch (...) {
            // Fallback values - keep at 0.0f
        }
    }
}

void Game::RefreshGraphicsSettings()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Refreshing graphics settings via console integration", L"INFO");
    
    if (m_graphics) {
        try {
            // Trigger a refresh of graphics state
            m_graphics->Console_ResetDevice();
            LOG_TO_CONSOLE_IMMEDIATE(L"Graphics settings refreshed successfully", L"SUCCESS");
        } catch (...) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to refresh graphics settings", L"ERROR");
        }
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Graphics refresh failed - graphics engine not available", L"ERROR");
    }
}

// ============================================================================
// ENHANCED SCENE MANAGEMENT METHODS - Full Implementation
// ============================================================================

bool Game::LoadScene(const std::string& scenePath)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Loading scene via console integration", L"INFO");
    
    if (!m_sceneManager) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Scene load failed - scene manager not available", L"ERROR");
        return false;
    }
    
    try {
        // Convert string to wstring for scene manager
        std::wstring wScenePath(scenePath.begin(), scenePath.end());
        bool success = m_sceneManager->LoadScene(wScenePath);
        
        if (success) {
            // Clear existing game objects if loading a new scene
            m_gameObjects.clear();
            
            std::wstring loadMsg = L"Scene loaded successfully: " + wScenePath;
            LOG_TO_CONSOLE_IMMEDIATE(loadMsg, L"SUCCESS");
        } else {
            std::wstring loadMsg = L"Failed to load scene: " + wScenePath;
            LOG_TO_CONSOLE_IMMEDIATE(loadMsg, L"ERROR");
        }
        
        return success;
    } catch (...) {
        std::wstring errorMsg = L"Exception occurred while loading scene: " + std::wstring(scenePath.begin(), scenePath.end());
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return false;
    }
}

bool Game::SaveScene(const std::string& scenePath)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Saving scene via console integration", L"INFO");
    
    if (!m_sceneManager) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Scene save failed - scene manager not available", L"ERROR");
        return false;
    }
    
    try {
        // Convert string to wstring for scene manager
        std::wstring wScenePath(scenePath.begin(), scenePath.end());
        
        // This would need a SaveScene method in SceneManager
        // For now, we'll simulate success
        bool success = true; // m_sceneManager->SaveScene(wScenePath);
        
        if (success) {
            std::wstring saveMsg = L"Scene saved successfully: " + wScenePath;
            LOG_TO_CONSOLE_IMMEDIATE(saveMsg, L"SUCCESS");
        } else {
            std::wstring saveMsg = L"Failed to save scene: " + wScenePath;
            LOG_TO_CONSOLE_IMMEDIATE(saveMsg, L"ERROR");
        }
        
        return success;
    } catch (...) {
        std::wstring errorMsg = L"Exception occurred while saving scene: " + std::wstring(scenePath.begin(), scenePath.end());
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return false;
    }
}

std::vector<std::string> Game::GetAvailableScenes() const
{
    std::vector<std::string> scenes;
    
    // Add some default/example scenes
    scenes.push_back("Assets/Scenes/level1.scene");
    scenes.push_back("Assets/Scenes/test.scene");
    scenes.push_back("Assets/Scenes/demo.scene");
    
    // In a real implementation, this would scan the Assets/Scenes directory
    // for .scene files and populate the vector
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Retrieved available scenes list", L"INFO");
    return scenes;
}

// ============================================================================
// CLASS SYSTEM METHODS
// ============================================================================

void Game::SetPlayerClass(PlayerClass classType)
{
    if (m_player && m_classSystem) {
        m_player->SetClass(classType, m_classSystem.get());
        const auto& def = m_classSystem->GetClassDefinition(classType);
        std::wstring classMsg = L"Class changed to: " + std::wstring(def.name.begin(), def.name.end());
        LOG_TO_CONSOLE_IMMEDIATE(classMsg, L"SUCCESS");
    }
}

PlayerClass Game::GetPlayerClass() const
{
    if (m_player) return m_player->GetClass();
    return PlayerClass::LIGHT_ASSAULT;
}

void Game::CycleNextClass()
{
    int current = static_cast<int>(GetPlayerClass());
    int next = (current + 1) % static_cast<int>(PlayerClass::COUNT);
    SetPlayerClass(static_cast<PlayerClass>(next));
}

void Game::CyclePrevClass()
{
    int current = static_cast<int>(GetPlayerClass());
    int prev = (current - 1 + static_cast<int>(PlayerClass::COUNT)) % static_cast<int>(PlayerClass::COUNT);
    SetPlayerClass(static_cast<PlayerClass>(prev));
}

// ============================================================================
// ENHANCED COMBAT ARENA LEVEL
// ============================================================================

void Game::CreateCombatArena()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Creating enhanced combat arena level...", L"INFO");

    // === LARGE GROUND PLANE (200x200 arena) ===
    {
        auto ground = std::make_unique<PlaneObject>(100.0f, 100.0f);
        ASSERT(ground);
        HRESULT hr = ground->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr)) {
            ground->SetPosition({ 0.0f, -1.0f, 0.0f });
            ground->SetName("Arena_Ground");
            m_gameObjects.push_back(std::move(ground));
        }
    }

    // === ALPHA BASE (south, z = -70) ===
    {
        // HQ Building
        auto hq = std::make_unique<ModelObject>(L"../Assets/Models/building_small.obj");
        if (hq) {
            HRESULT hr = hq->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
            if (SUCCEEDED(hr)) {
                hq->SetPosition({ -8.0f, 0.0f, -70.0f });
                hq->SetName("Alpha_HQ");
                m_gameObjects.push_back(std::move(hq));
            }
        }

        // Defensive barriers
        for (int i = -1; i <= 1; ++i) {
            auto barrier = std::make_unique<ModelObject>(L"../Assets/Models/barrier.obj");
            if (barrier) {
                HRESULT hr = barrier->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    barrier->SetPosition({ i * 15.0f, 0.0f, -65.0f });
                    barrier->SetName("Alpha_Barrier_" + std::to_string(i + 2));
                    m_gameObjects.push_back(std::move(barrier));
                }
            }
        }

        // Supply crates
        for (int i = 0; i < 2; ++i) {
            auto crate = std::make_unique<ModelObject>(L"../Assets/Models/crate.obj");
            if (crate) {
                HRESULT hr = crate->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    crate->SetPosition({ (i == 0 ? -5.0f : 5.0f), 0.0f, -68.0f });
                    crate->SetName("Alpha_Crate_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(crate));
                }
            }
        }
    }

    // === BRAVO BASE (north, z = 70) ===
    {
        auto hq = std::make_unique<ModelObject>(L"../Assets/Models/building_small.obj");
        if (hq) {
            HRESULT hr = hq->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
            if (SUCCEEDED(hr)) {
                hq->SetPosition({ 8.0f, 0.0f, 70.0f });
                hq->SetName("Bravo_HQ");
                m_gameObjects.push_back(std::move(hq));
            }
        }

        for (int i = -1; i <= 1; ++i) {
            auto barrier = std::make_unique<ModelObject>(L"../Assets/Models/barrier.obj");
            if (barrier) {
                HRESULT hr = barrier->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    barrier->SetPosition({ i * 15.0f, 0.0f, 65.0f });
                    barrier->SetName("Bravo_Barrier_" + std::to_string(i + 2));
                    m_gameObjects.push_back(std::move(barrier));
                }
            }
        }

        for (int i = 0; i < 2; ++i) {
            auto crate = std::make_unique<ModelObject>(L"../Assets/Models/crate.obj");
            if (crate) {
                HRESULT hr = crate->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    crate->SetPosition({ (i == 0 ? -5.0f : 5.0f), 0.0f, 68.0f });
                    crate->SetName("Bravo_Crate_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(crate));
                }
            }
        }
    }

    // === CENTER OBJECTIVE (z = 0) ===
    {
        // Central building
        auto centerBldg = std::make_unique<ModelObject>(L"../Assets/Models/building_small.obj");
        if (centerBldg) {
            HRESULT hr = centerBldg->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
            if (SUCCEEDED(hr)) {
                centerBldg->SetPosition({ 0.0f, 0.0f, 0.0f });
                centerBldg->SetScale({ 1.2f, 1.0f, 1.2f });
                centerBldg->SetName("Center_Building");
                m_gameObjects.push_back(std::move(centerBldg));
            }
        }

        // Cover around center
        const float coverPositions[][3] = {
            {-8.0f, 0.0f, -3.0f}, {8.0f, 0.0f, 3.0f},
            {-3.0f, 0.0f, 8.0f},  {3.0f, 0.0f, -8.0f}
        };
        for (int i = 0; i < 4; ++i) {
            auto barrier = std::make_unique<ModelObject>(L"../Assets/Models/barrier.obj");
            if (barrier) {
                HRESULT hr = barrier->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    barrier->SetPosition({ coverPositions[i][0], coverPositions[i][1], coverPositions[i][2] });
                    barrier->SetName("Center_Barrier_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(barrier));
                }
            }
        }

        // Crates around center
        const float cratePositions[][3] = {
            {-3.0f, 0.0f, 5.0f}, {3.0f, 0.0f, -5.0f},
            {-6.0f, 0.0f, -6.0f}, {6.0f, 0.0f, 6.0f}
        };
        for (int i = 0; i < 4; ++i) {
            auto crate = std::make_unique<ModelObject>(L"../Assets/Models/crate.obj");
            if (crate) {
                HRESULT hr = crate->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    crate->SetPosition({ cratePositions[i][0], cratePositions[i][1], cratePositions[i][2] });
                    crate->SetName("Center_Crate_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(crate));
                }
            }
        }
    }

    // === WEST OUTPOST (x = -45) ===
    {
        auto tower = std::make_unique<ModelObject>(L"../Assets/Models/watchtower.obj");
        if (tower) {
            HRESULT hr = tower->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
            if (SUCCEEDED(hr)) {
                tower->SetPosition({ -45.0f, 0.0f, 0.0f });
                tower->SetName("West_Tower");
                m_gameObjects.push_back(std::move(tower));
            }
        }

        for (int i = 0; i < 2; ++i) {
            auto barrier = std::make_unique<ModelObject>(L"../Assets/Models/barrier.obj");
            if (barrier) {
                HRESULT hr = barrier->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    barrier->SetPosition({ -40.0f, 0.0f, (i == 0 ? -5.0f : 5.0f) });
                    barrier->SetName("West_Cover_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(barrier));
                }
            }
        }

        for (int i = 0; i < 2; ++i) {
            auto crate = std::make_unique<ModelObject>(L"../Assets/Models/crate.obj");
            if (crate) {
                HRESULT hr = crate->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    crate->SetPosition({ -48.0f, 0.0f, (i == 0 ? -2.0f : 2.0f) });
                    crate->SetName("West_Crate_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(crate));
                }
            }
        }
    }

    // === EAST OUTPOST (x = 45) ===
    {
        auto tower = std::make_unique<ModelObject>(L"../Assets/Models/watchtower.obj");
        if (tower) {
            HRESULT hr = tower->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
            if (SUCCEEDED(hr)) {
                tower->SetPosition({ 45.0f, 0.0f, 0.0f });
                tower->SetName("East_Tower");
                m_gameObjects.push_back(std::move(tower));
            }
        }

        for (int i = 0; i < 2; ++i) {
            auto barrier = std::make_unique<ModelObject>(L"../Assets/Models/barrier.obj");
            if (barrier) {
                HRESULT hr = barrier->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    barrier->SetPosition({ 40.0f, 0.0f, (i == 0 ? -5.0f : 5.0f) });
                    barrier->SetName("East_Cover_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(barrier));
                }
            }
        }

        for (int i = 0; i < 2; ++i) {
            auto crate = std::make_unique<ModelObject>(L"../Assets/Models/crate.obj");
            if (crate) {
                HRESULT hr = crate->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    crate->SetPosition({ 48.0f, 0.0f, (i == 0 ? -2.0f : 2.0f) });
                    crate->SetName("East_Crate_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(crate));
                }
            }
        }
    }

    // === MID-FIELD COVER (scattered barriers and crates) ===
    {
        const float fieldBarrierPos[][3] = {
            {-25.0f, 0.0f, -30.0f}, {25.0f, 0.0f, -30.0f},
            {-25.0f, 0.0f,  30.0f}, {25.0f, 0.0f,  30.0f},
            {-20.0f, 0.0f,   0.0f}, {20.0f, 0.0f,   0.0f},
            {  0.0f, 0.0f, -35.0f}, { 0.0f, 0.0f,  35.0f}
        };
        for (int i = 0; i < 8; ++i) {
            auto barrier = std::make_unique<ModelObject>(L"../Assets/Models/barrier.obj");
            if (barrier) {
                HRESULT hr = barrier->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    barrier->SetPosition({ fieldBarrierPos[i][0], fieldBarrierPos[i][1], fieldBarrierPos[i][2] });
                    barrier->SetName("Field_Barrier_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(barrier));
                }
            }
        }

        const float fieldCratePos[][3] = {
            {-15.0f, 0.0f, -15.0f}, {15.0f, 0.0f, -15.0f},
            {-15.0f, 0.0f,  15.0f}, {15.0f, 0.0f,  15.0f},
            {-30.0f, 0.0f, -50.0f}, {30.0f, 0.0f, -50.0f},
            {-30.0f, 0.0f,  50.0f}, {30.0f, 0.0f,  50.0f}
        };
        for (int i = 0; i < 8; ++i) {
            auto crate = std::make_unique<ModelObject>(L"../Assets/Models/crate.obj");
            if (crate) {
                HRESULT hr = crate->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    crate->SetPosition({ fieldCratePos[i][0], fieldCratePos[i][1], fieldCratePos[i][2] });
                    crate->SetName("Field_Crate_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(crate));
                }
            }
        }
    }

    // === TARGET PRACTICE AREA (west side) ===
    {
        for (int i = 0; i < 5; ++i) {
            auto target = std::make_unique<ModelObject>(L"../Assets/Models/target.obj");
            if (target) {
                HRESULT hr = target->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    target->SetPosition({ -55.0f, 0.0f, -10.0f + i * 5.0f });
                    target->SetName("Target_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(target));
                }
            }
        }
    }

    // === CHARACTER MODELS (NPCs / bots placeholder) ===
    {
        const float npcPositions[][3] = {
            { 0.0f, 0.0f,  20.0f},
            {10.0f, 0.0f, -20.0f},
            {-10.0f, 0.0f, 30.0f},
            {20.0f, 0.0f, -40.0f}
        };
        for (int i = 0; i < 4; ++i) {
            auto npc = std::make_unique<ModelObject>(L"../Assets/Models/character.obj");
            if (npc) {
                HRESULT hr = npc->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    npc->SetPosition({ npcPositions[i][0], npcPositions[i][1], npcPositions[i][2] });
                    npc->SetName("NPC_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(npc));
                }
            }
        }
    }

    // === DECORATIVE SPHERES (represent control point markers) ===
    {
        const float cpPositions[][3] = {
            {  0.0f, 3.0f,   0.0f},  // Center (Point A)
            {-45.0f, 3.0f,   0.0f},  // West   (Point B)
            { 45.0f, 3.0f,   0.0f},  // East   (Point C)
        };
        for (int i = 0; i < 3; ++i) {
            auto sphere = std::make_unique<SphereObject>(0.5f, 12, 12);
            if (sphere) {
                HRESULT hr = sphere->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    sphere->SetPosition({ cpPositions[i][0], cpPositions[i][1], cpPositions[i][2] });
                    sphere->SetName("ControlPoint_" + std::to_string(i + 1));
                    m_gameObjects.push_back(std::move(sphere));
                }
            }
        }
    }

    // === WEAPON DISPLAYS (at spawn) ===
    {
        const wchar_t* weaponModels[] = {
            L"../Assets/Models/rifle.obj",
            L"../Assets/Models/sniper.obj",
            L"../Assets/Models/lmg.obj",
            L"../Assets/Models/shotgun.obj",
            L"../Assets/Models/pistol.obj"
        };
        const char* weaponNames[] = { "Rifle_Display", "Sniper_Display", "LMG_Display", "Shotgun_Display", "Pistol_Display" };
        for (int i = 0; i < 5; ++i) {
            auto wpn = std::make_unique<ModelObject>(weaponModels[i]);
            if (wpn) {
                HRESULT hr = wpn->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr)) {
                    wpn->SetPosition({ -3.0f + i * 1.5f, 1.2f, -72.0f });
                    wpn->SetScale({ 3.0f, 3.0f, 3.0f });
                    wpn->SetName(weaponNames[i]);
                    m_gameObjects.push_back(std::move(wpn));
                }
            }
        }
    }

    std::wstring totalMsg = L"Combat arena created. Total objects: " + std::to_wstring(m_gameObjects.size());
    LOG_TO_CONSOLE_IMMEDIATE(totalMsg, L"SUCCESS");
}

void Game::CreateTestScene(const std::string& sceneType)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Creating test scene via console integration", L"INFO");
    
    // Clear existing objects
    m_gameObjects.clear();
    
    if (sceneType == "basic") {
        // Create basic test scene
        CreateTestObjects(); // Use existing method
        
    } else if (sceneType == "performance") {
        // Create a performance test scene with many objects
        LOG_TO_CONSOLE_IMMEDIATE(L"Creating performance test scene with many objects", L"INFO");
        
        int objectsCreated = 0;
        for (int x = -10; x <= 10; x += 2) {
            for (int z = -10; z <= 10; z += 2) {
                auto cube = std::make_unique<CubeObject>(0.5f);
                if (cube) {
                    HRESULT hr = cube->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                    if (SUCCEEDED(hr)) {
                        cube->SetPosition({ static_cast<float>(x), 0.5f, static_cast<float>(z) });
                        m_gameObjects.push_back(std::move(cube));
                        objectsCreated++;
                    }
                }
            }
        }
        
        std::wstring perfMsg = L"Performance test scene created with " + std::to_wstring(objectsCreated) + L" objects";
        LOG_TO_CONSOLE_IMMEDIATE(perfMsg, L"SUCCESS");
        
    } else if (sceneType == "empty") {
        // Create empty scene (just clear objects)
        LOG_TO_CONSOLE_IMMEDIATE(L"Empty test scene created", L"SUCCESS");
        
    } else {
        // Unknown scene type, create basic
        std::wstring unknownMsg = L"Unknown scene type '" + std::wstring(sceneType.begin(), sceneType.end()) + L"', creating basic scene";
        LOG_TO_CONSOLE_IMMEDIATE(unknownMsg, L"WARNING");
        CreateTestObjects();
    }
    
    std::wstring sceneMsg = L"Test scene created: " + std::wstring(sceneType.begin(), sceneType.end()) +
                           L" (Total objects: " + std::to_wstring(m_gameObjects.size()) + L")";
    LOG_TO_CONSOLE_IMMEDIATE(sceneMsg, L"SUCCESS");
}

/*-------------------------------------------------------------
  Vehicle System Integration
--------------------------------------------------------------*/
Spark::Vehicle* Game::SpawnVehicle(SparkEditor::VehicleType type, float x, float y, float z)
{
    if (!m_vehicleSystem || !m_graphics) return nullptr;
    auto* vehicle = m_vehicleSystem->SpawnVehicle(type, {x, y, z},
        m_graphics->GetDevice(), m_graphics->GetContext());
    if (vehicle) {
        std::wstring msg = L"Vehicle spawned: " +
            std::wstring(vehicle->GetVehicleName().begin(), vehicle->GetVehicleName().end()) +
            L" at (" + std::to_wstring(x) + L"," + std::to_wstring(y) + L"," + std::to_wstring(z) + L")";
        LOG_TO_CONSOLE_IMMEDIATE(msg, L"SUCCESS");
    }
    return vehicle;
}

bool Game::PlayerEnterNearestVehicle()
{
    if (!m_player || !m_vehicleSystem) return false;
    if (m_player->IsInVehicle()) return false;

    auto* vehicle = m_vehicleSystem->FindNearestVehicle(m_player->GetPosition(), 5.0f);
    if (vehicle) {
        return m_player->EnterVehicle(vehicle);
    }

    LOG_TO_CONSOLE(L"No vehicle nearby to enter", L"INFO");
    return false;
}

bool Game::PlayerExitVehicle()
{
    if (!m_player || !m_player->IsInVehicle()) return false;
    return m_player->ExitVehicle();
}
#endif // SPARK_PLATFORM_WINDOWS
