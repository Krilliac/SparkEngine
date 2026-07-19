/**
 * @file SeamlessAreaManager.cpp
 * @brief Predictive asset streaming implementation
 *
 * Uses player velocity and camera direction to predict future position,
 * then pre-loads areas along the predicted path. Areas beyond the unload
 * radius are released to free memory.
 */

#include "SeamlessAreaManager.h"
#include "DirectStorageLoader.h"
#include "../../Core/FaultIsolation.h"
#include "../../Utils/DebugHookManager.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/SparkConsole.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace Spark::Streaming
{

    // ========================================================================
    // Lifecycle
    // ========================================================================

    void SeamlessAreaManager::Initialize()
    {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "Streaming", 0.0);
        if (m_initialized)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Scene, "SeamlessAreaManager already initialized");
            return;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "SeamlessAreaManager::Initialize");

        m_areas.clear();
        m_loadedAreaIds.clear();
        m_loadQueue.clear();
        m_currentAreaId = INVALID_AREA_ID;
        m_activeLoadCount = 0;
        m_timeSinceLastUpdate = 0.0f;

        // Initialize DirectStorageLoader for async I/O
        DirectStorageLoader::GetInstance().Initialize();

        // Initialize the asset loader bridge
        m_assetLoader.Initialize();

        m_initialized = true;

        Spark::SimpleConsole::GetInstance().LogInfo("[SeamlessAreaManager] Initialized");
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "Streaming", 0.0);
    }

    void SeamlessAreaManager::Update(float dt)
    {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "Streaming", 0.0);
        if (!m_initialized)
        {
            return;
        }

        // Poll async I/O completions every frame (not gated by updateInterval)
        m_assetLoader.Update();

        m_timeSinceLastUpdate += dt;
        if (m_timeSinceLastUpdate < m_config.updateInterval)
        {
            SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Streaming", 0.0);
            return;
        }
        m_timeSinceLastUpdate = 0.0f;

        // Determine which area the player is currently in
        XMFLOAT3 playerPos;
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            playerPos = m_playerPosition;
        }
        m_currentAreaId = FindContainingArea(playerPos);

        // Update distances from player (and predicted position) to each area
        UpdateAreaDistances();

        // Queue areas that are within load range but not yet loaded
        ProcessLoadQueue();

        // Unload areas that are beyond the unload radius
        ProcessUnloadQueue();
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Streaming", 0.0);
    }

    void SeamlessAreaManager::Shutdown()
    {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "Streaming", 0.0);
        if (!m_initialized)
        {
            return;
        }

        // Transition all loaded areas to unloaded
        for (auto& [id, area] : m_areas)
        {
            if (area.state == AreaState::Loaded || area.state == AreaState::Loading)
            {
                TransitionAreaState(area, AreaState::Unloaded);
            }
        }

        m_areas.clear();
        m_loadedAreaIds.clear();
        m_loadQueue.clear();
        m_stateCallbacks.clear();

        // Shutdown asset loader and DirectStorageLoader
        m_assetLoader.Shutdown();
        DirectStorageLoader::GetInstance().Shutdown();

        m_initialized = false;

        Spark::SimpleConsole::GetInstance().LogInfo("[SeamlessAreaManager] Shutdown");
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "Streaming", 0.0);
    }

    // ========================================================================
    // Area Registration
    // ========================================================================

    void SeamlessAreaManager::RegisterArea(const AreaDefinition& def)
    {
        ManagedArea managed;
        managed.definition = def;
        managed.state = AreaState::Unloaded;
        m_areas[def.areaId] = std::move(managed);
        m_registeredHistory.insert(def.areaId);
    }

    void SeamlessAreaManager::RegisterArea(const AreaDefinition& def, SceneManifest manifest)
    {
        RegisterArea(def);
        m_assetLoader.SetManifest(def.areaId, std::move(manifest));
    }

    void SeamlessAreaManager::SetManifest(AreaID areaId, SceneManifest manifest)
    {
        m_assetLoader.SetManifest(areaId, std::move(manifest));
    }

    void SeamlessAreaManager::UnregisterArea(AreaID areaId)
    {
        auto it = m_areas.find(areaId);
        if (it == m_areas.end())
        {
            bool wasEverRegistered = m_registeredHistory.contains(areaId);
            SPARK_LOG_WARN(Spark::LogCategory::Scene,
                           "[SeamlessAreaManager] UnregisterArea: area %u not found. Was it ever registered? %s",
                           areaId, wasEverRegistered ? "YES (previously unregistered)" : "NO (never registered)");
            return;
        }

        // Release any assets/DirectStorage state the loader still holds for this
        // area, otherwise its load state (submitted handles + loaded CPU data)
        // leaks until engine Shutdown. BeginAreaUnload cancels handles and frees
        // data; the empty callback is safe (the loader null-checks it). The
        // in-flight-load callback re-checks m_areas.find, so there is no double free.
        if (it->second.state == AreaState::Loaded || it->second.state == AreaState::Loading)
        {
            m_assetLoader.BeginAreaUnload(areaId, {});
        }
        // Drop the manifest regardless of load state — a registered-but-unloaded
        // area may still have one from RegisterArea(def, manifest).
        m_assetLoader.RemoveManifest(areaId);

        // Remove from loaded list if present
        if (it->second.state == AreaState::Loaded)
        {
            auto loadedIt = std::find(m_loadedAreaIds.begin(), m_loadedAreaIds.end(), areaId);
            if (loadedIt != m_loadedAreaIds.end())
            {
                m_loadedAreaIds.erase(loadedIt);
            }
        }

        m_areas.erase(it);
    }

    // ========================================================================
    // Player State
    // ========================================================================

    void SeamlessAreaManager::SetPlayerState(const XMFLOAT3& position, const XMFLOAT3& velocity,
                                             const XMFLOAT3& cameraDir)
    {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        m_playerPosition = position;
        m_playerVelocity = velocity;
        m_cameraDirection = cameraDir;
    }

    // ========================================================================
    // Queries
    // ========================================================================

    AreaState SeamlessAreaManager::GetAreaState(AreaID areaId) const
    {
        auto it = m_areas.find(areaId);
        if (it == m_areas.end())
        {
            return AreaState::Unloaded;
        }
        return it->second.state;
    }

    void SeamlessAreaManager::RegisterStateCallback(AreaStateChangedCallback callback)
    {
        m_stateCallbacks.push_back(std::move(callback));
    }

    // Prediction and streaming logic (PredictFuturePosition, DistanceToArea,
    // SnapshotPlayerDirection, DirectionalEffectiveDistance, FindContainingArea,
    // UpdateAreaDistances, ProcessLoadQueue, ProcessUnloadQueue,
    // TransitionAreaState) live in SeamlessAreaManagerStreaming.cpp.

    // ========================================================================
    // Diagnostics
    // ========================================================================

    std::string SeamlessAreaManager::Console_GetStatus() const
    {
        std::string result = "SeamlessAreaManager:\n";
        result += std::format("  Areas registered: {}\n", m_areas.size());
        result += std::format("  Areas loaded: {}\n", m_loadedAreaIds.size());
        result += std::format("  Current area: {}\n", m_currentAreaId);

        XMFLOAT3 pos;
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            pos = m_playerPosition;
        }
        result += std::format("  Player position: ({:.1f}, {:.1f}, {:.1f})\n", pos.x, pos.y, pos.z);

        for (const auto& [id, area] : m_areas)
        {
            const char* stateStr = "Unloaded";
            switch (area.state)
            {
            case AreaState::Loading:
                stateStr = "Loading";
                break;
            case AreaState::Loaded:
                stateStr = "Loaded";
                break;
            case AreaState::Unloading:
                stateStr = "Unloading";
                break;
            default:
                break;
            }
            result += std::format("  [{}] {} — {} (dist={:.1f})\n", id, area.definition.name, stateStr,
                                  area.distanceToPlayer);
        }

        return result;
    }

} // namespace Spark::Streaming
