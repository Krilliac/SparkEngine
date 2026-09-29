/**
 * @file GameConsoleOps.cpp
 * @brief Console integration, graphics settings, scene management, class system,
 *        and combat arena methods for the Game class.
 *
 * Extracted from Game.cpp to keep each file focused on one cohesive responsibility.
 */

#include "Core/Platform.h"
#include "Core/FPSLog.h"
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
#include "Game/CubeObject.h"
#include "Game/PlaneObject.h"
#include "Game/SphereObject.h"
#include "ModelObject.h"
#include "FPSAssetPaths.h"
#include "Enemy.h"
#include "Player.h"
#include "Projectiles/ProjectilePool.h"
#include "SceneManager/SceneManager.h"
#include "Engine/Networking/NetworkManager.h"
#include "Input/InputManager.h"
#include "MultiplayerSystem.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <system_error>


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

    std::string settingsMsg = "Physics updated - Gravity: " + std::to_string(gravity) +
                              ", Speed: " + std::to_string(playerSpeed) + ", Jump: " + std::to_string(jumpHeight) +
                              ", Friction: " + std::to_string(friction);
    FPS_CONSOLE(settingsMsg, "SUCCESS");
}

void Game::ApplyCameraSettings(float fov, float sensitivity, bool invertY)
{
    if (!m_camera)
    {
        FPS_CONSOLE("Camera settings failed - camera not available", "ERROR");
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

    std::string cameraMsg = "Camera settings applied - FOV: " + std::to_string(fov) +
                            ", Sensitivity: " + std::to_string(sensitivity) + ", InvertY: " + (invertY ? "ON" : "OFF");
    FPS_CONSOLE(cameraMsg, "SUCCESS");
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

    std::string debugMsg = "Debug settings applied - God Mode: " + (godMode ? std::string("ON") : std::string("OFF")) +
                           ", Noclip: " + (noclip ? std::string("ON") : std::string("OFF")) +
                           ", Infinite Ammo: " + (infiniteAmmo ? std::string("ON") : std::string("OFF"));
    FPS_CONSOLE(debugMsg, "SUCCESS");
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
    FPS_CONSOLE("Teleporting player via console integration", "INFO");

    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    {
        FPS_CONSOLE("Teleport refused - coordinates must be finite", "ERROR");
        return;
    }

    if (m_camera)
    {
        m_camera->SetPosition({x, y, z});

        std::string teleportMsg =
            "Player teleported to (" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ")";
        FPS_CONSOLE(teleportMsg, "SUCCESS");
    }
    else
    {
        FPS_CONSOLE("Teleport failed - camera not available", "ERROR");
    }
}

bool Game::SpawnObject(const std::string& type, float x, float y, float z)
{
    FPS_CONSOLE("Spawning object via console integration", "INFO");

    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    {
        FPS_CONSOLE("Spawn refused - coordinates must be finite", "ERROR");
        return false;
    }

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
        std::string errorMsg = "Unknown object type: " + std::string(type.begin(), type.end());
        FPS_CONSOLE(errorMsg, "ERROR");
        return false;
    }

    if (newObject)
    {
        HRESULT hr = newObject->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
        if (SUCCEEDED(hr))
        {
            newObject->SetPosition({x, y, z});
            m_gameObjects.push_back(std::move(newObject));

            std::string spawnMsg = "Spawned " + std::string(type.begin(), type.end()) + " at (" + std::to_string(x) +
                                   ", " + std::to_string(y) + ", " + std::to_string(z) + ")";
            FPS_CONSOLE(spawnMsg, "SUCCESS");
            return true;
        }
        else
        {
            std::string errorMsg = "Failed to initialize spawned object, HR=0x" + std::to_string(hr);
            FPS_CONSOLE(errorMsg, "ERROR");
        }
    }

    return false;
}

bool Game::DeleteObject(size_t index)
{
    if (!SPARK_BOUNDS_CHECK(index, m_gameObjects.size()))
    {
        FPS_LOG_EVERY_SECONDS(Error, 5, "DeleteObject: index {} out of bounds (size={})", index, m_gameObjects.size());
        return false;
    }

    const GameObject* removedObject = m_gameObjects[index].get();
    std::erase_if(m_enemies, [removedObject](const Enemy* enemy) { return enemy == removedObject; });
    m_gameObjects.erase(m_gameObjects.begin() + index);

    std::string deleteMsg = "Deleted object at index " + std::to_string(index) +
                            ". Remaining objects: " + std::to_string(m_gameObjects.size());
    FPS_CONSOLE(deleteMsg, "SUCCESS");
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

    std::string clearMsg = "Cleared " + std::to_string(originalCount) + " objects from scene";
    if (keepPlayer)
        clearMsg += " (player preserved)";
    FPS_CONSOLE(clearMsg, "SUCCESS");
}

