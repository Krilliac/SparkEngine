#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cstdint>
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <chrono>

#include "Game.h"
#include "ClassSystem.h"
#include "Core/FaultIsolation.h"
#include "Utils/Assert.h"
#include "Utils/SparkError.h"
#include "Utils/Validate.h"
#include "Utils/SparkConsole.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/TextureSystem.h"
#include "Graphics/AssetPipeline.h"
#include "Physics/PhysicsSystem.h"
#include "Input/InputManager.h"
#include "Camera/SparkEngineCamera.h"
#include "Game/GameObject.h"
#include "CubeObject.h"
#include "PlaneObject.h"
#include "SphereObject.h"
#include "WallObject.h"
#include "ModelObject.h"
#include "Player.h"
#include "Game/Console.h"
#include "Projectiles/ProjectilePool.h"
#include "SceneManager/SceneManager.h"
#include "Utils/SparkConsole.h"
#include "Console/AdvancedConsoleCommands.h"
#include "Engine/Events/EventSystem.h"
#include "Audio/MusicManager.h"
#include "Graphics/WeatherSystem.h"
#include "Engine/Cinematic/Sequencer.h"
#include "Engine/Replay/ReplaySystem.h"
#include <filesystem>

// Pull in game-specific globals defined in Main.cpp (SparkGame entry point)
extern Console g_console;

// Centralized logging macros (previously defined locally with inconsistent rate limits)
#include "Utils/LogMacros.h"

using namespace DirectX;
/*-------------------------------------------------------------
  Ctor / Dtor
--------------------------------------------------------------*/
Game::Game()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Game constructor called.", L"INFO");
}
Game::~Game()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Game destructor called.", L"INFO");
    Shutdown();
}

