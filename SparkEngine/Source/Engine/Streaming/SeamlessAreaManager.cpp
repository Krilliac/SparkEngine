/**
 * @file SeamlessAreaManager.cpp
 * @brief Implementation of seamless world area transitions
 */

#include "SeamlessAreaManager.h"
#include "../../Utils/LogMacros.h"

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
        SPARK_LOG_INFO("SeamlessArea", "Initialized seamless area manager.");
        return true;
    }

    void SeamlessAreaManager::Shutdown()
    {
        m_areas.clear();
        m_borders.clear();
        m_transitionCallbacks.clear();
        m_currentArea.clear();
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
        SPARK_LOG_INFO("SeamlessArea", "Registered area '%s' at (%.0f, %.0f, %.0f).", area.name.c_str(),
                       area.worldPosition.x, area.worldPosition.y, area.worldPosition.z);
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
        WorldArea* area = GetArea(areaName);
        if (!area || area->state != AreaState::Unloaded)
            return;

        area->state = AreaState::Loading;
        SPARK_LOG_INFO("SeamlessArea", "Loading area '%s'...", areaName.c_str());

        // In a full implementation, this would use SceneTransitionManager::LoadSceneAdditive()
        // to asynchronously load the area's scene file. For now, we mark it as active.
        area->state = AreaState::Active;
        area->loadProgress = 1.0f;
    }

    void SeamlessAreaManager::UnloadArea(const std::string& areaName)
    {
        WorldArea* area = GetArea(areaName);
        if (!area || area->state == AreaState::Unloaded || area->alwaysLoaded)
            return;

        area->state = AreaState::Unloading;
        SPARK_LOG_INFO("SeamlessArea", "Unloading area '%s'...", areaName.c_str());

        // In a full implementation, this would use SceneTransitionManager::UnloadScene()
        area->state = AreaState::Unloaded;
        area->loadProgress = 0.0f;
    }

    void SeamlessAreaManager::NotifyTransition(const AreaTransitionEvent& event)
    {
        for (const auto& callback : m_transitionCallbacks)
        {
            callback(event);
        }
    }

} // namespace Spark::Streaming
