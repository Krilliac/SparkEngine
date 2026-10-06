#include "Core/Platform.h"
#include "Core/FPSAssert.h"
#include "Core/FPSLog.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cstdint>
#include <cctype>
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <chrono>
#include <cstdio>

#include "Game.h"
#include "ClassSystem.h"
#include "Core/FaultIsolation.h"
#include "Utils/SparkError.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/TextureSystem.h"
#include "Graphics/AssetPipeline.h"
#include "Physics/PhysicsSystem.h"
#include "Input/InputManager.h"
#include "Camera/SparkEngineCamera.h"
#include "Game/GameObject.h"
#include "Game/CubeObject.h"
#include "Game/PlaneObject.h"
#include "Game/SphereObject.h"
#include "Game/WallObject.h"
#include "ModelObject.h"
#include "FPSAssetPaths.h"
#include "MultiplayerSystem.h"
#include "Player.h"
#include "Projectiles/ProjectilePool.h"
#include "SceneManager/SceneManager.h"
#include "Console/AdvancedConsoleCommands.h"
#include <Spark/IConsole.h>
#include "Engine/Events/EventSystem.h"
#include "Audio/MusicManager.h"
#include <cmath>
#include <filesystem>
#include <format>
#include <locale>
#include <sstream>
#include <unordered_set>

using namespace DirectX;

bool Game::ParseAuthoredFiniteFloat(const std::string& text, float& value)
{
    // Keep the old from_chars contract without relying on the floating-point
    // overload, which is unavailable on some libc++ toolchains.
    if (text.empty() || text.front() == '+' || std::isspace(static_cast<unsigned char>(text.front())))
    {
        return false;
    }
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    stream >> std::noskipws >> value;
    return !stream.fail() && stream.peek() == std::char_traits<char>::eof() && std::isfinite(value);
}

/*-------------------------------------------------------------
  Ctor / Dtor
--------------------------------------------------------------*/
Game::Game()
{
    FPS_CONSOLE("Game constructor called.", "INFO");
}
Game::~Game()
{
    FPS_CONSOLE("Game destructor called.", "INFO");
    Shutdown();
}