/*-------------------------------------------------------------
  Initialise all game-side systems
--------------------------------------------------------------*/
HRESULT Game::Initialize(GraphicsEngine* graphics, InputManager* input)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Game::Initialize called");
    LOG_TO_CONSOLE_IMMEDIATE(L"Game::Initialize called.", L"INFO");

    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, input);

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

    UINT winHeight = m_graphics->GetWindowHeight();
    float aspect = (winHeight > 0) ? float(m_graphics->GetWindowWidth()) / float(winHeight)
                                   : 16.0f / 9.0f; // Safe fallback if window is minimized
    ASSERT_MSG(aspect > 0.0f, "Invalid aspect ratio");

    m_camera->Initialize(aspect);
    // NOTE: Camera position is now defined in the scene file (Assets/Scenes/level1.scene)
    // as a [Camera] entry. It can be placed and edited in the SparkEditor.
    // The code below shows the equivalent C++ approach for reference.
    // m_camera->SetPosition({0.0f, 2.0f, -5.0f});
    m_camera->SetPosition({0.0f, 2.0f, -5.0f}); // Fallback if scene camera not loaded
    LOG_TO_CONSOLE_IMMEDIATE(L"Camera initialized (scene override available)", L"INFO");

    /* Class System -----------------------------------------*/
    m_classSystem = std::make_unique<Spark::ClassSystem>();
    ASSERT(m_classSystem);
    m_classSystem->Initialize();
    LOG_TO_CONSOLE_IMMEDIATE(L"Class system initialized with 6 classes", L"SUCCESS");

    /* Player -----------------------------------------------*/
    m_player = std::make_unique<Player>();
    ASSERT(m_player);

    HRESULT hr = m_player->Initialize(m_graphics->GetDevice(), m_graphics->GetContext(), m_camera.get(), m_input);
    ASSERT_MSG(SUCCEEDED(hr), "Player::Initialize failed");
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Player initialization failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Set graphics engine for weapon rendering shader setup
    m_player->SetGraphicsEngine(m_graphics);

    // Set default class (Scout)
    m_player->SetClass(PlayerClass::SCOUT, m_classSystem.get());
    LOG_TO_CONSOLE_IMMEDIATE(L"Player class set to Scout (default)", L"SUCCESS");

    /* Projectile pool --------------------------------------*/
    m_projectilePool = std::make_unique<ProjectilePool>(100);
    ASSERT(m_projectilePool);

    hr = m_projectilePool->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
    ASSERT_MSG(SUCCEEDED(hr), "ProjectilePool::Initialize failed");
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"ProjectilePool initialization failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    /* Scene objects - Enhanced combat arena ----------------*/
    CreateCombatArena();

    // Wire up graphics engine on all ModelObjects so they don't need the global
    for (auto& obj : m_gameObjects)
    {
        if (auto* mo = dynamic_cast<ModelObject*>(obj.get()))
            mo->SetGraphicsEngine(m_graphics);
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Game initialization complete - class system & combat arena ready", L"SUCCESS");

    /* Vehicle System -----------------------------------*/
    m_vehicleSystem = std::make_unique<Spark::VehicleSystem>();
    m_vehicleSystem->Initialize();
    if (m_projectilePool)
        m_vehicleSystem->SetProjectilePool(m_projectilePool.get());
    LOG_TO_CONSOLE_IMMEDIATE(L"Vehicle system initialized (9 vehicle types, weapons armed)", L"SUCCESS");

    /* Gravity System -----------------------------------*/
    m_gravitySystem = std::make_unique<Spark::GravitySystem>();
    m_gravitySystem->Initialize({0, -20.0f, 0});
    m_player->SetGravitySystem(m_gravitySystem.get());

    // NOTE: Gravity zones are now defined in the scene file (Assets/Scenes/level1.scene)
    // as [ForceRegion] entries. They can be placed and edited in the SparkEditor without
    // recompiling. The code below shows the equivalent C++ approach for reference.
    //
    // m_gravitySystem->CreateLowGravityZone("LowG_Platform", {20.0f, 5.0f, 20.0f},
    //                                       {8.0f, 8.0f, 8.0f}, -5.0f);
    // m_gravitySystem->CreateZeroGravityZone("ZeroG_Corridor", {0.0f, 15.0f, 30.0f},
    //                                        {5.0f, 5.0f, 15.0f});
    // m_gravitySystem->CreateReverseGravityZone("Reverse_Chamber", {-25.0f, 10.0f, 0.0f},
    //                                           {6.0f, 10.0f, 6.0f}, 12.0f);

    LOG_TO_CONSOLE_IMMEDIATE(L"Gravity zones loaded from scene file", L"SUCCESS");

    InitializeInteractionObjects();
    InitializeRespawnAndVehicles();
    InitializeGameModeAndHUD();
    InitializeInventorySystem();
    InitializeQuestSystem();
    InitializeEnemies();
    InitializeGameplaySystems();

    /* Register Advanced Console Commands */
    SparkConsole::RegisterAdvancedCommands(this, m_graphics);

    /* Engine system integration — audio, weather, destruction, dialogue, save */
    InitializeEngineSystems();

    LOG_TO_CONSOLE_IMMEDIATE(L"All systems online - gamemode, HUD, inventory, quests, vehicles, gravity, interactions, "
                             L"damage zones, respawn, audio, weather, destruction, dialogue, save",
                             L"SUCCESS");

    return S_OK;
}

/*-------------------------------------------------------------
  Tear-down
--------------------------------------------------------------*/
void Game::Shutdown()
{
    if (m_isShutDown)
        return;
    m_isShutDown = true;

    LOG_TO_CONSOLE_IMMEDIATE(L"Game::Shutdown called.", L"INFO");

    m_gameObjects.clear();
    m_lootSystem.reset();
    m_progression.reset();
    m_waveSpawner.reset();
    m_hudSystem.reset();
    m_gameMode.reset();
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
    m_eventBus = nullptr;

#ifdef ENABLE_NETWORKING
    if (m_networkInitialized)
    {
        Spark::Net::NetworkManager::GetInstance().Shutdown();
        m_networkInitialized = false;
    }
#endif

    LOG_TO_CONSOLE_IMMEDIATE(L"Game shutdown complete - all systems cleaned up.", L"INFO");
}

/*-------------------------------------------------------------
  Physics system wiring — propagates to all projectile pools
--------------------------------------------------------------*/
void Game::SetPhysicsSystem(PhysicsSystem* ps)
{
    if (m_projectilePool)
        m_projectilePool->SetPhysicsSystem(ps);
    // Player may own a separate projectile pool
    if (m_player && m_player->GetProjectilePool())
        m_player->GetProjectilePool()->SetPhysicsSystem(ps);
}

