/**
 * @file SeamlessAreaManager.cpp
 * @brief Implementation of seamless world area transitions
 */

#include "SeamlessAreaManager.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Spark::Streaming
{

    // ============================================================================
    // WorldArea
    // ============================================================================

    bool WorldArea::ContainsPoint(const DirectX::XMFLOAT3& point) const
    {
        float halfX = worldSize.x * 0.5f;
        float halfZ = worldSize.z * 0.5f;
        return point.x >= worldPosition.x - halfX && point.x <= worldPosition.x + halfX &&
               point.z >= worldPosition.z - halfZ && point.z <= worldPosition.z + halfZ;
    }

    float WorldArea::GetDistanceToCenter(const DirectX::XMFLOAT3& point) const
    {
        float dx = point.x - worldPosition.x;
        float dy = point.y - worldPosition.y;
        float dz = point.z - worldPosition.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float WorldArea::GetDistanceToEdge(const DirectX::XMFLOAT3& point) const
    {
        float halfX = worldSize.x * 0.5f;
        float halfZ = worldSize.z * 0.5f;
        float dx = std::max(0.0f, std::abs(point.x - worldPosition.x) - halfX);
        float dz = std::max(0.0f, std::abs(point.z - worldPosition.z) - halfZ);
        return std::sqrt(dx * dx + dz * dz);
    }

    // ============================================================================
    // BorderRegion
    // ============================================================================

    bool BorderRegion::ContainsPoint(const DirectX::XMFLOAT3& point) const
    {
        return std::abs(point.x - center.x) <= extents.x && std::abs(point.y - center.y) <= extents.y &&
               std::abs(point.z - center.z) <= extents.z;
    }

    // ============================================================================
    // SeamlessAreaManager
    // ============================================================================

    SeamlessAreaManager::SeamlessAreaManager() = default;
    SeamlessAreaManager::~SeamlessAreaManager() = default;

    bool SeamlessAreaManager::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        if (m_initialized)
        {
            SPARK_LOG_WARN("SeamlessArea", "Already initialized.");
            return true;
        }

        m_areas.clear();
        m_borders.clear();
        m_transitionCallbacks.clear();
        m_currentArea.clear();
        m_areaSceneIDs.clear();
        m_inBorderRegion = false;
        m_initialized = true;

        SPARK_LOG_INFO("SeamlessArea", "Initialized seamless area manager (max concurrent areas: %d).",
                       kMaxConcurrentAreas);
        return true;
    }

    void SeamlessAreaManager::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        if (!m_initialized)
        {
            return;
        }

        // Unload all active areas before clearing
        for (auto& area : m_areas)
        {
            if (area.state == AreaState::Active || area.state == AreaState::Border)
            {
                area.state = AreaState::Unloaded;
                area.loadProgress = 0.0f;
            }
        }

        m_areas.clear();
        m_borders.clear();
        m_transitionCallbacks.clear();
        m_areaSceneIDs.clear();
        m_currentArea.clear();
        m_inBorderRegion = false;
        m_initialized = false;

        SPARK_LOG_INFO("SeamlessArea", "Shut down seamless area manager.");
    }

    // ============================================================================
    // Area Management
    // ============================================================================

    bool SeamlessAreaManager::RegisterArea(const WorldArea& area)
    {
        if (area.name.empty())
        {
            SPARK_LOG_WARN("SeamlessArea", "Cannot register area with empty name.");
            return false;
        }

        // Validate area dimensions
        if (area.worldSize.x <= 0.0f || area.worldSize.y <= 0.0f || area.worldSize.z <= 0.0f)
        {
            SPARK_LOG_ERROR("SeamlessArea", "Cannot register area '%s': invalid dimensions (%.1f x %.1f x %.1f).",
                            area.name.c_str(), area.worldSize.x, area.worldSize.y, area.worldSize.z);
            return false;
        }

        // Validate streaming distances
        if (area.loadDistance <= 0.0f)
        {
            SPARK_LOG_WARN("SeamlessArea", "Area '%s' has non-positive loadDistance (%.1f), using default 2000.",
                           area.name.c_str(), area.loadDistance);
        }

        if (area.unloadDistance <= area.loadDistance && !area.alwaysLoaded)
        {
            SPARK_LOG_WARN("SeamlessArea",
                           "Area '%s' unloadDistance (%.1f) <= loadDistance (%.1f). "
                           "This may cause load/unload thrashing.",
                           area.name.c_str(), area.unloadDistance, area.loadDistance);
        }

        // Check for duplicates
        for (const auto& existing : m_areas)
        {
            if (existing.name == area.name)
            {
                SPARK_LOG_WARN("SeamlessArea", "Area '%s' already registered.", area.name.c_str());
                return false;
            }
        }

        m_areas.push_back(area);
        SPARK_LOG_INFO("SeamlessArea", "Registered area '%s' at (%.0f, %.0f, %.0f), size (%.0f x %.0f x %.0f)%s.",
                       area.name.c_str(), area.worldPosition.x, area.worldPosition.y, area.worldPosition.z,
                       area.worldSize.x, area.worldSize.y, area.worldSize.z,
                       area.alwaysLoaded ? " [always loaded]" : "");
        return true;
    }

    bool SeamlessAreaManager::UnregisterArea(const std::string& areaName)
    {
        auto it = std::find_if(m_areas.begin(), m_areas.end(),
                               [&areaName](const WorldArea& a) { return a.name == areaName; });
        if (it != m_areas.end())
        {
            // Remove associated borders
            m_borders.erase(std::remove_if(m_borders.begin(), m_borders.end(), [&areaName](const BorderRegion& b)
                                           { return b.areaA == areaName || b.areaB == areaName; }),
                            m_borders.end());

            m_areas.erase(it);
            return true;
        }
        return false;
    }

    WorldArea* SeamlessAreaManager::GetArea(const std::string& areaName)
    {
        for (auto& area : m_areas)
        {
            if (area.name == areaName)
                return &area;
        }
        return nullptr;
    }

    const WorldArea* SeamlessAreaManager::GetArea(const std::string& areaName) const
    {
        for (const auto& area : m_areas)
        {
            if (area.name == areaName)
                return &area;
        }
        return nullptr;
    }

    WorldArea* SeamlessAreaManager::GetAreaAtPosition(const DirectX::XMFLOAT3& position)
    {
        for (auto& area : m_areas)
        {
            if (area.ContainsPoint(position))
                return &area;
        }
        return nullptr;
    }

    // ============================================================================
    // Border Management
    // ============================================================================

    bool SeamlessAreaManager::DefineBorder(const std::string& areaA, const std::string& areaB, float overlapWidth)
    {
        if (areaA.empty() || areaB.empty())
        {
            SPARK_LOG_ERROR("SeamlessArea", "Cannot define border: area names must not be empty.");
            return false;
        }

        if (areaA == areaB)
        {
            SPARK_LOG_ERROR("SeamlessArea", "Cannot define border between area '%s' and itself.", areaA.c_str());
            return false;
        }

        if (overlapWidth <= 0.0f)
        {
            SPARK_LOG_ERROR("SeamlessArea", "Cannot define border: overlapWidth must be positive (got %.1f).",
                            overlapWidth);
            return false;
        }

        // Check for duplicate border
        for (const auto& existing : m_borders)
        {
            if ((existing.areaA == areaA && existing.areaB == areaB) ||
                (existing.areaA == areaB && existing.areaB == areaA))
            {
                SPARK_LOG_WARN("SeamlessArea", "Border between '%s' and '%s' already defined.", areaA.c_str(),
                               areaB.c_str());
                return false;
            }
        }

        const WorldArea* a = GetArea(areaA);
        const WorldArea* b = GetArea(areaB);

        if (!a || !b)
        {
            SPARK_LOG_WARN("SeamlessArea", "Cannot define border: area '%s' or '%s' not found.", areaA.c_str(),
                           areaB.c_str());
            return false;
        }

        BorderRegion border;
        border.areaA = areaA;
        border.areaB = areaB;
        border.overlapWidth = overlapWidth;

        // Calculate border center and extents from the two areas' positions
        border.center.x = (a->worldPosition.x + b->worldPosition.x) * 0.5f;
        border.center.y = (a->worldPosition.y + b->worldPosition.y) * 0.5f;
        border.center.z = (a->worldPosition.z + b->worldPosition.z) * 0.5f;

        // Determine border orientation from the relative positions
        float dx = std::abs(b->worldPosition.x - a->worldPosition.x);
        float dz = std::abs(b->worldPosition.z - a->worldPosition.z);

        if (dx > dz)
        {
            // Border runs along Z axis
            border.extents.x = overlapWidth * 0.5f;
            border.extents.y = std::max(a->worldSize.y, b->worldSize.y) * 0.5f;
            border.extents.z = std::min(a->worldSize.z, b->worldSize.z) * 0.5f;
        }
        else
        {
            // Border runs along X axis
            border.extents.x = std::min(a->worldSize.x, b->worldSize.x) * 0.5f;
            border.extents.y = std::max(a->worldSize.y, b->worldSize.y) * 0.5f;
            border.extents.z = overlapWidth * 0.5f;
        }

        m_borders.push_back(border);
        SPARK_LOG_INFO("SeamlessArea", "Defined border between '%s' and '%s' (overlap=%.0f).", areaA.c_str(),
                       areaB.c_str(), overlapWidth);
        return true;
    }

    std::vector<const BorderRegion*> SeamlessAreaManager::GetBordersForArea(const std::string& areaName) const
    {
        std::vector<const BorderRegion*> result;
        for (const auto& border : m_borders)
        {
            if (border.areaA == areaName || border.areaB == areaName)
            {
                result.push_back(&border);
            }
        }
        return result;
    }

    // ============================================================================
    // Update
    // ============================================================================

    void SeamlessAreaManager::Update(const DirectX::XMFLOAT3& referencePos, float /*deltaTime*/)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        if (!m_initialized)
        {
            return;
        }

        // Validate reference position
        if (!std::isfinite(referencePos.x) || !std::isfinite(referencePos.y) || !std::isfinite(referencePos.z))
        {
            SPARK_LOG_WARN("SeamlessArea", "Update called with non-finite reference position. Skipping.");
            return;
        }

        UpdateAreaStreaming(referencePos);
        UpdateBorderRegions(referencePos);
    }

    void SeamlessAreaManager::UpdateAreaStreaming(const DirectX::XMFLOAT3& referencePos)
    {
        for (auto& area : m_areas)
        {
            if (area.alwaysLoaded)
            {
                if (area.state == AreaState::Unloaded)
                {
                    LoadArea(area.name);
                }
                continue;
            }

            float dist = area.GetDistanceToEdge(referencePos);

            switch (area.state)
            {
            case AreaState::Unloaded:
                if (dist <= area.loadDistance)
                {
                    LoadArea(area.name);
                }
                break;

            case AreaState::Active:
            case AreaState::Border:
                if (dist > area.unloadDistance)
                {
                    UnloadArea(area.name);
                }
                break;

            default:
                break;
            }
        }

        // Detect current area
        std::string newArea;
        for (const auto& area : m_areas)
        {
            if (area.state == AreaState::Active && area.ContainsPoint(referencePos))
            {
                newArea = area.name;
                break;
            }
        }

        if (!newArea.empty() && newArea != m_currentArea)
        {
            AreaTransitionEvent event;
            event.fromArea = m_currentArea;
            event.toArea = newArea;
            event.crossingPosition = referencePos;
            event.isSeamless = true;

            SPARK_LOG_INFO("SeamlessArea", "Transitioned from '%s' to '%s'.", m_currentArea.c_str(), newArea.c_str());

            m_currentArea = newArea;
            NotifyTransition(event);
        }
    }

    void SeamlessAreaManager::UpdateBorderRegions(const DirectX::XMFLOAT3& referencePos)
    {
        m_inBorderRegion = false;
        for (const auto& border : m_borders)
        {
            if (border.ContainsPoint(referencePos))
            {
                m_inBorderRegion = true;

                // Ensure both areas are active when player is in the border
                WorldArea* areaA = GetArea(border.areaA);
                WorldArea* areaB = GetArea(border.areaB);

                if (areaA && areaA->state == AreaState::Unloaded)
                {
                    LoadArea(border.areaA);
                }
                if (areaB && areaB->state == AreaState::Unloaded)
                {
                    LoadArea(border.areaB);
                }

                break;
            }
        }
    }

    // ============================================================================
    // Coordinate Translation
    // ============================================================================

    DirectX::XMFLOAT3 SeamlessAreaManager::TranslatePosition(const DirectX::XMFLOAT3& position,
                                                             const std::string& fromArea,
                                                             const std::string& toArea) const
    {
        const WorldArea* from = GetArea(fromArea);
        const WorldArea* to = GetArea(toArea);

        if (!from || !to)
        {
            return position;
        }

        // Convert from source area's local space to absolute, then to target's local space
        // absolute = localPos + fromArea.localOrigin
        // targetLocal = absolute - toArea.localOrigin
        return {position.x + from->localOrigin.x - to->localOrigin.x,
                position.y + from->localOrigin.y - to->localOrigin.y,
                position.z + from->localOrigin.z - to->localOrigin.z};
    }

    // ============================================================================
    // Events
    // ============================================================================

    void SeamlessAreaManager::RegisterTransitionCallback(AreaTransitionCallback callback)
    {
        if (callback)
        {
            m_transitionCallbacks.push_back(std::move(callback));
        }
    }

    // ============================================================================
    // Queries
    // ============================================================================

    int SeamlessAreaManager::GetActiveAreaCount() const
    {
        int count = 0;
        for (const auto& area : m_areas)
        {
            if (area.state == AreaState::Active || area.state == AreaState::Border)
            {
                count++;
            }
        }
        return count;
    }

    std::string SeamlessAreaManager::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "SeamlessAreaManager: " << m_areas.size() << " areas (" << GetActiveAreaCount() << " active)" << " | "
            << m_borders.size() << " borders" << " | Current: " << (m_currentArea.empty() ? "none" : m_currentArea)
            << " | InBorder: " << (m_inBorderRegion ? "yes" : "no");
        return oss.str();
    }

    // ============================================================================
    // Internal
    // ============================================================================

    void SeamlessAreaManager::LoadArea(const std::string& areaName)
    {
        if (areaName.empty())
        {
            SPARK_LOG_WARN("SeamlessArea", "LoadArea called with empty area name.");
            return;
        }

        WorldArea* area = GetArea(areaName);
        if (!area)
        {
            SPARK_LOG_ERROR("SeamlessArea", "LoadArea: area '%s' not registered.", areaName.c_str());
            return;
        }

        if (area->state != AreaState::Unloaded)
        {
            SPARK_LOG_DEBUG("SeamlessArea", "LoadArea: area '%s' is already in state %d, skipping.", areaName.c_str(),
                            static_cast<int>(area->state));
            return;
        }

        // Guard against loading too many areas simultaneously
        int activeCount = GetActiveAreaCount();
        if (activeCount >= kMaxConcurrentAreas)
        {
            SPARK_LOG_WARN("SeamlessArea",
                           "LoadArea: cannot load '%s' — %d areas already active (max %d). "
                           "Consider increasing unloadDistance or reducing area density.",
                           areaName.c_str(), activeCount, kMaxConcurrentAreas);
            return;
        }

        if (area->scenePath.empty())
        {
            SPARK_LOG_WARN("SeamlessArea",
                           "LoadArea: area '%s' has no scene path configured. "
                           "Loading as empty area.",
                           areaName.c_str());
        }

        area->state = AreaState::Loading;
        area->loadProgress = 0.0f;
        SPARK_LOG_INFO("SeamlessArea", "Loading area '%s' (scene: '%s', priority: %d)...", areaName.c_str(),
                       area->scenePath.c_str(), area->priority);

        // Attempt to load via SceneTransitionManager if a scene path is provided.
        // The SceneTransitionManager handles async loading with progress callbacks.
        // If no scene path is set, we treat the area as immediately active (e.g., procedural areas).
        if (!area->scenePath.empty())
        {
            // In a production build with SceneTransitionManager integrated via EngineContext,
            // this would call:
            //   auto* stm = EngineContext::Get()->GetSubsystem<SceneTransitionManager>();
            //   SceneID sid = stm->LoadSceneAdditive(area->scenePath, progressCb, completeCb);
            //   m_areaSceneIDs[areaName] = sid;
            //
            // The completion callback would transition the area to Active or Failed.
            // For now, we simulate a successful synchronous load.
            area->loadProgress = 1.0f;
            area->state = AreaState::Active;
            SPARK_LOG_INFO("SeamlessArea", "Area '%s' loaded successfully (simulated sync load).", areaName.c_str());
        }
        else
        {
            // Procedural or empty area — immediately active
            area->loadProgress = 1.0f;
            area->state = AreaState::Active;
            SPARK_LOG_INFO("SeamlessArea", "Area '%s' activated (no scene file).", areaName.c_str());
        }
    }

    void SeamlessAreaManager::UnloadArea(const std::string& areaName)
    {
        if (areaName.empty())
        {
            SPARK_LOG_WARN("SeamlessArea", "UnloadArea called with empty area name.");
            return;
        }

        WorldArea* area = GetArea(areaName);
        if (!area)
        {
            SPARK_LOG_ERROR("SeamlessArea", "UnloadArea: area '%s' not registered.", areaName.c_str());
            return;
        }

        if (area->state == AreaState::Unloaded)
        {
            return; // Already unloaded, nothing to do
        }

        if (area->alwaysLoaded)
        {
            SPARK_LOG_DEBUG("SeamlessArea", "UnloadArea: area '%s' is marked alwaysLoaded, skipping.",
                            areaName.c_str());
            return;
        }

        if (area->state == AreaState::Loading)
        {
            SPARK_LOG_WARN("SeamlessArea",
                           "UnloadArea: area '%s' is currently loading. "
                           "Canceling load and marking as unloaded.",
                           areaName.c_str());
        }

        area->state = AreaState::Unloading;
        SPARK_LOG_INFO("SeamlessArea", "Unloading area '%s'...", areaName.c_str());

        // Unload the scene via SceneTransitionManager if it was loaded through one
        auto sceneIt = m_areaSceneIDs.find(areaName);
        if (sceneIt != m_areaSceneIDs.end())
        {
            // In a production build:
            //   auto* stm = EngineContext::Get()->GetSubsystem<SceneTransitionManager>();
            //   stm->UnloadScene(sceneIt->second);
            m_areaSceneIDs.erase(sceneIt);
            SPARK_LOG_INFO("SeamlessArea", "Area '%s' scene unloaded from SceneTransitionManager.", areaName.c_str());
        }

        // Clear the current area if we're unloading it
        if (m_currentArea == areaName)
        {
            SPARK_LOG_WARN("SeamlessArea", "Unloading current area '%s'. Player may be in limbo.", areaName.c_str());
            m_currentArea.clear();
        }

        area->state = AreaState::Unloaded;
        area->loadProgress = 0.0f;
        SPARK_LOG_INFO("SeamlessArea", "Area '%s' unloaded.", areaName.c_str());
    }

    void SeamlessAreaManager::NotifyTransition(const AreaTransitionEvent& event)
    {
        for (const auto& callback : m_transitionCallbacks)
        {
            callback(event);
        }
    }

} // namespace Spark::Streaming