/*-------------------------------------------------------------
  Initialise all game-side systems
--------------------------------------------------------------*/
HRESULT Game::Initialize(GraphicsEngine* graphics, InputManager* input)
{
    FPS_LOG_INFO("Game::Initialize called");
    FPS_CONSOLE("Game::Initialize called.", "INFO");

    FPS_REQUIRE_NOT_NULL(graphics);
    FPS_REQUIRE_NOT_NULL(input);

    m_graphics = graphics;
    m_input = input;
    FPS_CONSOLE("Graphics and InputManager assigned.", "INFO");

    // A NullRHI / headless host supplies a GraphicsEngine without a D3D11 device.
    // Gameplay state is still built in full; GPU resource creation and rendering
    // are the only things skipped, so a package smoke can drive the real module.
    m_renderingEnabled = (m_graphics->GetDevice() != nullptr) && (m_graphics->GetContext() != nullptr);
    if (!m_renderingEnabled)
    {
        FPS_CONSOLE("No D3D11 device - running gameplay only (GPU resources and rendering disabled)", "WARNING");
    }

    // SceneManager setup
    m_sceneManager = std::make_unique<SceneManager>(graphics, input);
    std::filesystem::path startupScene;
    bool packagedStartup = false;
    std::string startupError;
    if (!Spark::FPSAssets::ResolveStartupScene(startupScene, packagedStartup, startupError))
    {
        FPS_LOG_ERROR("FPS packaged startup rejected: {}", startupError);
        return E_FAIL;
    }
    const std::wstring authoredScenePath =
        packagedStartup ? startupScene.wstring() : Spark::FPSAssets::Resolve(L"Scenes/level1.scene");
    bool sceneLoaded = m_sceneManager->LoadScene(authoredScenePath);
    if (packagedStartup && !sceneLoaded)
    {
        FPS_LOG_ERROR("FPS packaged startup rejected: selected reflected scene failed to load");
        return E_FAIL;
    }
    std::string sceneMsg = "SceneManager::LoadScene returned: " + std::string(sceneLoaded ? "SUCCESS" : "FAILURE");
    FPS_CONSOLE(sceneMsg, "INFO");
    LogSceneIdentity(sceneLoaded, authoredScenePath);

    // Scene material paths are authored data, but the project root is trusted
    // module state. Bind that root immediately after scene construction so
    // GameObject::Render can resolve Assets/Materials/*.json without consulting
    // the process working directory or accepting an arbitrary scene-controlled
    // filesystem root. Keep the UTF-8 conversion explicit for installed paths
    // containing non-ASCII characters.
    if (sceneLoaded)
    {
        BindSceneMaterialRoots();
    }

    /* Camera ------------------------------------------------*/
    m_camera = std::make_unique<SparkEngineCamera>();
    FPS_ASSERT(m_camera);

    UINT winHeight = m_graphics->GetWindowHeight();
    float aspect = (winHeight > 0) ? float(m_graphics->GetWindowWidth()) / float(winHeight)
                                   : 16.0f / 9.0f; // Safe fallback if window is minimized
    FPS_ASSERT_MSG(aspect > 0.0f, "Invalid aspect ratio");

    m_camera->Initialize(aspect);
    const SceneNode* authoredCamera = nullptr;
    if (sceneLoaded)
    {
        for (int i = 0; i < m_sceneManager->GetNodeCount(); ++i)
        {
            const SceneNode* node = m_sceneManager->GetNode(i);
            if (!node || node->type != "Camera")
            {
                continue;
            }
            const auto projection = node->properties.find("projection");
            if (projection != node->properties.end() && projection->second != "perspective")
            {
                FPS_CONSOLE("Unsupported authored camera projection; keeping perspective fallback", "WARNING");
                continue;
            }
            if (!authoredCamera)
            {
                authoredCamera = node;
            }
            const auto main = node->properties.find("isMain");
            if (main != node->properties.end() && (main->second == "true" || main->second == "1"))
            {
                authoredCamera = node;
                break;
            }
        }
    }
    // Preserve the verified visible fallback when older scenes have no camera.
    m_camera->SetPosition(authoredCamera ? authoredCamera->position : XMFLOAT3{0.0f, 2.0f, -20.0f});
    if (authoredCamera)
    {
        const auto fovProperty = authoredCamera->properties.find("fov");
        float authoredFov = 0.0f;
        if (fovProperty != authoredCamera->properties.end() &&
            ParseAuthoredFiniteFloat(fovProperty->second, authoredFov) && authoredFov >= 10.0f && authoredFov <= 170.0f)
        {
            m_camera->Console_SetFOV(authoredFov);
        }
        m_camera->Console_SetRotation(authoredCamera->rotation.x, authoredCamera->rotation.y,
                                      authoredCamera->rotation.z);

        const auto nearProperty = authoredCamera->properties.find("nearPlane");
        const auto farProperty = authoredCamera->properties.find("farPlane");
        if (nearProperty != authoredCamera->properties.end() && farProperty != authoredCamera->properties.end())
        {
            float nearPlane = 0.0f;
            float farPlane = 0.0f;
            if (ParseAuthoredFiniteFloat(nearProperty->second, nearPlane) &&
                ParseAuthoredFiniteFloat(farProperty->second, farPlane) && nearPlane >= 0.01f && nearPlane <= 10.0f &&
                farPlane >= 100.0f && farPlane <= 10000.0f && nearPlane < farPlane)
            {
                m_camera->Console_SetClippingPlanes(nearPlane, farPlane);
            }
            else
            {
                FPS_CONSOLE("Invalid authored camera clipping; keeping camera defaults", "WARNING");
            }
        }

        const auto cameraState = m_camera->Console_GetState();
        FPS_CONSOLE(std::format("Camera authored state: rotation ({:.1f}, {:.1f}, {:.1f}) near/far ({:.2f}, {:.1f})",
                                cameraState.rotation.x, cameraState.rotation.y, cameraState.rotation.z,
                                cameraState.nearPlane, cameraState.farPlane),
                    "INFO");
    }
    const XMFLOAT3 cameraPosition = m_camera->GetPosition();
    const std::string cameraSource = authoredCamera ? "authored scene" : "fallback";
    FPS_CONSOLE("Camera initialized from " + cameraSource + " at (" + std::to_string(cameraPosition.x) + ", " +
                    std::to_string(cameraPosition.y) + ", " + std::to_string(cameraPosition.z) + ")",
                "INFO");

    /* Class System -----------------------------------------*/
    m_classSystem = std::make_unique<Spark::ClassSystem>();
    FPS_ASSERT(m_classSystem);
    m_classSystem->Initialize();
    FPS_CONSOLE("Class system initialized with 6 classes", "SUCCESS");

    /* Projectile pool --------------------------------------*/
    // Player and vehicles share this pool so every fired projectile follows
    // the same update, render, collision, and recycling path.
    m_projectilePool = std::make_unique<ProjectilePool>(100);
    FPS_ASSERT(m_projectilePool);

    // Initialize unconditionally: on a device-less host the pool populates itself
    // without GPU meshes, so firing still spawns a real projectile. Skipping this call
    // left the injected pool empty and turned every shot into a silent no-op.
    HRESULT hr = m_projectilePool->Initialize(m_renderingEnabled ? m_graphics->GetDevice() : nullptr,
                                              m_renderingEnabled ? m_graphics->GetContext() : nullptr);
    FPS_ASSERT_MSG(SUCCEEDED(hr), "ProjectilePool::Initialize failed");
    if (FAILED(hr))
    {
        std::string errorMsg = "ProjectilePool initialization failed with HR=0x" + std::to_string(hr);
        FPS_CONSOLE(errorMsg, "ERROR");
        return hr;
    }
    FPS_ASSERT_MSG(m_projectilePool->GetAvailableCount() > 0, "ProjectilePool initialized empty - firing would no-op");

    /* Player -----------------------------------------------*/
    m_player = std::make_unique<Player>();
    FPS_ASSERT(m_player);
    m_player->SetProjectilePool(m_projectilePool.get());

    hr = m_player->Initialize(m_graphics->GetDevice(), m_graphics->GetContext(), m_camera.get(), m_input);
    FPS_ASSERT_MSG(SUCCEEDED(hr), "Player::Initialize failed");
    if (FAILED(hr))
    {
        std::string errorMsg = "Player initialization failed with HR=0x" + std::to_string(hr);
        FPS_CONSOLE(errorMsg, "ERROR");
        return hr;
    }
    // Player movement/physics owns the camera position after the first tick.
    // Start both from the authored camera so a locked or skipped movement tick
    // cannot snap the view back to the player's constructor origin.
    m_player->SetPosition(m_camera->GetPosition());

    // Set graphics engine for weapon rendering shader setup
    m_player->SetGraphicsEngine(m_graphics);

    // Set default class (Scout)
    m_player->SetClass(PlayerClass::SCOUT, m_classSystem.get());
    FPS_CONSOLE("Player class set to Scout (default)", "SUCCESS");

    /* Scene objects - Enhanced combat arena ----------------*/
    if (m_renderingEnabled)
    {
        if (!packagedStartup)
        {
            CreateCombatArena();
        }

        // Wire up graphics engine on all ModelObjects so they don't need the global
        for (auto& obj : m_gameObjects)
        {
            if (auto* mo = dynamic_cast<ModelObject*>(obj.get()))
            {
                mo->SetGraphicsEngine(m_graphics);
            }
        }
        BindSceneMaterialRoots();
    }

    FPS_CONSOLE("Game initialization complete - class system & combat arena ready", "SUCCESS");

    /* Vehicle System -----------------------------------*/
    m_vehicleSystem = std::make_unique<Spark::VehicleSystem>();
    m_vehicleSystem->Initialize();
    if (m_projectilePool)
    {
        m_vehicleSystem->SetProjectilePool(m_projectilePool.get());
    }
    FPS_CONSOLE("Vehicle system initialized (9 vehicle types, weapons armed)", "SUCCESS");

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

    FPS_CONSOLE("Gravity zones loaded from scene file", "SUCCESS");

    InitializeInteractionObjects();
    InitializeRespawnAndVehicles();
    InitializeGameModeAndHUD();
    InitializeInventorySystem();
    InitializeQuestSystem();
    InitializeEnemies();
    InitializeGameplaySystems();

    /* Engine system integration — audio, weather, destruction, dialogue, save, advanced console commands */
    if (m_engineContext)
    {
        InitializeEngineSystems();
    }
    else
    {
        FPS_CONSOLE("Engine services will attach when the SDK-v2 context is available", "INFO");
    }

    FPS_CONSOLE("Gameplay systems online - gamemode, HUD, inventory, quests, vehicles, gravity, "
                "interactions, damage zones, and respawn",
                "SUCCESS");

    if (packagedStartup)
    {
        // Emit only after the real Game state is initialized, not from a preview
        // or parser-only path. Scene hashes/installed image identity are retained
        // by the caller; these records expose what the committed game consumed.
        std::printf("SPARK_FPS_STARTUP scene=Startup.sparkscene nodes=%d rendering=%d\n",
                    m_sceneManager->GetNodeCount(), m_renderingEnabled ? 1 : 0);
        for (int i = 0; i < m_sceneManager->GetNodeCount(); ++i)
        {
            const auto* node = m_sceneManager->GetNode(i);
            std::printf("SPARK_FPS_STARTUP_NODE index=%d type=%s position=%.9g,%.9g,%.9g "
                        "rotation=%.9g,%.9g,%.9g scale=%.9g,%.9g,%.9g\n",
                        i, node->type.c_str(), node->position.x, node->position.y, node->position.z, node->rotation.x,
                        node->rotation.y, node->rotation.z, node->scale.x, node->scale.y, node->scale.z);
            const auto& object = m_sceneManager->GetObjects()[static_cast<size_t>(i)];
            if (object)
            {
                const auto position = object->GetPosition();
                const auto scale = object->GetScale();
                std::printf("SPARK_FPS_STARTUP_MESH index=%d position=%.9g,%.9g,%.9g scale=%.9g,%.9g,%.9g\n", i,
                            position.x, position.y, position.z, scale.x, scale.y, scale.z);
            }
        }
        const auto state = m_camera->Console_GetState();
        const auto spawn = m_player->GetPosition();
        std::printf("SPARK_FPS_STARTUP_CAMERA position=%.9g,%.9g,%.9g fov=%.9g near=%.9g far=%.9g\n", state.position.x,
                    state.position.y, state.position.z, state.defaultFov, state.nearPlane, state.farPlane);
        std::printf("SPARK_FPS_STARTUP_PLAYER position=%.9g,%.9g,%.9g\n", spawn.x, spawn.y, spawn.z);
        std::fflush(stdout);
    }
    return S_OK;
}