/*-------------------------------------------------------------
  EventBus wiring — connects cross-system event subscriptions
--------------------------------------------------------------*/
void Game::SetEventBus(Spark::EventBus* bus)
{
    m_eventBus = bus;
    if (!bus)
        return;

    // Entity killed → update gamemode scoring, quest progress, and HUD kill feed
    (void)bus->Subscribe<Spark::EntityKilledEvent>(
        [this](const Spark::EntityKilledEvent& e)
        {
            if (m_gameMode)
                m_gameMode->RecordKill("Player1", "Enemy");

            // Show hit marker and add kill feed entry on HUD
            if (m_hudSystem)
            {
                m_hudSystem->ShowHitMarker(false);
                m_hudSystem->AddKillFeedEntry("Player1", "Enemy", e.cause);
            }

            // Progress kill-based quest objectives
            Spark::QuestOps::UpdateObjective(m_playerQuests, m_questRegistry, 1, 0, 1, m_eventBus, 0);
        });

    // Entity damaged → show damage indicator on HUD
    (void)bus->Subscribe<Spark::EntityDamagedEvent>(
        [this](const Spark::EntityDamagedEvent& e)
        {
            if (m_hudSystem && m_player)
            {
                // Normalize damage to 0-1 intensity range (assume 100 HP max)
                float intensity = std::min(e.damage / 100.0f, 1.0f);
                // Use a default forward angle since we don't have source position
                m_hudSystem->AddDamageIndicator(0.0f, intensity);
            }
        });

    // Item pickup → add to player inventory
    (void)bus->Subscribe<Spark::ItemPickedUpEvent>(
        [this](const Spark::ItemPickedUpEvent& e)
        { Spark::InventoryOps::AddItem(m_playerInventory, m_itemRegistry, e.itemDefId, e.count); });

    // Player respawn → teleport to spawn point and reset HUD state
    (void)bus->Subscribe<Spark::PlayerRespawnEvent>(
        [this](const Spark::PlayerRespawnEvent& e)
        {
            // Teleport camera/player to spawn location
            if (m_camera)
            {
                m_camera->Console_SetPosition(e.spawnX, e.spawnY, e.spawnZ);
            }
            if (m_player)
            {
                m_player->Console_SetPosition(e.spawnX, e.spawnY, e.spawnZ);
            }

            // Reset HUD damage indicators on respawn
            if (m_hudSystem)
            {
                m_hudSystem->ShowHitMarker(false); // Clear any lingering hit marker
            }

            std::wstring msg = L"Player respawned at (" + std::to_wstring(e.spawnX) + L", " +
                               std::to_wstring(e.spawnY) + L", " + std::to_wstring(e.spawnZ) + L")";
            LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
        });

    LOG_TO_CONSOLE_IMMEDIATE(L"EventBus connected - cross-system events wired", L"SUCCESS");
}

