/**
 * @file SeamlessAreaManager.cpp
 * @brief Predictive asset streaming implementation
 *
 * Uses player velocity and camera direction to predict future position,
 * then pre-loads areas along the predicted path. Areas beyond the unload
 * radius are released to free memory.
 */

#include "SeamlessAreaManager.h"
#include "../../Utils/SparkConsole.h"

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
        if (m_initialized)
        {
            return;
        }

        m_areas.clear();
        m_loadedAreaIds.clear();
        m_loadQueue.clear();
        m_currentAreaId = INVALID_AREA_ID;
        m_activeLoadCount = 0;
        m_timeSinceLastUpdate = 0.0f;
        m_initialized = true;

        Spark::SimpleConsole::GetInstance().LogInfo("[SeamlessAreaManager] Initialized");
    }

    void SeamlessAreaManager::Update(float dt)
    {
        if (!m_initialized)
        {
            return;
        }

        m_timeSinceLastUpdate += dt;
        if (m_timeSinceLastUpdate < m_config.updateInterval)
        {
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
    }

    void SeamlessAreaManager::Shutdown()
    {
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
        m_initialized = false;

        Spark::SimpleConsole::GetInstance().LogInfo("[SeamlessAreaManager] Shutdown");
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
    }

    void SeamlessAreaManager::UnregisterArea(AreaID areaId)
    {
        auto it = m_areas.find(areaId);
        if (it == m_areas.end())
        {
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

            // Check if within load radius from either current or predicted position
            float distCurrent = area.distanceToPlayer;
            float distPredicted = DistanceToArea(predicted, area.definition);
            float effectiveDist = std::min(distCurrent, distPredicted);

            if (effectiveDist <= m_config.loadRadius)
            {
                m_loadQueue.push_back(id);
            }
        }

        // Sort by priority (descending), then by distance (ascending)
        std::sort(m_loadQueue.begin(), m_loadQueue.end(),
                  [this](AreaID a, AreaID b)
                  {
                      const auto& areaA = m_areas.at(a);
                      const auto& areaB = m_areas.at(b);
                      if (areaA.definition.priority != areaB.definition.priority)
                      {
                          return areaA.definition.priority > areaB.definition.priority;
                      }
                      return areaA.distanceToPlayer < areaB.distanceToPlayer;
                  });

        // Start loading up to the concurrent limit
        for (AreaID id : m_loadQueue)
        {
            if (m_activeLoadCount >= m_config.maxConcurrentLoads)
            {
                break;
            }

            auto& area = m_areas.at(id);
            TransitionAreaState(area, AreaState::Loading);
            ++m_activeLoadCount;

            // Simulate immediate load completion for now (actual async loading
            // would integrate with the asset pipeline here)
            TransitionAreaState(area, AreaState::Loaded);
            m_loadedAreaIds.push_back(id);
            --m_activeLoadCount;
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
                TransitionAreaState(area, AreaState::Unloaded);
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
            callback(area.definition.areaId, newState);
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
