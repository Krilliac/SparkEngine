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

    // ========================================================================
    // Prediction
    // ========================================================================

    XMFLOAT3 SeamlessAreaManager::PredictFuturePosition() const
    {
        std::lock_guard<std::mutex> lock(m_playerMutex);

        // Extrapolate using velocity * lookahead, biased toward camera direction
        float vx = m_playerVelocity.x;
        float vy = m_playerVelocity.y;
        float vz = m_playerVelocity.z;

        float speed = std::sqrt(vx * vx + vy * vy + vz * vz);

        // If the player is nearly stationary, predict along camera direction
        // with a small default speed to still pre-load what they're looking at
        constexpr float MIN_SPEED = 0.5f;
        constexpr float CAMERA_BIAS_SPEED = 5.0f;
        if (speed < MIN_SPEED)
        {
            vx = m_cameraDirection.x * CAMERA_BIAS_SPEED;
            vy = m_cameraDirection.y * CAMERA_BIAS_SPEED;
            vz = m_cameraDirection.z * CAMERA_BIAS_SPEED;
        }

        float t = m_config.lookaheadTime;
        return XMFLOAT3{m_playerPosition.x + vx * t, m_playerPosition.y + vy * t, m_playerPosition.z + vz * t};
    }

    float SeamlessAreaManager::DistanceToArea(const XMFLOAT3& point, const AreaDefinition& area) const
    {
        // Signed distance from point to AABB (0 if inside)
        float dx = std::max(0.0f, std::max(area.boundsMin.x - point.x, point.x - area.boundsMax.x));
        float dy = std::max(0.0f, std::max(area.boundsMin.y - point.y, point.y - area.boundsMax.y));
        float dz = std::max(0.0f, std::max(area.boundsMin.z - point.z, point.z - area.boundsMax.z));
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float SeamlessAreaManager::DirectionalEffectiveDistance(float rawDistance, const AreaDefinition& area) const
    {
        if (m_config.directionalBias <= 0.0f || rawDistance <= 0.0f)
            return rawDistance;

        XMFLOAT3 playerPos;
        XMFLOAT3 dir;
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            playerPos = m_playerPosition;
            const float speedSq = m_playerVelocity.x * m_playerVelocity.x + m_playerVelocity.y * m_playerVelocity.y +
                                  m_playerVelocity.z * m_playerVelocity.z;
            if (speedSq > 0.25f)
            {
                const float inv = 1.0f / std::sqrt(speedSq);
                dir = {m_playerVelocity.x * inv, m_playerVelocity.y * inv, m_playerVelocity.z * inv};
            }
            else
            {
                dir = m_cameraDirection;
            }
        }

        // Vector from player to area centre
        const float cx = 0.5f * (area.boundsMin.x + area.boundsMax.x);
        const float cy = 0.5f * (area.boundsMin.y + area.boundsMax.y);
        const float cz = 0.5f * (area.boundsMin.z + area.boundsMax.z);
        const float vx = cx - playerPos.x;
        const float vy = cy - playerPos.y;
        const float vz = cz - playerPos.z;
        const float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (vlen < 1e-4f)
            return rawDistance;

        const float dot = (dir.x * vx + dir.y * vy + dir.z * vz) / vlen;
        if (dot < m_config.directionalDotThreshold)
            return rawDistance;

        // Map dot in [threshold, 1] to bias in [0, directionalBias].
        const float t =
            (dot - m_config.directionalDotThreshold) / std::max(1e-4f, 1.0f - m_config.directionalDotThreshold);
        const float reduction = std::clamp(m_config.directionalBias * t, 0.0f, 0.95f);
        return rawDistance * (1.0f - reduction);
    }

    AreaID SeamlessAreaManager::FindContainingArea(const XMFLOAT3& point) const
    {
        for (const auto& [id, area] : m_areas)
        {
            const auto& min = area.definition.boundsMin;
            const auto& max = area.definition.boundsMax;
            if (point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y && point.z >= min.z &&
                point.z <= max.z)
            {
                return id;
            }
        }
        return INVALID_AREA_ID;
    }

    // ========================================================================
    // Streaming Logic
    // ========================================================================

    void SeamlessAreaManager::UpdateAreaDistances()
    {
        XMFLOAT3 playerPos;
        XMFLOAT3 playerVel;
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            playerPos = m_playerPosition;
            playerVel = m_playerVelocity;
        }

        float speed = std::sqrt(playerVel.x * playerVel.x + playerVel.y * playerVel.y + playerVel.z * playerVel.z);

        for (auto& [id, area] : m_areas)
        {
            area.distanceToPlayer = DistanceToArea(playerPos, area.definition);

            // Estimate time until the player enters this area
            if (area.distanceToPlayer <= 0.0f)
            {
                area.predictedArrivalTime = 0.0f; // Already inside
            }
            else if (speed > 0.1f)
            {
                area.predictedArrivalTime = area.distanceToPlayer / speed;
            }
            else
            {
                area.predictedArrivalTime = std::numeric_limits<float>::max();
            }
        }
    }

    void SeamlessAreaManager::ProcessLoadQueue()
    {
        XMFLOAT3 predicted = PredictFuturePosition();

        // Build a sorted list of areas that should be loaded
        m_loadQueue.clear();
        for (auto& [id, area] : m_areas)
        {
            if (area.state != AreaState::Unloaded)
            {
                continue;
            }

            // Check if within load radius from either current or predicted position.
            // Areas along the movement direction get a reduced effective distance
            // so they preload even if slightly beyond the raw radius.
            float distCurrent = area.distanceToPlayer;
            float distPredicted = DistanceToArea(predicted, area.definition);
            float biased = DirectionalEffectiveDistance(std::min(distCurrent, distPredicted), area.definition);

            if (biased <= m_config.loadRadius)
            {
                m_loadQueue.push_back(id);
            }
        }

        // Sort by priority (descending), then by distance (ascending)
        std::sort(m_loadQueue.begin(), m_loadQueue.end(),
                  [this](AreaID a, AreaID b)
                  {
                      auto itA = m_areas.find(a);
                      auto itB = m_areas.find(b);
                      if (itA == m_areas.end() || itB == m_areas.end())
                          return itA != m_areas.end();
                      const auto& areaA = itA->second;
                      const auto& areaB = itB->second;
                      if (areaA.definition.priority != areaB.definition.priority)
                      {
                          return areaA.definition.priority > areaB.definition.priority;
                      }
                      const float dA = DirectionalEffectiveDistance(areaA.distanceToPlayer, areaA.definition);
                      const float dB = DirectionalEffectiveDistance(areaB.distanceToPlayer, areaB.definition);
                      return dA < dB;
                  });

        // Start loading up to the concurrent limit
        for (AreaID id : m_loadQueue)
        {
            if (m_activeLoadCount >= m_config.maxConcurrentLoads)
            {
                break;
            }

            auto areaIt = m_areas.find(id);
            if (areaIt == m_areas.end())
                continue;
            auto& area = areaIt->second;
            TransitionAreaState(area, AreaState::Loading);
            ++m_activeLoadCount;

            // Submit async load via AreaAssetLoader — callback fires when done
            m_assetLoader.BeginAreaLoad(id,
                                        [this](AreaID loadedId)
                                        {
                                            auto it = m_areas.find(loadedId);
                                            if (it != m_areas.end() && it->second.state == AreaState::Loading)
                                            {
                                                TransitionAreaState(it->second, AreaState::Loaded);
                                                m_loadedAreaIds.push_back(loadedId);
                                            }
                                            if (m_activeLoadCount > 0)
                                                --m_activeLoadCount;
                                        });
        }
    }

    void SeamlessAreaManager::ProcessUnloadQueue()
    {
        // Unload areas that are beyond the unload radius
        for (auto it = m_loadedAreaIds.begin(); it != m_loadedAreaIds.end();)
        {
            auto areaIt = m_areas.find(*it);
            if (areaIt == m_areas.end())
            {
                it = m_loadedAreaIds.erase(it);
                continue;
            }

            auto& area = areaIt->second;
            if (area.distanceToPlayer > m_config.unloadRadius && *it != m_currentAreaId)
            {
                TransitionAreaState(area, AreaState::Unloading);

                // Release loaded assets via AreaAssetLoader
                AreaID unloadId = *it;
                m_assetLoader.BeginAreaUnload(unloadId,
                                              [this, unloadId](AreaID /*id*/)
                                              {
                                                  auto aIt = m_areas.find(unloadId);
                                                  if (aIt != m_areas.end())
                                                  {
                                                      TransitionAreaState(aIt->second, AreaState::Unloaded);
                                                  }
                                              });

                it = m_loadedAreaIds.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void SeamlessAreaManager::TransitionAreaState(ManagedArea& area, AreaState newState)
    {
        area.state = newState;

        for (const auto& callback : m_stateCallbacks)
        {
            SPARK_GUARDED_UPDATE("SeamlessArea:StateCallback", "Streaming",
                                 { callback(area.definition.areaId, newState); });
        }
    }

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