/*-------------------------------------------------------------
  Per-frame update
--------------------------------------------------------------*/
void Game::Update(float dt)
{
    if (m_isPaused)
    {
        return;
    }

    // Validate delta time to prevent physics explosions from bad frames
    if (!std::isfinite(dt) || dt < 0.0f)
    {
        SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Warn, "Game", 5, "Invalid deltaTime %.6f -- clamping to 0", dt);
        dt = 0.0f;
    }
    if (dt > 0.25f)
    {
        SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Warn, "Game", 5,
                                "Large deltaTime %.4fs (>250ms) -- clamping to 250ms to prevent physics instability",
                                dt);
        dt = 0.25f;
    }

    dt *= m_timeScale;

    SPARK_CATCH_ALL("Game", {
        HandleInput(dt);
        UpdateCamera(dt);
        UpdateGameObjects(dt);
    });

    SPARK_GUARDED_UPDATE("Game:ClassSystem", "Game", {
        if (m_classSystem)
            m_classSystem->Update(dt);
    });
    SPARK_GUARDED_UPDATE("Game:Player", "Game", {
        if (m_player)
            m_player->Update(dt);
    });
    SPARK_GUARDED_UPDATE("Game:Projectiles", "Game", {
        if (m_projectilePool)
            m_projectilePool->Update(dt);
    });

    // New systems
    SPARK_GUARDED_UPDATE("Game:Vehicles", "Game", {
        if (m_vehicleSystem)
            m_vehicleSystem->Update(dt);
        if (m_gravitySystem)
            m_gravitySystem->Update(dt);
    });
    SPARK_GUARDED_UPDATE("Game:Interaction", "Game", {
        if (m_interactionSystem)
            m_interactionSystem->Update(dt, m_player.get());
        if (m_damageZoneSystem)
            m_damageZoneSystem->Update(dt, m_player.get());
        if (m_respawnSystem)
            m_respawnSystem->Update(dt);
    });

    // Gameplay systems
    SPARK_GUARDED_UPDATE("Game:WaveSpawner", "Game", {
        if (m_waveSpawner)
            m_waveSpawner->Update(dt, GetAliveEnemyCount(), this);
        if (m_lootSystem)
            m_lootSystem->Update(dt, m_player.get());
    });

    // Integrated systems
    SPARK_GUARDED_UPDATE("Game:GameMode", "Game", {
        if (m_gameMode)
            m_gameMode->Update(dt);
        if (m_hudSystem)
            m_hudSystem->Update(dt);
        Spark::QuestOps::UpdateTimers(m_playerQuests, m_questRegistry, dt);
    });

    // Track play time for save metadata
    m_playTime += dt;

    // Update dynamic music intensity based on nearby enemies
    if (m_audioInitialized && m_engineContext)
    {
        if (auto* music = m_engineContext->GetMusic())
        {
            size_t aliveEnemies = GetAliveEnemyCount();
            auto intensity = (aliveEnemies > 4)   ? Spark::Audio::CombatIntensity::Combat
                             : (aliveEnemies > 0) ? Spark::Audio::CombatIntensity::LowThreat
                                                  : Spark::Audio::CombatIntensity::Exploration;
            music->SetCombatIntensity(intensity);
        }
    }

    // Cycle weather presets periodically to showcase the system
    if (m_weatherActive && m_engineContext)
    {
        m_weatherTransitionTimer += dt;
        if (m_weatherTransitionTimer > 120.0f) // Every 2 minutes
        {
            m_weatherTransitionTimer = 0.0f;
            if (auto* weather = m_engineContext->GetWeather())
            {
                // Cycle: Clear → Rain → Fog → Storm → Clear
                static int weatherCycle = 0;
                constexpr Spark::WeatherType cycle[] = {
                    Spark::WeatherType::Rain,
                    Spark::WeatherType::Fog,
                    Spark::WeatherType::Storm,
                    Spark::WeatherType::Clear,
                };
                weather->SetWeather(cycle[weatherCycle % 4], 0.8f, 5.0f);
                weatherCycle++;
            }
        }
    }

    // Update cinematic sequencer via context
    SPARK_GUARDED_UPDATE("Game:Cinematic", "Game", {
        if (m_engineContext)
        {
            if (auto* cinematic = m_engineContext->GetCinematic())
                cinematic->Update(dt);

            if (auto* replay = m_engineContext->GetReplay())
            {
                if (replay->GetPlaybackState() == Spark::PlaybackState::Playing)
                    replay->UpdatePlayback(dt);
            }
        }
    });

#ifdef ENABLE_NETWORKING
    // Update networking - process incoming messages, send outgoing state
    SPARK_GUARDED_UPDATE("Game:Networking", "Game", {
        if (m_networkInitialized)
        {
            auto& netMgr = Spark::Net::NetworkManager::GetInstance();
            netMgr.Update(dt);

            // Send the client input heartbeat for prediction/reconciliation.
            if (m_player && netMgr.GetRole() != Spark::Net::NetworkRole::None)
            {
                Spark::Net::ClientInputState inputState{};
                inputState.deltaTime = dt;
                inputState.timestamp = netMgr.GetServerTime();
                netMgr.SendClientInput(inputState);
            }
        }
    });
#endif

    // Update advanced systems through main GraphicsEngine
    SPARK_GUARDED_UPDATE("Game:Graphics", "Game", {
        if (m_graphics)
        {
            if (auto textureSystem = m_graphics->GetTextureSystem())
                textureSystem->Update(dt);
            if (auto assetPipeline = m_graphics->GetAssetPipeline())
                assetPipeline->Update(dt);
        }
    });

    // Update physics via engine context
    SPARK_GUARDED_UPDATE("Game:Physics", "Game", {
        if (m_engineContext)
        {
            if (auto* physics = m_engineContext->GetPhysics())
                physics->Update(dt);
        }
    });
}