void Game::LogSceneIdentity(bool sceneLoaded, const std::wstring& scenePath) const
{
    // CreateCombatArena() renders a plausible arena whether or not the authored
    // scene loaded, so a rendered frame alone cannot tell the two apart. Package
    // smokes require this exact marker (Tests/PackageSmoke/CheckFPSVisibleFrame.ps1)
    // as positive evidence that the authored scene, not the fallback, is live.
    // The audit trail is UTF-8 (wide paths are converted explicitly), and the smoke compares
    // this path to the package directory. It goes through the SDK ModuleLog (MOD-310).
    const auto utf8Path = std::filesystem::path(scenePath).u8string();
    const std::string path(utf8Path.begin(), utf8Path.end());
    if (!sceneLoaded || !m_sceneManager)
    {
        FPS_LOG_WARN("FPS scene identity: procedural fallback arena (authored scene failed to load from {})", path);
        return;
    }

    // Keep the scene label on one line with unambiguous quote delimiters.
    std::string sceneName;
    for (const char ch : m_sceneManager->GetMetadata().sceneName)
    {
        const auto byte = static_cast<unsigned char>(ch);
        sceneName.push_back((byte >= 0x20 && byte < 0x7F && byte != '"') ? ch : '?');
    }
    FPS_LOG_INFO("FPS scene identity: authored scene \"{}\" ({} nodes) from {}", sceneName,
                 m_sceneManager->GetNodeCount(), path);
}