void Game::SetTimeScale(float scale)
{
    // NaN fails both range comparisons below and would be stored as-is, making
    // every later Update() run with dt = NaN. Reject it instead of clamping.
    if (!std::isfinite(scale))
    {
        FPS_CONSOLE("Time scale must be a finite number; unchanged", "WARNING");
        return;
    }
    if (scale < 0.1f || scale > 10.0f)
    {
        FPS_CONSOLE("Time scale out of range (0.1-10.0), clamping", "WARNING");
        scale = std::max(0.1f, std::min(10.0f, scale));
    }

    m_timeScale = scale;

    std::string scaleMsg = "Time scale set to " + std::to_string(scale) + "x";
    FPS_CONSOLE(scaleMsg, "SUCCESS");
}

// ============================================================================
// ENHANCED GRAPHICS INTEGRATION METHODS - Full Implementation
// ============================================================================

void Game::ApplyGraphicsSettings(bool wireframe, bool vsync, bool showFPS)
{
    FPS_CONSOLE("Applying graphics settings via console integration", "INFO");

    if (m_graphics)
    {
        try
        {
            m_graphics->Console_SetWireframe(wireframe);
            m_graphics->Console_SetVSync(vsync);
            m_showFPS = showFPS;

            std::string graphicsMsg =
                "Graphics settings applied - Wireframe: " + (wireframe ? std::string("ON") : std::string("OFF")) +
                ", VSync: " + (vsync ? std::string("ON") : std::string("OFF")) +
                ", Show FPS: " + (showFPS ? std::string("ON") : std::string("OFF"));
            FPS_CONSOLE(graphicsMsg, "SUCCESS");
        }
        catch (...)
        {
            FPS_CONSOLE("Failed to apply graphics settings", "ERROR");
        }
    }
    else
    {
        FPS_CONSOLE("Graphics settings failed - graphics engine not available", "ERROR");
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
    FPS_CONSOLE("Refreshing graphics settings via console integration", "INFO");

    if (m_graphics)
    {
        try
        {
            // Trigger a refresh of graphics state
            m_graphics->Console_ResetDevice();
            FPS_CONSOLE("Graphics settings refreshed successfully", "SUCCESS");
        }
        catch (...)
        {
            FPS_CONSOLE("Failed to refresh graphics settings", "ERROR");
        }
    }
    else
    {
        FPS_CONSOLE("Graphics refresh failed - graphics engine not available", "ERROR");
    }
}

// ============================================================================
// ENHANCED SCENE MANAGEMENT METHODS - Full Implementation
// ============================================================================

void Game::RefreshAuthoredSceneRuntimeState()
{
    if (!m_sceneManager)
        return;

    const SceneNode* authoredCamera = nullptr;
    for (int i = 0; i < m_sceneManager->GetNodeCount(); ++i)
    {
        const SceneNode* node = m_sceneManager->GetNode(i);
        if (!node || node->type != "Camera")
            continue;
        const auto projection = node->properties.find("projection");
        if (projection != node->properties.end() && projection->second != "perspective")
            continue;
        if (!authoredCamera)
            authoredCamera = node;
        const auto main = node->properties.find("isMain");
        if (main != node->properties.end() && (main->second == "true" || main->second == "1"))
        {
            authoredCamera = node;
            break;
        }
    }

    if (m_camera)
    {
        m_camera->SetPosition(authoredCamera ? authoredCamera->position : DirectX::XMFLOAT3{0.0f, 2.0f, -20.0f});
        if (authoredCamera)
        {
            m_camera->Console_SetRotation(authoredCamera->rotation.x, authoredCamera->rotation.y,
                                          authoredCamera->rotation.z);
            const auto nearProperty = authoredCamera->properties.find("nearPlane");
            const auto farProperty = authoredCamera->properties.find("farPlane");
            float nearPlane = 0.0f;
            float farPlane = 0.0f;
            if (nearProperty != authoredCamera->properties.end() && farProperty != authoredCamera->properties.end() &&
                ParseAuthoredFiniteFloat(nearProperty->second, nearPlane) &&
                ParseAuthoredFiniteFloat(farProperty->second, farPlane) && nearPlane >= 0.01f && nearPlane <= 10.0f &&
                farPlane >= 100.0f && farPlane <= 10000.0f && nearPlane < farPlane)
            {
                m_camera->Console_SetClippingPlanes(nearPlane, farPlane);
            }
        }
        if (m_player)
            m_player->SetPosition(m_camera->GetPosition());
    }

    // Rebuild authored spawn bindings while preserving the existing player,
    // score, and event subscriptions.  InitializeRespawnAndVehicles creates a
    // fresh spawn state and reinstalls the player's death callback.
    if (m_respawnSystem)
        InitializeRespawnAndVehicles();

    if (m_waveSpawner)
    {
        std::vector<DirectX::XMFLOAT3> waveSpawns;
        for (int i = 0; i < m_sceneManager->GetNodeCount(); ++i)
        {
            const SceneNode* node = m_sceneManager->GetNode(i);
            if (!node || node->type != "SpawnPoint")
                continue;
            const auto tag = node->properties.find("tag");
            if (tag != node->properties.end() && tag->second == "wave_spawn")
                waveSpawns.push_back(node->position);
        }
        m_waveSpawner->Initialize(waveSpawns);
    }
}

bool Game::LoadScene(const std::string& scenePath)
{
    FPS_CONSOLE("Loading scene via console integration", "INFO");

    if (!m_sceneManager)
    {
        FPS_CONSOLE("Scene load failed - scene manager not available", "ERROR");
        return false;
    }

    try
    {
        std::filesystem::path trustedScenePath;
        std::string pathError;
        if (!Spark::FPSAssets::ResolveScenePath(scenePath, trustedScenePath, pathError))
        {
            FPS_CONSOLE("Scene load rejected: " + pathError, "ERROR");
            return false;
        }
        const std::wstring wScenePath = trustedScenePath.wstring();
        bool success = m_sceneManager->LoadScene(wScenePath);
        const std::u8string scenePathU8 = trustedScenePath.u8string();
        const std::string scenePathUtf8(reinterpret_cast<const char*>(scenePathU8.data()), scenePathU8.size());

        if (success)
        {
            std::error_code arenaIdentityError;
            const bool isBuiltInArena =
                std::filesystem::equivalent(trustedScenePath, Spark::FPSAssets::Root() / "Scenes" / "level1.scene",
                                            arenaIdentityError) &&
                !arenaIdentityError;
            BindSceneMaterialRoots();
            // A successful replacement discards objects from the old level.
            // The built-in FPS level also has a legacy combat-arena layer
            // created at startup; recreate that same layer on reload instead
            // of leaving the player in a mostly empty blue scene.
            m_enemies.clear();
            m_gameObjects.clear();
            if (m_renderingEnabled && isBuiltInArena)
            {
                CreateCombatArena();
                for (auto& object : m_gameObjects)
                {
                    if (auto* model = dynamic_cast<ModelObject*>(object.get()))
                        model->SetGraphicsEngine(m_graphics);
                }
                BindSceneMaterialRoots();
            }
            // Invalidate only after the replacement scene and its procedural
            // layer have their trusted roots bound, so the next render parses
            // each newly referenced material from disk.
            InvalidateSceneBasicMaterials();
            RefreshAuthoredSceneRuntimeState();

            std::string loadMsg = "Scene loaded successfully: " + scenePathUtf8;
            FPS_CONSOLE(loadMsg, "SUCCESS");
        }
        else
        {
            std::string loadMsg = "Failed to load scene: " + scenePathUtf8;
            FPS_CONSOLE(loadMsg, "ERROR");
        }

        return success;
    }
    catch (...)
    {
        std::string errorMsg =
            "Exception occurred while loading scene: " + std::string(scenePath.begin(), scenePath.end());
        FPS_CONSOLE(errorMsg, "ERROR");
        return false;
    }
}

bool Game::SaveScene(const std::string& scenePath)
{
    FPS_CONSOLE("Saving scene via console integration", "INFO");

    if (!m_sceneManager)
    {
        FPS_CONSOLE("Scene save failed - scene manager not available", "ERROR");
        return false;
    }

    // Accept the same spellings as scene_load ("x.scene", "Scenes/x.scene",
    // "Assets/Scenes/x.scene") and write only below the trusted FPS scene
    // directory. SaveSceneWithinRoot rejects absolute paths, traversal and any
    // symlink/junction component, and is race-free against a component being
    // swapped mid-save. Only .scene is accepted so scene_load can reload it.
    if (scenePath.empty() || scenePath.size() > 4096 || scenePath.find('\0') != std::string::npos)
    {
        FPS_CONSOLE("Scene save rejected: malformed scene path", "ERROR");
        return false;
    }
    std::string normalized = scenePath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    for (const std::string_view prefix : {std::string_view("Assets/Scenes/"), std::string_view("Scenes/")})
    {
        if (normalized.starts_with(prefix))
        {
            normalized.erase(0, prefix.size());
            break;
        }
    }
    const std::filesystem::path relative(
        std::u8string(reinterpret_cast<const char8_t*>(normalized.data()), normalized.size()));
    if (relative.extension() != ".scene")
    {
        FPS_CONSOLE("Scene save rejected (expected a relative .scene path): " + normalized, "ERROR");
        return false;
    }

    const bool saved = m_sceneManager->SaveSceneWithinRoot(Spark::FPSAssets::Root() / "Scenes", relative);
    if (saved)
    {
        FPS_CONSOLE("Scene saved: Scenes/" + normalized, "SUCCESS");
    }
    else
    {
        FPS_CONSOLE("Scene save failed or was refused: " + normalized, "ERROR");
    }
    return saved;
}

std::vector<std::string> Game::GetAvailableScenes() const
{
    std::vector<std::string> scenes;

    // Scan common scene directories for .scene / .xml / .json files
    // Both directories and results are UTF-8 (the SaveScene path above decodes the
    // same way). The narrow path(std::string)/path::string() conversions use the
    // Windows ANSI code page instead, and one scene name it cannot spell threw out of
    // the loop and dropped the rest of that directory's listing.
    const std::string sceneDirs[] = {Spark::FPSAssets::ResolveUtf8("Scenes"), "Scenes"};
    for (const auto& dir : sceneDirs)
    {
        try
        {
            const std::filesystem::path directory(
                std::u8string(reinterpret_cast<const char8_t*>(dir.data()), dir.size()));
            if (!std::filesystem::exists(directory))
                continue;
            for (const auto& entry : std::filesystem::directory_iterator(directory))
            {
                if (!entry.is_regular_file())
                    continue;
                const auto ext = entry.path().extension();
                if (ext == ".scene" || ext == ".xml" || ext == ".json")
                {
                    // u8string() throws only for a name that is not well-formed UTF-16;
                    // skip that one entry, not the rest of the directory.
                    try
                    {
                        const std::u8string scenePath = entry.path().u8string();
                        scenes.emplace_back(reinterpret_cast<const char*>(scenePath.data()), scenePath.size());
                    }
                    catch (const std::system_error& error)
                    {
                        FPS_LOG_WARN("Skipping a scene file whose name is not UTF-8: {}", error.what());
                    }
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

        std::string classMsg = "Class changed to: " + std::string(def.name.begin(), def.name.end());
        FPS_CONSOLE(classMsg, "SUCCESS");
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

    std::string ProceduralMaterialFor(const wchar_t* modelPath, const std::string& name)
    {
        if (name == "Center_Building")
            return "Assets/Materials/Arena_CenterBuilding.json";

        const std::wstring path(modelPath ? modelPath : L"");
        // Training-kit props (Assets/Models/FPS/Kit) carry palette colours in their MTL files, but the D3D11
        // Model ignores MTL data, so give each one the procedural material of its dominant surface.
        if (path.find(L"FPS/Kit/") != std::wstring::npos)
        {
            const bool concrete =
                path.find(L"cover_barrier") != std::wstring::npos || path.find(L"spawn_pad") != std::wstring::npos;
            return concrete ? "Assets/Materials/Concrete.json" : "Assets/Materials/Metal.json";
        }
        if (path.find(L"crate.obj") != std::wstring::npos)
            return "Assets/Materials/Wood.json";
        if (path.find(L"target.obj") != std::wstring::npos || path.find(L"rifle.obj") != std::wstring::npos ||
            path.find(L"sniper.obj") != std::wstring::npos || path.find(L"lmg.obj") != std::wstring::npos ||
            path.find(L"shotgun.obj") != std::wstring::npos || path.find(L"pistol.obj") != std::wstring::npos)
            return "Assets/Materials/Metal.json";
        if (path.find(L"building_small.obj") != std::wstring::npos || path.find(L"barrier.obj") != std::wstring::npos ||
            path.find(L"watchtower.obj") != std::wstring::npos || path.find(L"character.obj") != std::wstring::npos)
            return "Assets/Materials/Concrete.json";
        return {};
    }

    /// Helper: create a ModelObject, initialize it, set position/name, and add to the list
    void PlaceModel(const wchar_t* modelPath, const std::string& name, XMFLOAT3 pos, ID3D11Device* device,
                    ID3D11DeviceContext* context, std::vector<std::unique_ptr<GameObject>>& objects,
                    XMFLOAT3 scale = {1.0f, 1.0f, 1.0f}, float yaw = 0.0f)
    {
        auto obj = std::make_unique<ModelObject>(Spark::FPSAssets::Resolve(modelPath));
        HRESULT hr = obj->Initialize(device, context);
        if (FAILED(hr))
            return;
        obj->SetPosition(pos);
        obj->SetName(name);
        obj->SetMaterialPath(ProceduralMaterialFor(modelPath, name));
        if (scale.x != 1.0f || scale.y != 1.0f || scale.z != 1.0f)
            obj->SetScale(scale);
        if (yaw != 0.0f)
            obj->SetRotation({0.0f, yaw, 0.0f});
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
    FPS_CONSOLE("Creating enhanced combat arena level...", "INFO");

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
            ground->SetMaterialPath("Assets/Materials/Terrain_Dirt.json");
            m_gameObjects.push_back(std::move(ground));
        }
    }

    // === ALPHA BASE (south, z = -70) ===
    PlaceModel(L"Models/building_small.obj", "Alpha_HQ", {-8.0f, 0.0f, -70.0f}, device, context, m_gameObjects);
    for (int i = -1; i <= 1; ++i)
    {
        PlaceModel(L"Models/barrier.obj", "Alpha_Barrier_" + std::to_string(i + 2), {i * 15.0f, 0.0f, -65.0f}, device,
                   context, m_gameObjects);
    }
    PlaceModel(L"Models/crate.obj", "Alpha_Crate_1", {-5.0f, 0.0f, -68.0f}, device, context, m_gameObjects);
    PlaceModel(L"Models/crate.obj", "Alpha_Crate_2", {5.0f, 0.0f, -68.0f}, device, context, m_gameObjects);

    // === BRAVO BASE (north, z = 70) ===
    PlaceModel(L"Models/building_small.obj", "Bravo_HQ", {8.0f, 0.0f, 70.0f}, device, context, m_gameObjects);
    for (int i = -1; i <= 1; ++i)
    {
        PlaceModel(L"Models/barrier.obj", "Bravo_Barrier_" + std::to_string(i + 2), {i * 15.0f, 0.0f, 65.0f}, device,
                   context, m_gameObjects);
    }
    PlaceModel(L"Models/crate.obj", "Bravo_Crate_1", {-5.0f, 0.0f, 68.0f}, device, context, m_gameObjects);
    PlaceModel(L"Models/crate.obj", "Bravo_Crate_2", {5.0f, 0.0f, 68.0f}, device, context, m_gameObjects);

    // === CENTER OBJECTIVE (z = 0) ===
    PlaceModel(L"Models/building_small.obj", "Center_Building", {0.0f, 0.0f, 0.0f}, device, context, m_gameObjects,
               {1.2f, 1.0f, 1.2f});
    {
        const float coverPos[][3] = {
            {-8.0f, 0.0f, -3.0f}, {8.0f, 0.0f, 3.0f}, {-3.0f, 0.0f, 8.0f}, {3.0f, 0.0f, -8.0f}};
        PlaceModelsAt(L"Models/barrier.obj", "Center_Barrier_", coverPos, device, context, m_gameObjects);

        const float cratePos[][3] = {
            {-3.0f, 0.0f, 5.0f}, {3.0f, 0.0f, -5.0f}, {-6.0f, 0.0f, -6.0f}, {6.0f, 0.0f, 6.0f}};
        PlaceModelsAt(L"Models/crate.obj", "Center_Crate_", cratePos, device, context, m_gameObjects);
    }

    // === WEST OUTPOST (x = -45) ===
    PlaceModel(L"Models/watchtower.obj", "West_Tower", {-45.0f, 0.0f, 0.0f}, device, context, m_gameObjects);
    PlaceModel(L"Models/barrier.obj", "West_Cover_1", {-40.0f, 0.0f, -5.0f}, device, context, m_gameObjects);
    PlaceModel(L"Models/barrier.obj", "West_Cover_2", {-40.0f, 0.0f, 5.0f}, device, context, m_gameObjects);
    PlaceModel(L"Models/crate.obj", "West_Crate_1", {-48.0f, 0.0f, -2.0f}, device, context, m_gameObjects);
    PlaceModel(L"Models/crate.obj", "West_Crate_2", {-48.0f, 0.0f, 2.0f}, device, context, m_gameObjects);

    // === EAST OUTPOST (x = 45) ===
    PlaceModel(L"Models/watchtower.obj", "East_Tower", {45.0f, 0.0f, 0.0f}, device, context, m_gameObjects);
    PlaceModel(L"Models/barrier.obj", "East_Cover_1", {40.0f, 0.0f, -5.0f}, device, context, m_gameObjects);
    PlaceModel(L"Models/barrier.obj", "East_Cover_2", {40.0f, 0.0f, 5.0f}, device, context, m_gameObjects);
    PlaceModel(L"Models/crate.obj", "East_Crate_1", {48.0f, 0.0f, -2.0f}, device, context, m_gameObjects);
    PlaceModel(L"Models/crate.obj", "East_Crate_2", {48.0f, 0.0f, 2.0f}, device, context, m_gameObjects);

    // === MID-FIELD COVER (scattered barriers and crates) ===
    {
        const float barrierPos[][3] = {{-25.0f, 0.0f, -30.0f}, {25.0f, 0.0f, -30.0f}, {-25.0f, 0.0f, 30.0f},
                                       {25.0f, 0.0f, 30.0f},   {-20.0f, 0.0f, 0.0f},  {20.0f, 0.0f, 0.0f},
                                       {0.0f, 0.0f, -35.0f},   {0.0f, 0.0f, 35.0f}};
        PlaceModelsAt(L"Models/barrier.obj", "Field_Barrier_", barrierPos, device, context, m_gameObjects);

        const float cratePos[][3] = {{-15.0f, 0.0f, -15.0f}, {15.0f, 0.0f, -15.0f},  {-15.0f, 0.0f, 15.0f},
                                     {15.0f, 0.0f, 15.0f},   {-30.0f, 0.0f, -50.0f}, {30.0f, 0.0f, -50.0f},
                                     {-30.0f, 0.0f, 50.0f},  {30.0f, 0.0f, 50.0f}};
        PlaceModelsAt(L"Models/crate.obj", "Field_Crate_", cratePos, device, context, m_gameObjects);
    }

    // === TARGET PRACTICE AREA (west side) ===
    for (int i = 0; i < 5; ++i)
    {
        PlaceModel(L"Models/target.obj", "Target_" + std::to_string(i + 1), {-55.0f, 0.0f, -10.0f + i * 5.0f}, device,
                   context, m_gameObjects);
    }

    // === CHARACTER MODELS (NPCs / bots placeholder) ===
    {
        const float npcPos[][3] = {
            {0.0f, 0.0f, 20.0f}, {10.0f, 0.0f, -20.0f}, {-10.0f, 0.0f, 30.0f}, {20.0f, 0.0f, -40.0f}};
        PlaceModelsAt(L"Models/character.obj", "NPC_", npcPos, device, context, m_gameObjects);
    }

    // === DECORATIVE SPHERES (control point markers) ===
    {
        const float cpPositions[][3] = {{0.0f, 3.0f, 0.0f}, {-45.0f, 3.0f, 0.0f}, {45.0f, 3.0f, 0.0f}};
        for (int i = 0; i < 3; ++i)
        {
            auto sphere = std::make_unique<SphereObject>(0.5f, 12, 12);
            if (SUCCEEDED(sphere->Initialize(device, context)))
            {
                sphere->SetPosition({cpPositions[i][0], cpPositions[i][1], cpPositions[i][2]});
                sphere->SetName("ControlPoint_" + std::to_string(i + 1));
                sphere->SetMaterialPath("Assets/Materials/Metal.json");
                m_gameObjects.push_back(std::move(sphere));
            }
        }
    }

    // === WEAPON DISPLAYS (at spawn) ===
    {
        const wchar_t* weaponModels[] = {L"Models/rifle.obj", L"Models/sniper.obj", L"Models/lmg.obj",
                                         L"Models/shotgun.obj", L"Models/pistol.obj"};
        const char* weaponNames[] = {"Rifle_Display", "Sniper_Display", "LMG_Display", "Shotgun_Display",
                                     "Pistol_Display"};
        for (int i = 0; i < 5; ++i)
        {
            PlaceModel(weaponModels[i], weaponNames[i], {-3.0f + i * 1.5f, 1.2f, -72.0f}, device, context,
                       m_gameObjects, {3.0f, 3.0f, 3.0f});
        }
    }

    // === TRAINING KIT (tools/blender/author_fps_kit.py -> Art/Blender/SparkGameFPS) ===
    // Kit props face +Z; the yaw turns each one toward the play space. Spawn pads mark the default spawns
    // of Scenes/level1.scene (the east/west pads sit 1.6 m toward +Z of theirs, clear of Field_Barrier_5/6 at
    // x = +/-20), racks and ammo stand at the back of each base (behind Alpha's weapon displays), and the
    // dummies stagger behind the practice targets facing the shooting lane (+X).
    {
        struct KitPlacement
        {
            const wchar_t* model;
            const char* name;
            XMFLOAT3 position;
            float yaw;
        };
        const KitPlacement kitPlacements[] = {
            {L"Models/FPS/Kit/spawn_pad.obj", "North_SpawnPad", {0.0f, 0.0f, -20.0f}, 0.0f},
            {L"Models/FPS/Kit/spawn_pad.obj", "South_SpawnPad", {0.0f, 0.0f, 20.0f}, XM_PI},
            {L"Models/FPS/Kit/spawn_pad.obj", "East_SpawnPad", {20.0f, 0.0f, 1.6f}, -XM_PIDIV2},
            {L"Models/FPS/Kit/spawn_pad.obj", "West_SpawnPad", {-20.0f, 0.0f, 1.6f}, XM_PIDIV2},
            {L"Models/FPS/Kit/cover_barrier.obj", "North_SpawnCover_1", {-4.0f, 0.0f, -16.0f}, 0.0f},
            {L"Models/FPS/Kit/cover_barrier.obj", "North_SpawnCover_2", {4.0f, 0.0f, -16.0f}, 0.0f},
            {L"Models/FPS/Kit/cover_barrier.obj", "South_SpawnCover_1", {-4.0f, 0.0f, 16.0f}, XM_PI},
            {L"Models/FPS/Kit/cover_barrier.obj", "South_SpawnCover_2", {4.0f, 0.0f, 16.0f}, XM_PI},
            {L"Models/FPS/Kit/weapon_rack.obj", "Alpha_WeaponRack_1", {-0.8f, 0.0f, -73.5f}, 0.0f},
            {L"Models/FPS/Kit/weapon_rack.obj", "Alpha_WeaponRack_2", {0.8f, 0.0f, -73.5f}, 0.0f},
            {L"Models/FPS/Kit/ammo_crate.obj", "Alpha_AmmoCrate_1", {-2.4f, 0.0f, -73.6f}, 0.0f},
            {L"Models/FPS/Kit/ammo_crate.obj", "Alpha_AmmoCrate_2", {2.4f, 0.0f, -73.6f}, 0.0f},
            {L"Models/FPS/Kit/weapon_rack.obj", "Bravo_WeaponRack_1", {-0.8f, 0.0f, 73.5f}, XM_PI},
            {L"Models/FPS/Kit/weapon_rack.obj", "Bravo_WeaponRack_2", {0.8f, 0.0f, 73.5f}, XM_PI},
            {L"Models/FPS/Kit/ammo_crate.obj", "Bravo_AmmoCrate_1", {-2.4f, 0.0f, 73.6f}, XM_PI},
            {L"Models/FPS/Kit/ammo_crate.obj", "Bravo_AmmoCrate_2", {2.4f, 0.0f, 73.6f}, XM_PI},
            {L"Models/FPS/Kit/target_dummy.obj", "Target_Dummy_1", {-58.0f, 0.0f, -7.5f}, XM_PIDIV2},
            {L"Models/FPS/Kit/target_dummy.obj", "Target_Dummy_2", {-58.0f, 0.0f, -2.5f}, XM_PIDIV2},
            {L"Models/FPS/Kit/target_dummy.obj", "Target_Dummy_3", {-58.0f, 0.0f, 2.5f}, XM_PIDIV2},
            {L"Models/FPS/Kit/target_dummy.obj", "Target_Dummy_4", {-58.0f, 0.0f, 7.5f}, XM_PIDIV2},
        };
        for (const auto& placement : kitPlacements)
        {
            PlaceModel(placement.model, placement.name, placement.position, device, context, m_gameObjects,
                       {1.0f, 1.0f, 1.0f}, placement.yaw);
        }
    }

    std::string totalMsg = "Combat arena created. Total objects: " + std::to_string(m_gameObjects.size());
    FPS_CONSOLE(totalMsg, "SUCCESS");
}

void Game::CreateTestScene(const std::string& sceneType)
{
    FPS_CONSOLE("Creating test scene via console integration", "INFO");

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
        FPS_CONSOLE("Creating performance test scene with many objects", "INFO");

        int objectsCreated = 0;
        for (int x = -10; x <= 10; x += 2)
        {
            for (int z = -10; z <= 10; z += 2)
            {
                auto cube = std::make_unique<CubeObject>(0.5f);
                HRESULT hr = cube->Initialize(m_graphics->GetDevice(), m_graphics->GetContext());
                if (SUCCEEDED(hr))
                {
                    cube->SetPosition({static_cast<float>(x), 0.5f, static_cast<float>(z)});
                    m_gameObjects.push_back(std::move(cube));
                    objectsCreated++;
                }
            }
        }

        std::string perfMsg = "Performance test scene created with " + std::to_string(objectsCreated) + " objects";
        FPS_CONSOLE(perfMsg, "SUCCESS");
    }
    else if (sceneType == "empty")
    {
        // Create empty scene (just clear objects)
        FPS_CONSOLE("Empty test scene created", "SUCCESS");
    }
    else
    {
        // Unknown scene type, create basic
        std::string unknownMsg =
            "Unknown scene type '" + std::string(sceneType.begin(), sceneType.end()) + "', creating basic scene";
        FPS_CONSOLE(unknownMsg, "WARNING");
        CreateTestObjects();
    }

    std::string sceneMsg = "Test scene created: " + std::string(sceneType.begin(), sceneType.end()) +
                           " (Total objects: " + std::to_string(m_gameObjects.size()) + ")";
    FPS_CONSOLE(sceneMsg, "SUCCESS");
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

        std::string msg =
            "Vehicle spawned: " + std::string(vehicle->GetVehicleName().begin(), vehicle->GetVehicleName().end()) +
            " at (" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + ")";
        FPS_CONSOLE(msg, "SUCCESS");
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

    FPS_CONSOLE_RATE_LIMITED(3, 3, "No vehicle nearby to enter", "INFO");
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
    auto& multiplayer = SparkFPS::FPSMultiplayerSystem::GetInstance();
    if (multiplayer.IsActive())
        multiplayer.Shutdown();

    multiplayer.Initialize(true);
    if (maxClients <= 0 || !multiplayer.StartServer(port, static_cast<uint32_t>(maxClients)))
    {
        FPS_CONSOLE("Failed to start server on port " + std::to_string(port), "ERROR");
        return false;
    }
    m_networkInitialized = true;
    m_networkInputAccumulator = 0.0f;

    FPS_CONSOLE("Server started on port " + std::to_string(port) + " (max " + std::to_string(maxClients) + " clients)",
                "SUCCESS");
    return true;
}

bool Game::ConnectToServer(const std::string& address, uint16_t port)
{
    auto& multiplayer = SparkFPS::FPSMultiplayerSystem::GetInstance();
    if (multiplayer.IsActive())
        multiplayer.Shutdown();

    FPS_CONSOLE("Connecting to " + address + ":" + std::to_string(port) + "...", "INFO");
    multiplayer.Initialize(false);
    if (!multiplayer.Connect(address, port))
    {
        FPS_CONSOLE("Failed to connect to " + address + ":" + std::to_string(port), "ERROR");
        return false;
    }
    m_networkInitialized = true;
    m_networkInputAccumulator = 0.0f;
    return true;
}

void Game::DisconnectNetwork()
{
    auto& multiplayer = SparkFPS::FPSMultiplayerSystem::GetInstance();
    if (!multiplayer.IsActive())
        return;

    const bool wasServer = multiplayer.IsServer();
    multiplayer.Shutdown();
    FPS_CONSOLE(wasServer ? "Server stopped" : "Disconnected from server", "INFO");
}

void Game::UpdateMultiplayer(float dt)
{
    auto& multiplayer = SparkFPS::FPSMultiplayerSystem::GetInstance();
    multiplayer.Update(dt);
    // Input goes out only once the session is live: a client whose handshake is still
    // pending has no id for the server to apply it to.
    if (!multiplayer.IsConnected() || !m_player || !m_input)
    {
        m_networkInputAccumulator = 0.0f;
        return;
    }

    // Each FPSMultiplayerSystem input is one 1/60 s step on both the client's prediction and
    // the server, so inputs go out at that fixed rate whatever the render frame rate. The cap
    // keeps a long hitch from sending a burst the server's input budget would drop anyway.
    constexpr float kInputStep = 1.0f / 60.0f;
    constexpr float kMaxBacklog = 0.1f;
    m_networkInputAccumulator = (std::min)(m_networkInputAccumulator + dt, kMaxBacklog);
    if (m_networkInputAccumulator < kInputStep)
        return;

    // The same bindings Player::HandleInput reads. Movement is suppressed in a vehicle or
    // while dead, matching what the local player can do.
    SparkFPS::PlayerInput input;
    const XMFLOAT3 forward = m_player->GetForwardDirection();
    input.yaw = std::atan2(forward.z, forward.x);
    input.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    if (!m_player->IsInVehicle() && m_player->IsAlive())
    {
        input.forward = (m_input->IsKeyDown('W') ? 1.0f : 0.0f) - (m_input->IsKeyDown('S') ? 1.0f : 0.0f);
        input.strafe = (m_input->IsKeyDown('D') ? 1.0f : 0.0f) - (m_input->IsKeyDown('A') ? 1.0f : 0.0f);
        input.jump = m_input->IsKeyDown(VK_SPACE);
        input.fire = m_input->IsMouseButtonDown(0);
        input.reload = m_input->IsKeyDown('R');
        input.crouch = m_input->IsKeyDown(VK_LCONTROL);
    }

    while (m_networkInputAccumulator >= kInputStep && multiplayer.IsConnected())
    {
        m_networkInputAccumulator -= kInputStep;
        multiplayer.SendInput(input);
    }
}

bool Game::IsNetworkActive() const
{
    return m_networkInitialized && SparkFPS::FPSMultiplayerSystem::GetInstance().IsActive();
}

std::string Game::GetNetworkStatus() const
{
    if (!m_networkInitialized)
        return "Networking not initialized";
    return SparkFPS::FPSMultiplayerSystem::GetInstance().Console_GetStatus() + "\n" +
           Spark::Net::NetworkManager::GetInstance().Console_GetStatus();
}

Spark::Net::NetworkStats Game::GetNetworkStats() const
{
    if (!m_networkInitialized)
        return {};
    return Spark::Net::NetworkManager::GetInstance().GetStats();
}

#endif // ENABLE_NETWORKING