/*-------------------------------------------------------------
  Per-frame render – called between BeginFrame / EndFrame
--------------------------------------------------------------*/
void Game::Render()
{
    // **UNIFIED RENDERING SOLUTION: Single render location for all graphics**
    if (!m_graphics)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Graphics engine not available for rendering", L"ERROR");
        return;
    }

    // **CRITICAL: This is the ONLY place BeginFrame/EndFrame should be called**
    bool frameStarted = false;
    try
    {
        m_graphics->BeginFrame();
        frameStarted = true;

        // Collect all renderable objects for unified rendering
        std::vector<GameObject*> renderableObjects;

        // Add game objects
        for (auto& obj : m_gameObjects)
        {
            if (obj && obj->IsActive() && obj->IsVisible())
            {
                renderableObjects.push_back(obj.get());
            }
        }

        // Add scene manager objects
        if (m_sceneManager)
        {
            for (auto& obj : m_sceneManager->GetObjects())
            {
                if (obj && obj->IsActive() && obj->IsVisible())
                {
                    renderableObjects.push_back(obj.get());
                }
            }
        }

        // Add vehicles
        if (m_vehicleSystem)
        {
            for (auto& vehicle : m_vehicleSystem->GetVehicles())
            {
                if (vehicle && vehicle->IsActive() && vehicle->IsVisible())
                {
                    renderableObjects.push_back(vehicle.get());
                }
            }
        }

        // Add interactive objects
        if (m_interactionSystem)
        {
            for (auto& obj : m_interactionSystem->GetObjects())
            {
                if (obj && obj->IsActive() && obj->IsVisible())
                {
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
        if (m_player)
        {
            m_player->Render(view, proj);
        }

        if (m_projectilePool)
        {
            m_projectilePool->Render(view, proj);
        }

        // **SOLUTION: Single console rendering location - this is the ONLY place console renders**
        if (g_console.IsVisible())
        {
            g_console.Render(m_graphics->GetContext());
        }
    }
    catch (const std::exception& e)
    {
        SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "Render", 5, "Rendering exception: %s", e.what());
    }
    catch (...)
    {
        SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "Render", 5, "Unknown rendering exception caught");
    }

    // Always call EndFrame exactly once if BeginFrame succeeded
    if (frameStarted)
    {
        SPARK_CATCH_ALL("Render", { m_graphics->EndFrame(); });
    }
}

/*-------------------------------------------------------------*/
void Game::UpdateCamera(float dt)
{
    // No logging for per-frame operations
    ASSERT(dt >= 0.0f);
    if (m_camera)
        m_camera->Update(dt);
}

/*-------------------------------------------------------------*/
void Game::UpdateGameObjects(float dt)
{
    // No logging for per-frame operations
    ASSERT(dt >= 0.0f);
    for (auto& obj : m_gameObjects)
        if (obj && obj->IsActive())
            obj->Update(dt);
}