void Game::BindSceneMaterialRoots()
{
    if (!m_sceneManager)
    {
        return;
    }

    const std::filesystem::path projectRoot = Spark::FPSAssets::Root().parent_path();
    const std::u8string projectRootU8 = projectRoot.u8string();
    const std::string projectRootUtf8(reinterpret_cast<const char*>(projectRootU8.data()), projectRootU8.size());
    int materialRootsBound = 0;
    if (!projectRootUtf8.empty())
    {
        for (auto& object : m_sceneManager->GetObjects())
        {
            if (object && object->SetMaterialProjectRoot(projectRootUtf8))
            {
                ++materialRootsBound;
            }
        }
        for (auto& object : m_gameObjects)
        {
            if (object && object->SetMaterialProjectRoot(projectRootUtf8))
            {
                ++materialRootsBound;
            }
        }
    }
    FPS_CONSOLE("Scene and procedural material roots bound for " + std::to_string(materialRootsBound) +
                    " renderable objects",
                "INFO");
}

void Game::InvalidateSceneBasicMaterials()
{
    if (!m_graphics || !m_sceneManager)
    {
        return;
    }

    const std::filesystem::path projectRoot = Spark::FPSAssets::Root().parent_path();
    const std::u8string projectRootU8 = projectRoot.u8string();
    const std::string projectRootUtf8(reinterpret_cast<const char*>(projectRootU8.data()), projectRootU8.size());
    if (projectRootUtf8.empty())
    {
        return;
    }

    std::unordered_set<std::string> materialPaths;
    for (const auto& object : m_sceneManager->GetObjects())
    {
        if (object && !object->GetMaterialPath().empty())
        {
            materialPaths.insert(object->GetMaterialPath());
        }
    }
    for (const auto& object : m_gameObjects)
    {
        if (object && !object->GetMaterialPath().empty())
        {
            materialPaths.insert(object->GetMaterialPath());
        }
    }

    for (const auto& materialPath : materialPaths)
    {
        m_graphics->InvalidateBasicMaterial(materialPath, projectRootUtf8);
    }
}

/*-------------------------------------------------------------
  Tear-down
--------------------------------------------------------------*/
void Game::Shutdown()
{
    EndInputObservation();
    if (m_isShutDown)
    {
        return;
    }
    m_isShutDown = true;

    FPS_CONSOLE("Game::Shutdown called.", "INFO");

    // Same host console InitializeEngineSystems registered the advanced commands on.
    if (Spark::IConsole* console = m_engineContext ? m_engineContext->GetConsole() : nullptr)
    {
        SparkConsole::UnregisterAdvancedCommands(*console);
    }

    // Subscription handles capture this Game. Detach them before destroying any
    // systems those callbacks may access.
    m_eventSubscriptions.clear();
    m_enemies.clear();
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
    m_weatherIntegration.Clear();
    m_engineContext = nullptr;
    m_engineSystemsInitialized = false;

#ifdef ENABLE_NETWORKING
    if (m_networkInitialized)
    {
        SparkFPS::FPSMultiplayerSystem::GetInstance().Shutdown();
        Spark::Net::NetworkManager::GetInstance().Shutdown();
        m_networkInitialized = false;
    }
#endif

    FPS_CONSOLE("Game shutdown complete - all systems cleaned up.", "INFO");
}

