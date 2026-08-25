/**
 * @file GameConsoleOps.cpp
 * @brief Console integration, graphics settings, scene management, class system,
 *        and combat arena methods for the Game class.
 *
 * Extracted from Game.cpp to keep each file focused on one cohesive responsibility.
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cstdint>
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS

#include "Game.h"
#include "ClassSystem.h"
#include "Utils/Assert.h"
#include "Utils/Validate.h"
#include "Utils/SparkConsole.h"

#include "Graphics/GraphicsEngine.h"
#include "Physics/PhysicsSystem.h"
#include "Camera/SparkEngineCamera.h"
#include "Game/GameObject.h"
#include "CubeObject.h"
#include "PlaneObject.h"
#include "SphereObject.h"
#include "ModelObject.h"
#include "Enemy.h"
#include "Player.h"
#include "Projectiles/ProjectilePool.h"
#include "SceneManager/SceneManager.h"
#include "Engine/Networking/NetworkManager.h"
#include <algorithm>
#include <filesystem>

#include "Utils/LogMacros.h"

using namespace DirectX;

// ============================================================================
// CONSOLE INTEGRATION IMPLEMENTATIONS - Real Cross-Hook System Integration
// ============================================================================

void Game::ApplyPhysicsSettings(float gravity, float playerSpeed, float jumpHeight, float friction)
{
    // Apply gravity to the GravitySystem if available
    if (m_gravitySystem)
    {
        m_gravitySystem->Initialize({0, -gravity, 0});
    }

    // Apply player movement settings via Player's console API
    if (m_player)
    {
        m_player->Console_SetSpeed(playerSpeed);
        m_player->Console_SetJumpHeight(jumpHeight);
    }

    // Apply friction to camera movement speed as a proxy
    if (m_camera && friction > 0.0f)
    {
        m_camera->Console_SetMoveSpeed(playerSpeed * friction);
    }

    std::wstring settingsMsg = L"Physics updated - Gravity: " + std::to_wstring(gravity) + L", Speed: " +
                               std::to_wstring(playerSpeed) + L", Jump: " + std::to_wstring(jumpHeight) +
                               L", Friction: " + std::to_wstring(friction);
    LOG_TO_CONSOLE_IMMEDIATE(settingsMsg, L"SUCCESS");
}

void Game::ApplyCameraSettings(float fov, float sensitivity, bool invertY)
{
    if (!m_camera)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Camera settings failed - camera not available", L"ERROR");
        return;
    }

    // Apply FOV via camera's console API
    if (fov > 0.0f)
    {
        m_camera->Console_SetFOV(fov);
    }

    // Apply mouse sensitivity and Y-axis inversion
    m_camera->Console_SetMouseSensitivity(sensitivity);
    m_camera->Console_SetInvertY(invertY);

    std::wstring cameraMsg = L"Camera settings applied - FOV: " + std::to_wstring(fov) + L", Sensitivity: " +
                             std::to_wstring(sensitivity) + L", InvertY: " + (invertY ? L"ON" : L"OFF");
    LOG_TO_CONSOLE_IMMEDIATE(cameraMsg, L"SUCCESS");
}

void Game::ApplyDebugSettings(bool godMode, bool noclip, bool infiniteAmmo)
{
    m_godModeEnabled = godMode;
    m_noclipEnabled = noclip;
    m_infiniteAmmoEnabled = infiniteAmmo;

    // Forward debug settings to the Player's console API
    if (m_player)
    {
        m_player->Console_SetGodMode(godMode);
        m_player->Console_SetNoclip(noclip);
        m_player->Console_SetInfiniteAmmo(infiniteAmmo);
    }

    std::wstring debugMsg = L"Debug settings applied - God Mode: " +
                            (godMode ? std::wstring(L"ON") : std::wstring(L"OFF")) + L", Noclip: " +
                            (noclip ? std::wstring(L"ON") : std::wstring(L"OFF")) + L", Infinite Ammo: " +
                            (infiniteAmmo ? std::wstring(L"ON") : std::wstring(L"OFF"));
    LOG_TO_CONSOLE_IMMEDIATE(debugMsg, L"SUCCESS");
}

void Game::GetPerformanceStats(int& outDrawCalls, int& outTriangles, int& outActiveObjects) const
{
    int activeCount = 0;
    for (const auto& obj : m_gameObjects)
    {
        if (obj && obj->IsActive())
            activeCount++;
    }

    if (m_sceneManager)
    {
        for (const auto& obj : m_sceneManager->GetObjects())
        {
            if (obj && obj->IsActive())
                activeCount++;
        }
    }

    if (m_vehicleSystem)
    {
        for (const auto& v : m_vehicleSystem->GetVehicles())
        {
            if (v && v->IsActive())
                activeCount++;
        }
    }

    if (m_interactionSystem)
    {
        for (const auto& obj : m_interactionSystem->GetObjects())
        {
            if (obj && obj->IsActive())
                activeCount++;
        }
    }

    if (m_player)
        activeCount++;

    outDrawCalls = activeCount;
    outTriangles = 0;

    if (m_graphics)
    {
        try
        {
            auto metrics = m_graphics->Console_GetStatistics();
            outDrawCalls = static_cast<int>(metrics.drawCalls);
            outTriangles = static_cast<int>(metrics.triangles);
        }
        catch (...)
        {
        }
    }

    outActiveObjects = activeCount;
}

void Game::TeleportPlayer(float x, float y, float z)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Teleporting player via console integration", L"INFO");

    if (m_camera)
    {
        m_camera->SetPosition({x, y, z});

        std::wstring teleportMsg = L"Player teleported to (" + std::to_wstring(x) + L", " + std::to_wstring(y) + L", " +
                                   std::to_wstring(z) + L")";
        LOG_TO_CONSOLE_IMMEDIATE(teleportMsg, L"SUCCESS");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Teleport failed - camera not available", L"ERROR");
    }
}

bool Game::SpawnObject(const std::string& type, float x, float y, float z)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Spawning object via console integration", L"INFO");

    std::unique_ptr<GameObject> newObject;

    if (type == "cube")
    {
        newObject = std::make_unique<CubeObject>(1.0f);
    }
    else if (type == "sphere")
    {
        newObject = std::make_unique<SphereObject>(1.0f, 16, 16);
    }
    else if (type == "wall" || type == "plane")
    {
        newObject = std::make_unique<PlaneObject>(2.0f, 2.0f);
    }
    else
    {
        std::wstring errorMsg = L"Unknown object type: " + std::wstring(type.begin(), type.end());
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return false;
    }

    if (newObject)
    {
        HRESULT hr = newObject->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            newObject->SetPosition({x, y, z});
            m_gameObjects.push_back(std::move(newObject));

            std::wstring spawnMsg = L"Spawned " + std::wstring(type.begin(), type.end()) + L" at (" +
                                    std::to_wstring(x) + L", " + std::to_wstring(y) + L", " + std::to_wstring(z) + L")";
            LOG_TO_CONSOLE_IMMEDIATE(spawnMsg, L"SUCCESS");
            return true;
        }
        else
        {
            std::wstring errorMsg = L"Failed to initialize spawned object, HR=0x" + std::to_wstring(hr);
            LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        }
    }

    return false;
}

bool Game::DeleteObject(size_t index)
{
    if (!SPARK_BOUNDS_CHECK(index, m_gameObjects.size()))
    {
        SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "Game", 5, "DeleteObject: index %zu out of bounds (size=%zu)",
                                index, m_gameObjects.size());
        return false;
    }

    const GameObject* removedObject = m_gameObjects[index].get();
    std::erase_if(m_enemies, [removedObject](const Enemy* enemy) { return enemy == removedObject; });
    m_gameObjects.erase(m_gameObjects.begin() + index);

    std::wstring deleteMsg = L"Deleted object at index " + std::to_wstring(index) + L". Remaining objects: " +
                             std::to_wstring(m_gameObjects.size());
    LOG_TO_CONSOLE_IMMEDIATE(deleteMsg, L"SUCCESS");
    return true;
}

void Game::ClearScene(bool keepPlayer)
{
    size_t originalCount = m_gameObjects.size();
    m_enemies.clear();
    m_gameObjects.clear();

    if (!keepPlayer)
    {
        if (m_hudSystem)
            m_hudSystem->SetPlayer(nullptr);
        m_player.reset();
        m_projectilePool.reset();
    }

    std::wstring clearMsg = L"Cleared " + std::to_wstring(originalCount) + L" objects from scene";
    if (keepPlayer)
        clearMsg += L" (player preserved)";
    LOG_TO_CONSOLE_IMMEDIATE(clearMsg, L"SUCCESS");
}

void Game::SetTimeScale(float scale)
{
    if (scale < 0.1f || scale > 10.0f)
    {
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

    if (m_graphics)
    {
        try
        {
            m_graphics->Console_SetWireframe(wireframe);
            m_graphics->Console_SetVSync(vsync);
            m_showFPS = showFPS;

            std::wstring graphicsMsg = L"Graphics settings applied - Wireframe: " +
                                       (wireframe ? std::wstring(L"ON") : std::wstring(L"OFF")) + L", VSync: " +
                                       (vsync ? std::wstring(L"ON") : std::wstring(L"OFF")) + L", Show FPS: " +
                                       (showFPS ? std::wstring(L"ON") : std::wstring(L"OFF"));
            LOG_TO_CONSOLE_IMMEDIATE(graphicsMsg, L"SUCCESS");
        }
        catch (...)
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to apply graphics settings", L"ERROR");
        }
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Graphics settings failed - graphics engine not available", L"ERROR");
    }
}

void Game::GetGraphicsPerformance(float& outFrameTime, float& outRenderTime, float& outUpdateTime) const
{
    outFrameTime = 0.0f;
    outRenderTime = 0.0f;
    outUpdateTime = 0.0f;

    if (m_graphics)
    {
        try
        {
            auto metrics = m_graphics->Console_GetStatistics();
            outFrameTime = metrics.frameTime;
            outRenderTime = metrics.renderTime;
            outUpdateTime = metrics.presentTime; // Use present time as update time approximation
        }
        catch (...)
        {
            // Fallback values - keep at 0.0f
        }
    }
}

void Game::RefreshGraphicsSettings()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Refreshing graphics settings via console integration", L"INFO");

    if (m_graphics)
    {
        try
        {
            // Trigger a refresh of graphics state
            m_graphics->Console_ResetDevice();
            LOG_TO_CONSOLE_IMMEDIATE(L"Graphics settings refreshed successfully", L"SUCCESS");
        }
        catch (...)
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to refresh graphics settings", L"ERROR");
        }
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Graphics refresh failed - graphics engine not available", L"ERROR");
    }
}

// ============================================================================
// ENHANCED SCENE MANAGEMENT METHODS - Full Implementation
// ============================================================================

bool Game::LoadScene(const std::string& scenePath)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Loading scene via console integration", L"INFO");

    if (!m_sceneManager)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Scene load failed - scene manager not available", L"ERROR");
        return false;
    }

    try
    {
        // Convert string to wstring for scene manager
        std::wstring wScenePath(scenePath.begin(), scenePath.end());
        bool success = m_sceneManager->LoadScene(wScenePath);

        if (success)
        {
            // Clear existing game objects if loading a new scene
            m_enemies.clear();
            m_gameObjects.clear();

            std::wstring loadMsg = L"Scene loaded successfully: " + wScenePath;
            LOG_TO_CONSOLE_IMMEDIATE(loadMsg, L"SUCCESS");
        }
        else
        {
            std::wstring loadMsg = L"Failed to load scene: " + wScenePath;
            LOG_TO_CONSOLE_IMMEDIATE(loadMsg, L"ERROR");
        }

        return success;
    }
    catch (...)
    {
        std::wstring errorMsg =
            L"Exception occurred while loading scene: " + std::wstring(scenePath.begin(), scenePath.end());
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return false;
    }
}

bool Game::SaveScene(const std::string& scenePath)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Saving scene via console integration", L"INFO");

    if (!m_sceneManager)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Scene save failed - scene manager not available", L"ERROR");
        return false;
    }

    std::wstring wScenePath(scenePath.begin(), scenePath.end());
    bool saved = m_sceneManager->SaveScene(wScenePath);
    if (saved)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Scene saved: " + wScenePath, L"SUCCESS");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Scene save failed: " + wScenePath, L"ERROR");
    }
    return saved;
}

std::vector<std::string> Game::GetAvailableScenes() const
{
    std::vector<std::string> scenes;

    // Scan common scene directories for .scene / .xml / .json files
    const std::string sceneDirs[] = {"../Assets/Scenes", "../Scenes", "Scenes"};
    for (const auto& dir : sceneDirs)
    {
        try
        {
            if (!std::filesystem::exists(dir))
                continue;
            for (const auto& entry : std::filesystem::directory_iterator(dir))
            {
                if (!entry.is_regular_file())
                    continue;
                auto ext = entry.path().extension().string();
                if (ext == ".scene" || ext == ".xml" || ext == ".json")
                {
                    scenes.push_back(entry.path().string());
                }
            }
        }
        catch (...)
        {
            // Directory not accessible, skip
        }
    }

    return scenes;
}

// ============================================================================
// CLASS SYSTEM METHODS
// ============================================================================

void Game::SetPlayerClass(PlayerClass classType)
{
    if (m_player && m_classSystem)
    {
        m_player->SetClass(classType, m_classSystem.get());
        const auto& def = m_classSystem->GetClassDefinition(classType);

        // Notify HUD of class change
        if (m_hudSystem)
        {
            m_hudSystem->ShowClassChange(def.name, classType);
            m_hudSystem->SetCurrentClass(classType);
        }

        std::wstring classMsg = L"Class changed to: " + std::wstring(def.name.begin(), def.name.end());
        LOG_TO_CONSOLE_IMMEDIATE(classMsg, L"SUCCESS");
    }
}

PlayerClass Game::GetPlayerClass() const
{
    if (m_player)
        return m_player->GetClass();
    return PlayerClass::SCOUT;
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

namespace
{

    /// Helper: create a ModelObject, initialize it, set position/name, and add to the list
    void PlaceModel(const wchar_t* modelPath, const std::string& name, XMFLOAT3 pos, ID3D11Device* device,
                    ID3D11DeviceContext* context, std::vector<std::unique_ptr<GameObject>>& objects,
                    XMFLOAT3 scale = {1.0f, 1.0f, 1.0f})
    {
        auto obj = std::make_unique<ModelObject>(modelPath);
        if (!obj)
            return;
        HRESULT hr = obj->Initialize(device, context);
        if (FAILED(hr))
            return;
        obj->SetPosition(pos);
        obj->SetName(name);
        if (scale.x != 1.0f || scale.y != 1.0f || scale.z != 1.0f)
            obj->SetScale(scale);
        objects.push_back(std::move(obj));
    }

    /// Helper: place an array of models at listed positions with indexed names
    template <size_t N>
    void PlaceModelsAt(const wchar_t* modelPath, const std::string& prefix, const float (&positions)[N][3],
                       ID3D11Device* device, ID3D11DeviceContext* context,
                       std::vector<std::unique_ptr<GameObject>>& objects)
    {
        for (size_t i = 0; i < N; ++i)
        {
            PlaceModel(modelPath, prefix + std::to_string(i + 1), {positions[i][0], positions[i][1], positions[i][2]},
                       device, context, objects);
        }
    }

} // anonymous namespace

void Game::CreateCombatArena()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Creating enhanced combat arena level...", L"INFO");

    auto* device = m_graphics->GetDevice();
    auto* context = m_graphics->GetContext();

    // === LARGE GROUND PLANE (200x200 arena) ===
    {
        auto ground = std::make_unique<PlaneObject>(100.0f, 100.0f);
        ASSERT(ground);
        if (SUCCEEDED(ground->Initialize(device, context)))
        {
            ground->SetPosition({0.0f, -1.0f, 0.0f});
            ground->SetName("Arena_Ground");
            m_gameObjects.push_back(std::move(ground));
        }
    }

    // === ALPHA BASE (south, z = -70) ===
    PlaceModel(L"../Assets/Models/building_small.obj", "Alpha_HQ", {-8.0f, 0.0f, -70.0f}, device, context,
               m_gameObjects);
    for (int i = -1; i <= 1; ++i)
    {
        PlaceModel(L"../Assets/Models/barrier.obj", "Alpha_Barrier_" + std::to_string(i + 2), {i * 15.0f, 0.0f, -65.0f},
                   device, context, m_gameObjects);
    }
    PlaceModel(L"../Assets/Models/crate.obj", "Alpha_Crate_1", {-5.0f, 0.0f, -68.0f}, device, context, m_gameObjects);
    PlaceModel(L"../Assets/Models/crate.obj", "Alpha_Crate_2", {5.0f, 0.0f, -68.0f}, device, context, m_gameObjects);

    // === BRAVO BASE (north, z = 70) ===
    PlaceModel(L"../Assets/Models/building_small.obj", "Bravo_HQ", {8.0f, 0.0f, 70.0f}, device, context, m_gameObjects);
    for (int i = -1; i <= 1; ++i)
    {
        PlaceModel(L"../Assets/Models/barrier.obj", "Bravo_Barrier_" + std::to_string(i + 2), {i * 15.0f, 0.0f, 65.0f},
                   device, context, m_gameObjects);
    }
    PlaceModel(L"../Assets/Models/crate.obj", "Bravo_Crate_1", {-5.0f, 0.0f, 68.0f}, device, context, m_gameObjects);
    PlaceModel(L"../Assets/Models/crate.obj", "Bravo_Crate_2", {5.0f, 0.0f, 68.0f}, device, context, m_gameObjects);

    // === CENTER OBJECTIVE (z = 0) ===
    PlaceModel(L"../Assets/Models/building_small.obj", "Center_Building", {0.0f, 0.0f, 0.0f}, device, context,
               m_gameObjects, {1.2f, 1.0f, 1.2f});
    {
        const float coverPos[][3] = {
            {-8.0f, 0.0f, -3.0f}, {8.0f, 0.0f, 3.0f}, {-3.0f, 0.0f, 8.0f}, {3.0f, 0.0f, -8.0f}};
        PlaceModelsAt(L"../Assets/Models/barrier.obj", "Center_Barrier_", coverPos, device, context, m_gameObjects);

        const float cratePos[][3] = {
            {-3.0f, 0.0f, 5.0f}, {3.0f, 0.0f, -5.0f}, {-6.0f, 0.0f, -6.0f}, {6.0f, 0.0f, 6.0f}};
        PlaceModelsAt(L"../Assets/Models/crate.obj", "Center_Crate_", cratePos, device, context, m_gameObjects);
    }

    // === WEST OUTPOST (x = -45) ===
    PlaceModel(L"../Assets/Models/watchtower.obj", "West_Tower", {-45.0f, 0.0f, 0.0f}, device, context, m_gameObjects);
    PlaceModel(L"../Assets/Models/barrier.obj", "West_Cover_1", {-40.0f, 0.0f, -5.0f}, device, context, m_gameObjects);
    PlaceModel(L"../Assets/Models/barrier.obj", "West_Cover_2", {-40.0f, 0.0f, 5.0f}, device, context, m_gameObjects);
    PlaceModel(L"../Assets/Models/crate.obj", "West_Crate_1", {-48.0f, 0.0f, -2.0f}, device, context, m_gameObjects);
    PlaceModel(L"../Assets/Models/crate.obj", "West_Crate_2", {-48.0f, 0.0f, 2.0f}, device, context, m_gameObjects);

    // === EAST OUTPOST (x = 45) ===
    PlaceModel(L"../Assets/Models/watchtower.obj", "East_Tower", {45.0f, 0.0f, 0.0f}, device, context, m_gameObjects);
    PlaceModel(L"../Assets/Models/barrier.obj", "East_Cover_1", {40.0f, 0.0f, -5.0f}, device, context, m_gameObjects);
    PlaceModel(L"../Assets/Models/barrier.obj", "East_Cover_2", {40.0f, 0.0f, 5.0f}, device, context, m_gameObjects);
    PlaceModel(L"../Assets/Models/crate.obj", "East_Crate_1", {48.0f, 0.0f, -2.0f}, device, context, m_gameObjects);
    PlaceModel(L"../Assets/Models/crate.obj", "East_Crate_2", {48.0f, 0.0f, 2.0f}, device, context, m_gameObjects);

    // === MID-FIELD COVER (scattered barriers and crates) ===
    {
        const float barrierPos[][3] = {{-25.0f, 0.0f, -30.0f}, {25.0f, 0.0f, -30.0f}, {-25.0f, 0.0f, 30.0f},
                                       {25.0f, 0.0f, 30.0f},   {-20.0f, 0.0f, 0.0f},  {20.0f, 0.0f, 0.0f},
                                       {0.0f, 0.0f, -35.0f},   {0.0f, 0.0f, 35.0f}};
        PlaceModelsAt(L"../Assets/Models/barrier.obj", "Field_Barrier_", barrierPos, device, context, m_gameObjects);

        const float cratePos[][3] = {{-15.0f, 0.0f, -15.0f}, {15.0f, 0.0f, -15.0f},  {-15.0f, 0.0f, 15.0f},
                                     {15.0f, 0.0f, 15.0f},   {-30.0f, 0.0f, -50.0f}, {30.0f, 0.0f, -50.0f},
                                     {-30.0f, 0.0f, 50.0f},  {30.0f, 0.0f, 50.0f}};
        PlaceModelsAt(L"../Assets/Models/crate.obj", "Field_Crate_", cratePos, device, context, m_gameObjects);
    }

    // === TARGET PRACTICE AREA (west side) ===
    for (int i = 0; i < 5; ++i)
    {
        PlaceModel(L"../Assets/Models/target.obj", "Target_" + std::to_string(i + 1), {-55.0f, 0.0f, -10.0f + i * 5.0f},
                   device, context, m_gameObjects);
    }

    // === CHARACTER MODELS (NPCs / bots placeholder) ===
    {
        const float npcPos[][3] = {
            {0.0f, 0.0f, 20.0f}, {10.0f, 0.0f, -20.0f}, {-10.0f, 0.0f, 30.0f}, {20.0f, 0.0f, -40.0f}};
        PlaceModelsAt(L"../Assets/Models/character.obj", "NPC_", npcPos, device, context, m_gameObjects);
    }

    // === DECORATIVE SPHERES (control point markers) ===
    {
        const float cpPositions[][3] = {{0.0f, 3.0f, 0.0f}, {-45.0f, 3.0f, 0.0f}, {45.0f, 3.0f, 0.0f}};
        for (int i = 0; i < 3; ++i)
        {
            auto sphere = std::make_unique<SphereObject>(0.5f, 12, 12);
            if (sphere && SUCCEEDED(sphere->Initialize(device, context)))
            {
                sphere->SetPosition({cpPositions[i][0], cpPositions[i][1], cpPositions[i][2]});
                sphere->SetName("ControlPoint_" + std::to_string(i + 1));
                m_gameObjects.push_back(std::move(sphere));
            }
        }
    }

    // === WEAPON DISPLAYS (at spawn) ===
    {
        const wchar_t* weaponModels[] = {L"../Assets/Models/rifle.obj", L"../Assets/Models/sniper.obj",
                                         L"../Assets/Models/lmg.obj", L"../Assets/Models/shotgun.obj",
                                         L"../Assets/Models/pistol.obj"};
        const char* weaponNames[] = {"Rifle_Display", "Sniper_Display", "LMG_Display", "Shotgun_Display",
                                     "Pistol_Display"};
        for (int i = 0; i < 5; ++i)
        {
            PlaceModel(weaponModels[i], weaponNames[i], {-3.0f + i * 1.5f, 1.2f, -72.0f}, device, context,
                       m_gameObjects, {3.0f, 3.0f, 3.0f});
        }
    }

    std::wstring totalMsg = L"Combat arena created. Total objects: " + std::to_wstring(m_gameObjects.size());
    LOG_TO_CONSOLE_IMMEDIATE(totalMsg, L"SUCCESS");
}

void Game::CreateTestScene(const std::string& sceneType)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Creating test scene via console integration", L"INFO");

    // Clear existing objects
    m_enemies.clear();
    m_gameObjects.clear();

    if (sceneType == "basic")
    {
        // Create basic test scene
        CreateTestObjects(); // Use existing method
    }
    else if (sceneType == "performance")
    {
        // Create a performance test scene with many objects
        LOG_TO_CONSOLE_IMMEDIATE(L"Creating performance test scene with many objects", L"INFO");

        int objectsCreated = 0;
        for (int x = -10; x <= 10; x += 2)
        {
            for (int z = -10; z <= 10; z += 2)
            {
                auto cube = std::make_unique<CubeObject>(0.5f);
                if (cube)
                {
                    HRESULT hr = cube->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                    if (SUCCEEDED(hr))
                    {
                        cube->SetPosition({static_cast<float>(x), 0.5f, static_cast<float>(z)});
                        m_gameObjects.push_back(std::move(cube));
                        objectsCreated++;
                    }
                }
            }
        }

        std::wstring perfMsg = L"Performance test scene created with " + std::to_wstring(objectsCreated) + L" objects";
        LOG_TO_CONSOLE_IMMEDIATE(perfMsg, L"SUCCESS");
    }
    else if (sceneType == "empty")
    {
        // Create empty scene (just clear objects)
        LOG_TO_CONSOLE_IMMEDIATE(L"Empty test scene created", L"SUCCESS");
    }
    else
    {
        // Unknown scene type, create basic
        std::wstring unknownMsg =
            L"Unknown scene type '" + std::wstring(sceneType.begin(), sceneType.end()) + L"', creating basic scene";
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
    if (!m_vehicleSystem || !m_graphics)
        return nullptr;
    auto* vehicle = m_vehicleSystem->SpawnVehicle(type, {x, y, z}, m_graphics->GetDevice(), m_graphics->GetContext());
    if (vehicle)
    {
        // Wire projectile pool so vehicle weapons can fire
        if (m_projectilePool)
            vehicle->SetProjectilePool(m_projectilePool.get());

        std::wstring msg = L"Vehicle spawned: " +
                           std::wstring(vehicle->GetVehicleName().begin(), vehicle->GetVehicleName().end()) + L" at (" +
                           std::to_wstring(x) + L"," + std::to_wstring(y) + L"," + std::to_wstring(z) + L")";
        LOG_TO_CONSOLE_IMMEDIATE(msg, L"SUCCESS");
    }
    return vehicle;
}

bool Game::PlayerEnterNearestVehicle()
{
    if (!m_player || !m_vehicleSystem)
        return false;
    if (m_player->IsInVehicle())
        return false;

    auto* vehicle = m_vehicleSystem->FindNearestVehicle(m_player->GetPosition(), 5.0f);
    if (vehicle)
    {
        return m_player->EnterVehicle(vehicle);
    }

    LOG_TO_CONSOLE(L"No vehicle nearby to enter", L"INFO");
    return false;
}

bool Game::PlayerExitVehicle()
{
    if (!m_player || !m_player->IsInVehicle())
        return false;
    return m_player->ExitVehicle();
}

// ============================================================================
// NETWORKING SYSTEM
// ============================================================================

#ifdef ENABLE_NETWORKING

bool Game::StartServer(uint16_t port, int maxClients)
{
    auto& netMgr = Spark::Net::NetworkManager::GetInstance();
    if (!m_networkInitialized)
    {
        if (!netMgr.Initialize())
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"NetworkManager::Initialize() failed", L"ERROR");
            return false;
        }
        m_networkInitialized = true;
    }

    if (!netMgr.StartServer(port, maxClients))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to start server on port " + std::to_wstring(port), L"ERROR");
        return false;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Server started on port " + std::to_wstring(port) + L" (max " +
                                 std::to_wstring(maxClients) + L" clients)",
                             L"SUCCESS");

    // Register player entity for replication
    Spark::Net::ReplicatedEntity playerEntity{};
    playerEntity.entityType = "Player";
    playerEntity.ownerID = netMgr.GetLocalClientID();
    if (m_player)
    {
        auto pos = m_player->GetPosition();
        playerEntity.position = {pos.x, pos.y, pos.z};
    }
    netMgr.RegisterReplicatedEntity(playerEntity);

    return true;
}

bool Game::ConnectToServer(const std::string& address, uint16_t port)
{
    auto& netMgr = Spark::Net::NetworkManager::GetInstance();
    if (!m_networkInitialized)
    {
        if (!netMgr.Initialize())
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"NetworkManager::Initialize() failed", L"ERROR");
            return false;
        }
        m_networkInitialized = true;
    }

    std::wstring addr(address.begin(), address.end());
    LOG_TO_CONSOLE_IMMEDIATE(L"Connecting to " + addr + L":" + std::to_wstring(port) + L"...", L"INFO");
    if (!netMgr.Connect(address, port, "Player"))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to connect to " + addr + L":" + std::to_wstring(port), L"ERROR");
        return false;
    }
    return true;
}

void Game::DisconnectNetwork()
{
    auto& netMgr = Spark::Net::NetworkManager::GetInstance();
    if (netMgr.GetRole() == Spark::Net::NetworkRole::Server)
    {
        netMgr.StopServer();
        LOG_TO_CONSOLE_IMMEDIATE(L"Server stopped", L"INFO");
    }
    else if (netMgr.GetRole() == Spark::Net::NetworkRole::Client)
    {
        netMgr.Disconnect();
        LOG_TO_CONSOLE_IMMEDIATE(L"Disconnected from server", L"INFO");
    }
}

bool Game::IsNetworkActive() const
{
    if (!m_networkInitialized)
        return false;
    auto& netMgr = Spark::Net::NetworkManager::GetInstance();
    return netMgr.GetRole() != Spark::Net::NetworkRole::None;
}

std::string Game::GetNetworkStatus() const
{
    if (!m_networkInitialized)
        return "Networking not initialized";
    return Spark::Net::NetworkManager::GetInstance().Console_GetStatus();
}

Spark::Net::NetworkStats Game::GetNetworkStats() const
{
    if (!m_networkInitialized)
        return {};
    return Spark::Net::NetworkManager::GetInstance().GetStats();
}

#endif // ENABLE_NETWORKING