/*-------------------------------------------------------------
  WASD + mouse look, zoom, and shooting input handling
--------------------------------------------------------------*/
void Game::HandleInput(float dt)
{
    // No logging for per-frame operations
    ASSERT(dt >= 0.0f);
    if (!m_input || !m_camera)
    {
        return;
    }

    auto [dx, dy] = m_input->GetMouseDelta();
    if (dx != 0 || dy != 0)
    {
        constexpr float mouseSens = 0.005f;
        m_camera->Yaw(dx * mouseSens);
        m_camera->Pitch(-dy * mouseSens);
    }

    float moveSpeed = 10.0f * dt;
    if (m_input->IsKeyDown('W'))
        m_camera->MoveForward(moveSpeed);
    if (m_input->IsKeyDown('S'))
        m_camera->MoveForward(-moveSpeed);
    if (m_input->IsKeyDown('A'))
        m_camera->MoveRight(-moveSpeed);
    if (m_input->IsKeyDown('D'))
        m_camera->MoveRight(moveSpeed);
    if (m_input->IsKeyDown(VK_SPACE))
        m_camera->MoveUp(moveSpeed);
    if (m_input->IsKeyDown(VK_LCONTROL))
        m_camera->MoveUp(-moveSpeed);

    m_camera->SetZoom(m_input->IsMouseButtonDown(1));

    if (m_input->WasMouseButtonPressed(0) && m_projectilePool)
    {
        auto pos = m_camera->GetPosition();
        auto dir = m_camera->GetForward();
        m_projectilePool->FireProjectile(ProjectileType::BULLET, pos, dir, 50.0f);
        LOG_TO_CONSOLE(L"Projectile fired", L"DEBUG");
    }

    // Class switching with F5-F10 keys
    if (m_input->WasKeyPressed(VK_F5))
        SetPlayerClass(PlayerClass::SCOUT);
    if (m_input->WasKeyPressed(VK_F6))
        SetPlayerClass(PlayerClass::MEDIC);
    if (m_input->WasKeyPressed(VK_F7))
        SetPlayerClass(PlayerClass::ENGINEER);
    if (m_input->WasKeyPressed(VK_F8))
        SetPlayerClass(PlayerClass::RECON);
    if (m_input->WasKeyPressed(VK_F9))
        SetPlayerClass(PlayerClass::VANGUARD);
    if (m_input->WasKeyPressed(VK_F10))
        SetPlayerClass(PlayerClass::TITAN);

    // Cycle classes with [ and ]
    if (m_input->WasKeyPressed(VK_OEM_4))
        CyclePrevClass(); // [ key
    if (m_input->WasKeyPressed(VK_OEM_6))
        CycleNextClass(); // ] key

    // Vehicle enter/exit with V key
    if (m_input->WasKeyPressed('V'))
    {
        if (m_player && m_player->IsInVehicle())
        {
            PlayerExitVehicle();
        }
        else
        {
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
        if (SUCCEEDED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Ground plane created successfully", L"INFO");
            ground->SetPosition({0.0f, -1.0f, 0.0f});
            m_gameObjects.push_back(std::move(ground));
        }
        else
        {
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
        if (SUCCEEDED(hr))
        {
            cube->SetPosition({i * 3.0f - 6.0f, 1.0f, 10.0f});
            m_gameObjects.push_back(std::move(cube));
            cubesCreated++;
        }
        else
        {
            std::wstring errorMsg =
                L"Cube " + std::to_wstring(i) + L" creation failed with HR=0x" + std::to_wstring(hr);
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
        if (SUCCEEDED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Sphere created successfully", L"INFO");
            sphere->SetPosition({5.0f, 0.0f, 0.0f});
            m_gameObjects.push_back(std::move(sphere));
        }
        else
        {
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
        if (SUCCEEDED(hr))
        {
            target1->SetPosition({-8.0f, 2.0f, 15.0f});
            target1->SetName("Target_1");
            m_gameObjects.push_back(std::move(target1));
            LOG_TO_CONSOLE_IMMEDIATE(L"Target 1 model created successfully", L"INFO");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Target 1 model creation failed", L"WARNING");
        }

        auto target2 = std::make_unique<ModelObject>(L"../Assets/Models/target.obj");
        ASSERT(target2);
        hr = target2->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            target2->SetPosition({8.0f, 2.0f, 15.0f});
            target2->SetName("Target_2");
            m_gameObjects.push_back(std::move(target2));
            LOG_TO_CONSOLE_IMMEDIATE(L"Target 2 model created successfully", L"INFO");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Target 2 model creation failed", L"WARNING");
        }

        // Character model for testing
        auto character = std::make_unique<ModelObject>(L"../Assets/Models/character.obj");
        ASSERT(character);
        hr = character->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            character->SetPosition({0.0f, 0.0f, 8.0f});
            character->SetName("Character_Model");
            m_gameObjects.push_back(std::move(character));
            LOG_TO_CONSOLE_IMMEDIATE(L"Character model created successfully", L"INFO");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Character model creation failed", L"WARNING");
        }

        // Weapon display (rifle on a stand)
        auto weaponDisplay = std::make_unique<ModelObject>(L"../Assets/Models/rifle.obj");
        ASSERT(weaponDisplay);
        hr = weaponDisplay->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            weaponDisplay->SetPosition({-3.0f, 1.5f, 5.0f});
            weaponDisplay->SetName("Weapon_Display");
            m_gameObjects.push_back(std::move(weaponDisplay));
            LOG_TO_CONSOLE_IMMEDIATE(L"Weapon display model created successfully", L"INFO");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Weapon display model creation failed", L"WARNING");
        }
    }

    std::wstring totalMsg =
        L"Test objects creation complete. Total: " + std::to_wstring(m_gameObjects.size()) + L" objects";
    LOG_TO_CONSOLE_IMMEDIATE(totalMsg, L"SUCCESS");
}