/*-------------------------------------------------------------
  Physics system wiring — propagates to all projectile pools
--------------------------------------------------------------*/
void Game::SetPhysicsSystem(PhysicsSystem* ps)
{
    if (m_projectilePool)
    {
        m_projectilePool->SetPhysicsSystem(ps);
    }
    // Player may own a separate projectile pool
    if (m_player && m_player->GetProjectilePool())
    {
        m_player->GetProjectilePool()->SetPhysicsSystem(ps);
    }
}

/*-------------------------------------------------------------
  EventBus wiring — connects cross-system event subscriptions
--------------------------------------------------------------*/
void Game::SetEventBus(Spark::EventBus* bus)
{
    // SubscriptionHandle is RAII; retaining these handles is what keeps the
    // callbacks connected. Clearing first also safely supports context swaps.
    m_eventSubscriptions.clear();
    m_eventBus = bus;

    // The respawn system publishes PlayerRespawnEvent, so it needs the same bus.
    if (m_respawnSystem)
    {
        m_respawnSystem->SetEventBus(bus);
    }

    if (!bus)
    {
        return;
    }

    // Entity killed → update gamemode scoring, quest progress, and HUD kill feed
    m_eventSubscriptions.emplace_back(bus->Subscribe<Spark::EntityKilledEvent>(
        [this](const Spark::EntityKilledEvent& e)
        {
            if (m_gameMode)
            {
                m_gameMode->RecordKill("Player1", "Enemy");
            }

            if (m_progression)
            {
                int xp = Spark::ProgressionSystem::XP_PER_KILL;
                if (m_lootSystem && m_lootSystem->HasBuff(Spark::PowerUpType::DoubleXP))
                {
                    xp *= 2;
                }
                m_progression->AwardXP(xp, "kill");
            }

            // The event currently carries no world position, so place drops a
            // short distance from the player instead of stacking them at the origin.
            if (m_lootSystem && m_player)
            {
                XMFLOAT3 deathPos = m_player->GetPosition();
                deathPos.x += 2.0f;
                deathPos.z += 2.0f;
                const bool isBoss = m_waveSpawner && m_waveSpawner->IsBossWave();
                m_lootSystem->SpawnEnemyLoot(deathPos, 0, isBoss);
            }

            // Show hit marker and add kill feed entry on HUD
            if (m_hudSystem)
            {
                m_hudSystem->ShowHitMarker(false);
                m_hudSystem->AddKillFeedEntry("Player1", "Enemy", e.cause);
            }

            // Progress kill-based quest objectives
            Spark::QuestOps::UpdateObjective(m_playerQuests, m_questRegistry, 1, 0, 1, m_eventBus, 0);
        }));

    // Entity damaged → show damage indicator on HUD
    m_eventSubscriptions.emplace_back(bus->Subscribe<Spark::EntityDamagedEvent>(
        [this](const Spark::EntityDamagedEvent& e)
        {
            if (m_hudSystem && m_player)
            {
                // Normalize damage to 0-1 intensity range (assume 100 HP max)
                float intensity = std::min(e.damage / 100.0f, 1.0f);
                // Use a default forward angle since we don't have source position
                m_hudSystem->AddDamageIndicator(0.0f, intensity);
            }
        }));

    // Item pickup → add to player inventory
    m_eventSubscriptions.emplace_back(bus->Subscribe<Spark::ItemPickedUpEvent>(
        [this](const Spark::ItemPickedUpEvent& e)
        { Spark::InventoryOps::AddItem(m_playerInventory, m_itemRegistry, e.itemDefId, e.count); }));

    // Player respawn → restore the player, teleport to spawn point, reset HUD
    m_eventSubscriptions.emplace_back(bus->Subscribe<Spark::PlayerRespawnEvent>(
        [this](const Spark::PlayerRespawnEvent& e)
        {
            // Teleport camera/player to spawn location
            if (m_camera)
            {
                m_camera->Console_SetPosition(e.spawnX, e.spawnY, e.spawnZ);
                // The event carries only a position; face the authored spawn
                // rotation of the point the respawn system actually used.
                if (m_respawnSystem && m_respawnSystem->HasLastRespawnPoint())
                {
                    const Spark::RespawnPoint& spawn = m_respawnSystem->GetLastRespawnPoint();
                    if (spawn.position.x == e.spawnX && spawn.position.y == e.spawnY && spawn.position.z == e.spawnZ)
                    {
                        m_camera->Console_SetRotation(spawn.rotation.x, spawn.rotation.y, spawn.rotation.z);
                    }
                }
            }
            if (m_player)
            {
                // Restore before reactivating: Player::Update returns early while
                // health is zero, so a respawn that only moves the player is inert.
                m_player->Console_SetHealth(m_player->GetMaxHealth());
                m_player->Console_SetArmor(0.0f);
                m_player->Console_SetPosition(e.spawnX, e.spawnY, e.spawnZ);
                m_player->SetActive(true);
            }

            // Reset HUD damage indicators on respawn
            if (m_hudSystem)
            {
                m_hudSystem->ShowHitMarker(false); // Clear any lingering hit marker
            }

            std::string msg = "Player respawned at (" + std::to_string(e.spawnX) + ", " + std::to_string(e.spawnY) +
                              ", " + std::to_string(e.spawnZ) + ")";
            FPS_CONSOLE(msg, "INFO");
        }));

    FPS_CONSOLE("EventBus connected - cross-system events wired", "SUCCESS");
}

/*-------------------------------------------------------------
  Per-frame update
--------------------------------------------------------------*/
void Game::Update(float dt)
{
    BeginInputObservation();
    if (m_isPaused)
    {
        return;
    }

    // Validate delta time to prevent physics explosions from bad frames
    if (!std::isfinite(dt) || dt < 0.0f)
    {
        FPS_LOG_EVERY_SECONDS(Warn, 5, "Invalid deltaTime {:.6f} -- clamping to 0", dt);
        dt = 0.0f;
    }
    if (dt > 0.25f)
    {
        FPS_LOG_EVERY_SECONDS(
            Warn, 5, "Large deltaTime {:.4f}s (>250ms) -- clamping to 250ms to prevent physics instability", dt);
        dt = 0.25f;
    }

    dt *= m_timeScale;
    // Re-check after scaling: the frame dt was validated above, but the scale
    // is not, and one non-finite product would poison every system below.
    if (!std::isfinite(dt))
    {
        dt = 0.0f;
    }

    bool inputUpdateCompleted = false;
    SPARK_CATCH_ALL("Game", {
        HandleInput(dt);
        RecordInputObservation("dispatch");
        UpdateArenaAutopilot(dt);
        UpdateCamera(dt);
        UpdateGameObjects(dt);
        inputUpdateCompleted = true;
    });
    if (m_inputObservationEnabled && !inputUpdateCompleted)
    {
        m_inputObservationFailed = true;
    }

    SPARK_GUARDED_UPDATE("Game:ClassSystem", "Game", {
        if (m_classSystem)
        {
            m_classSystem->Update(dt);
        }
    });
    SPARK_GUARDED_UPDATE("Game:Player", "Game", {
        if (m_player)
        {
            m_player->Update(dt);
        }
    });
    SPARK_GUARDED_UPDATE("Game:Projectiles", "Game", {
        if (m_projectilePool)
        {
            m_projectilePool->Update(dt);
            m_projectilePool->ResolveEnemyHits(m_enemies, m_eventBus);
        }
    });

    // Vehicle simulation runs from SparkGameModule::OnFixedUpdate so it is
    // deterministic and is not advanced twice by variable + fixed ticks.
    SPARK_GUARDED_UPDATE("Game:Interaction", "Game", {
        if (m_interactionSystem)
        {
            m_interactionSystem->Update(dt, m_player.get());
        }
        if (m_damageZoneSystem)
        {
            m_damageZoneSystem->Update(dt, m_player.get());
        }
        if (m_respawnSystem)
        {
            m_respawnSystem->Update(dt);
        }
    });

    // Gameplay systems
    SPARK_GUARDED_UPDATE("Game:WaveSpawner", "Game", {
        if (m_waveSpawner)
        {
            m_waveSpawner->Update(dt, GetAliveEnemyCount(), this);
        }
        if (m_lootSystem)
        {
            m_lootSystem->Update(dt, m_player.get());
        }
    });

    // Integrated systems
    SPARK_GUARDED_UPDATE("Game:GameMode", "Game", {
        if (m_gameMode)
        {
            m_gameMode->Update(dt);
        }
        if (m_hudSystem)
        {
            m_hudSystem->Update(dt);
        }
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
            if (m_weatherIntegration.IsActive())
            {
                // Cycle: Clear → Rain → Fog → Storm → Clear
                static int weatherCycle = 0;
                constexpr SparkGameFPS::WeatherPreset cycle[] = {
                    SparkGameFPS::WeatherPreset::Rain,
                    SparkGameFPS::WeatherPreset::Fog,
                    SparkGameFPS::WeatherPreset::Storm,
                    SparkGameFPS::WeatherPreset::Clear,
                };
                m_weatherIntegration.SetWeather(cycle[weatherCycle % 4], 0.8f, 5.0f);
                weatherCycle++;
            }
        }
    }

    // SequencerManager and ReplaySystem are engine-owned singletons ticked once
    // per frame by the host lifecycle (GameplayLifecycleShared::UpdateExtendedSystems).
    // Ticking them here as well advanced cutscenes and replay playback at 2x.

#ifdef ENABLE_NETWORKING
    // Update networking - process incoming messages, send outgoing state
    SPARK_GUARDED_UPDATE("Game:Networking", "Game", {
        if (m_networkInitialized)
        {
            UpdateMultiplayer(dt);
        }
    });
#endif

    // Update advanced systems through main GraphicsEngine
    SPARK_GUARDED_UPDATE("Game:Graphics", "Game", {
        if (m_graphics)
        {
            if (auto textureSystem = m_graphics->GetTextureSystem())
            {
                textureSystem->Update(dt);
            }
            if (auto assetPipeline = m_graphics->GetAssetPipeline())
            {
                assetPipeline->Update(dt);
            }
        }
    });

    // Update physics via engine context
    SPARK_GUARDED_UPDATE("Game:Physics", "Game", {
        if (m_engineContext)
        {
            if (auto* physics = m_engineContext->GetPhysics())
            {
                physics->Update(dt);
            }
        }
    });
    RecordInputObservation("complete");
}

/*-------------------------------------------------------------
  Per-frame render – called between BeginFrame / EndFrame
--------------------------------------------------------------*/
void Game::Render()
{
    // **UNIFIED RENDERING SOLUTION: Single render location for all graphics**
    if (!m_graphics)
    {
        FPS_CONSOLE("Graphics engine not available for rendering", "ERROR");
        return;
    }
    if (!m_renderingEnabled)
    {
        return; // Device-less host: nothing to draw, and BeginFrame would fail.
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
    }
    catch (const std::exception& e)
    {
        FPS_LOG_EVERY_SECONDS(Error, 5, "Rendering exception: {}", e.what());
    }
    catch (...)
    {
        FPS_LOG_EVERY_SECONDS(Error, 5, "Unknown rendering exception caught");
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
    FPS_ASSERT(dt >= 0.0f);
    if (m_camera)
    {
        m_camera->Update(dt);
    }
}

/*-------------------------------------------------------------*/
void Game::UpdateGameObjects(float dt)
{
    // No logging for per-frame operations
    FPS_ASSERT(dt >= 0.0f);
    for (auto& obj : m_gameObjects)
    {
        if (obj && obj->IsActive())
        {
            obj->Update(dt);
        }
    }
}

/*-------------------------------------------------------------
  Camera look plus arena-level input handling. Player owns movement/combat.
--------------------------------------------------------------*/
void Game::HandleInput(float)
{
    if (!m_input || !m_camera)
    {
        return;
    }

#ifdef SPARK_PLATFORM_WINDOWS
    // On Windows mouse-look belongs to the captured cursor: a click in the
    // window captures it and Escape releases it. Uncaptured, InputManager still
    // reports WM_MOUSEMOVE deltas whenever the pointer merely crosses the
    // window -- the user working in another app, or the pointer that
    // InputManager::Initialize warped to the window center -- and turning the
    // camera on those made unattended runs (and their screenshots) depend on
    // whatever the desktop pointer happened to do.
    const bool mouseLookActive = m_input->IsMouseCaptured();
#else
    // The POSIX/SDL path never enters capture on click, so the uncaptured
    // cursor is the only mouse-look source there.
    const bool mouseLookActive = true;
#endif
    auto [dx, dy] = m_input->GetMouseDelta();
    if (mouseLookActive && (dx != 0 || dy != 0))
    {
        constexpr float mouseSens = 0.005f;
        m_camera->Yaw(dx * mouseSens);
        m_camera->Pitch(-dy * mouseSens);
    }

    m_camera->SetZoom(m_input->IsMouseButtonDown(1));

    // Player-facing persistence remains available when the developer console is
    // disabled. Edge queries prevent held keys from repeatedly rewriting/loading.
    const bool savePressed = m_input->WasKeyPressed(VK_F2);
    const bool loadPressed = m_input->WasKeyPressed(VK_F3);
    // Simultaneous rising edges must not overwrite a slot before loading it.
    if (savePressed != loadPressed)
    {
        std::string message;
        const bool succeeded = savePressed ? QuickSaveProfile(message) : QuickLoadProfile(message);
        if (m_inputObservationEnabled)
        {
            ++m_inputObservationOperation;
            m_inputObservationAction = savePressed ? 1 : 2;
            m_inputObservationResult = succeeded ? 1 : 0;
            RecordInputObservation("operation");
        }
        if (succeeded)
        {
            FPS_LOG_INFO("{}", message);
        }
        else
        {
            FPS_LOG_WARN("{}", message);
        }
    }

    if (m_inputObservationEnabled && savePressed && loadPressed)
    {
        m_inputObservationAction = 3;
        RecordInputObservation("operation");
    }

    // Class switching with F5-F10 keys
    if (m_input->WasKeyPressed(VK_F5))
    {
        SetPlayerClass(PlayerClass::SCOUT);
    }
    if (m_input->WasKeyPressed(VK_F6))
    {
        SetPlayerClass(PlayerClass::MEDIC);
    }
    if (m_input->WasKeyPressed(VK_F7))
    {
        SetPlayerClass(PlayerClass::ENGINEER);
    }
    if (m_input->WasKeyPressed(VK_F8))
    {
        SetPlayerClass(PlayerClass::RECON);
    }
    if (m_input->WasKeyPressed(VK_F9))
    {
        SetPlayerClass(PlayerClass::VANGUARD);
    }
    if (m_input->WasKeyPressed(VK_F10))
    {
        SetPlayerClass(PlayerClass::TITAN);
    }

    // F11 starts or restarts the complete survival loop used by UI and console.
    if (m_input->WasKeyPressed(VK_F11))
    {
        StartWaves();
    }

    // Cycle classes with [ and ]
    if (m_input->WasKeyPressed(VK_OEM_4))
    {
        CyclePrevClass(); // [ key
    }
    if (m_input->WasKeyPressed(VK_OEM_6))
    {
        CycleNextClass(); // ] key
    }

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
    if (!m_renderingEnabled)
    {
        FPS_CONSOLE("Test objects need a D3D11 device - skipped on a device-less host", "WARNING");
        return;
    }

    FPS_CONSOLE("Creating test objects...", "INFO");

    // Ground plane
    {
        auto ground = std::make_unique<PlaneObject>(20.0f, 20.0f);
        FPS_ASSERT(ground);
        HRESULT hr = ground->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            FPS_CONSOLE("Ground plane created successfully", "INFO");
            ground->SetPosition({0.0f, -1.0f, 0.0f});
            m_gameObjects.push_back(std::move(ground));
        }
        else
        {
            std::string errorMsg = "Ground plane creation failed with HR=0x" + std::to_string(hr);
            FPS_CONSOLE(errorMsg, "ERROR");
        }
    }

    // Row of cubes
    int cubesCreated = 0;
    for (int i = 0; i < 5; ++i)
    {
        auto cube = std::make_unique<CubeObject>(1.0f);
        FPS_ASSERT(cube);
        HRESULT hr = cube->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            cube->SetPosition({i * 3.0f - 6.0f, 1.0f, 10.0f});
            m_gameObjects.push_back(std::move(cube));
            cubesCreated++;
        }
        else
        {
            std::string errorMsg = "Cube " + std::to_string(i) + " creation failed with HR=0x" + std::to_string(hr);
            FPS_CONSOLE(errorMsg, "ERROR");
        }
    }
    std::string cubeMsg = "Created " + std::to_string(cubesCreated) + " cubes";
    FPS_CONSOLE(cubeMsg, "INFO");

    // Single sphere
    {
        auto sphere = std::make_unique<SphereObject>(1.0f, 16, 16);
        FPS_ASSERT(sphere);
        HRESULT hr = sphere->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            FPS_CONSOLE("Sphere created successfully", "INFO");
            sphere->SetPosition({5.0f, 0.0f, 0.0f});
            m_gameObjects.push_back(std::move(sphere));
        }
        else
        {
            std::string errorMsg = "Sphere creation failed with HR=0x" + std::to_string(hr);
            FPS_CONSOLE(errorMsg, "ERROR");
        }
    }

    // **ENHANCED: Add model-based objects using our new .obj files**
    {
        // Target practice targets
        auto target1 = std::make_unique<ModelObject>(Spark::FPSAssets::Resolve(L"Models/target.obj"));
        FPS_ASSERT(target1);
        HRESULT hr = target1->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            target1->SetPosition({-8.0f, 2.0f, 15.0f});
            target1->SetName("Target_1");
            m_gameObjects.push_back(std::move(target1));
            FPS_CONSOLE("Target 1 model created successfully", "INFO");
        }
        else
        {
            FPS_CONSOLE("Target 1 model creation failed", "WARNING");
        }

        auto target2 = std::make_unique<ModelObject>(Spark::FPSAssets::Resolve(L"Models/target.obj"));
        FPS_ASSERT(target2);
        hr = target2->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            target2->SetPosition({8.0f, 2.0f, 15.0f});
            target2->SetName("Target_2");
            m_gameObjects.push_back(std::move(target2));
            FPS_CONSOLE("Target 2 model created successfully", "INFO");
        }
        else
        {
            FPS_CONSOLE("Target 2 model creation failed", "WARNING");
        }

        // Character model for testing
        auto character = std::make_unique<ModelObject>(Spark::FPSAssets::Resolve(L"Models/character.obj"));
        FPS_ASSERT(character);
        hr = character->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            character->SetPosition({0.0f, 0.0f, 8.0f});
            character->SetName("Character_Model");
            m_gameObjects.push_back(std::move(character));
            FPS_CONSOLE("Character model created successfully", "INFO");
        }
        else
        {
            FPS_CONSOLE("Character model creation failed", "WARNING");
        }

        // Weapon display (rifle on a stand)
        auto weaponDisplay = std::make_unique<ModelObject>(Spark::FPSAssets::Resolve(L"Models/rifle.obj"));
        FPS_ASSERT(weaponDisplay);
        hr = weaponDisplay->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            weaponDisplay->SetPosition({-3.0f, 1.5f, 5.0f});
            weaponDisplay->SetName("Weapon_Display");
            m_gameObjects.push_back(std::move(weaponDisplay));
            FPS_CONSOLE("Weapon display model created successfully", "INFO");
        }
        else
        {
            FPS_CONSOLE("Weapon display model creation failed", "WARNING");
        }
    }

    std::string totalMsg =
        "Test objects creation complete. Total: " + std::to_string(m_gameObjects.size()) + " objects";
    FPS_CONSOLE(totalMsg, "SUCCESS");
}
