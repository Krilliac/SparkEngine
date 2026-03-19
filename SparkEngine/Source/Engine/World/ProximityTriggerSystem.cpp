/**
 * @file ProximityTriggerSystem.cpp
 * @brief Spatial trigger volume system implementation
 */

#include "ProximityTriggerSystem.h"

#include <cmath>

namespace Spark::World
{

    // ============================================================================
    // Initialization / Shutdown
    // ============================================================================

    void ProximityTriggerSystem::Initialize()
    {
        m_triggers.clear();
        m_occupants.clear();
        m_nextID = 1;
    }

    void ProximityTriggerSystem::Shutdown()
    {
        m_triggers.clear();
        m_occupants.clear();
        m_nextID = 1;
    }

    // ============================================================================
    // Trigger Creation / Removal
    // ============================================================================

    uint32_t ProximityTriggerSystem::CreateSphereTrigger(const XMFLOAT3& center, float radius, TriggerCallback onEnter,
                                                         TriggerCallback onExit)
    {
        uint32_t id = m_nextID++;

        TriggerVolume trigger;
        trigger.id = id;
        trigger.shape = TriggerShape::Sphere;
        trigger.center = center;
        trigger.radius = radius;
        trigger.onEnter = std::move(onEnter);
        trigger.onExit = std::move(onExit);

        m_triggers.emplace(id, std::move(trigger));
        m_occupants.emplace(id, std::unordered_set<uint32_t>{});

        return id;
    }

    uint32_t ProximityTriggerSystem::CreateAABBTrigger(const XMFLOAT3& center, const XMFLOAT3& halfExtents,
                                                       TriggerCallback onEnter, TriggerCallback onExit)
    {
        uint32_t id = m_nextID++;

        TriggerVolume trigger;
        trigger.id = id;
        trigger.shape = TriggerShape::AABB;
        trigger.center = center;
        trigger.halfExtents = halfExtents;
        trigger.onEnter = std::move(onEnter);
        trigger.onExit = std::move(onExit);

        m_triggers.emplace(id, std::move(trigger));
        m_occupants.emplace(id, std::unordered_set<uint32_t>{});

        return id;
    }

    void ProximityTriggerSystem::RemoveTrigger(uint32_t triggerID)
    {
        m_triggers.erase(triggerID);
        m_occupants.erase(triggerID);
    }

    void ProximityTriggerSystem::EnableTrigger(uint32_t triggerID, bool enabled)
    {
        auto it = m_triggers.find(triggerID);
        if (it != m_triggers.end())
        {
            it->second.enabled = enabled;

            // When disabling, fire exit events for all current occupants
            if (!enabled)
            {
                auto occIt = m_occupants.find(triggerID);
                if (occIt != m_occupants.end())
                {
                    for (uint32_t entityID : occIt->second)
                    {
                        if (it->second.onExit)
                        {
                            it->second.onExit(triggerID, entityID);
                        }
                    }
                    occIt->second.clear();
                }
            }
        }
    }

    // ============================================================================
    // Per-frame Update
    // ============================================================================

    void ProximityTriggerSystem::Update(const std::vector<EntityPosition>& entityPositions)
    {
        for (auto& [triggerID, trigger] : m_triggers)
        {
            if (!trigger.enabled)
            {
                continue;
            }

            auto& currentOccupants = m_occupants[triggerID];

            // Build a set of entities currently inside this trigger
            std::unordered_set<uint32_t> nowInside;
            for (const auto& ep : entityPositions)
            {
                if (IsInsideTrigger(trigger, ep.position))
                {
                    nowInside.insert(ep.entityID);
                }
            }

            // Detect entries: entities in nowInside but not in currentOccupants
            for (uint32_t entityID : nowInside)
            {
                if (currentOccupants.find(entityID) == currentOccupants.end())
                {
                    if (trigger.onEnter)
                    {
                        trigger.onEnter(triggerID, entityID);
                    }
                }
            }

            // Detect exits: entities in currentOccupants but not in nowInside
            for (uint32_t entityID : currentOccupants)
            {
                if (nowInside.find(entityID) == nowInside.end())
                {
                    if (trigger.onExit)
                    {
                        trigger.onExit(triggerID, entityID);
                    }
                }
            }

            // Update the occupant set
            currentOccupants = std::move(nowInside);
        }
    }

    uint32_t ProximityTriggerSystem::GetActiveTriggerCount() const
    {
        uint32_t count = 0;
        for (const auto& [id, trigger] : m_triggers)
        {
            if (trigger.enabled)
            {
                ++count;
            }
        }
        return count;
    }

    // ============================================================================
    // Overlap Testing
    // ============================================================================

    bool ProximityTriggerSystem::IsInsideTrigger(const TriggerVolume& trigger, const XMFLOAT3& position)
    {
        switch (trigger.shape)
        {
        case TriggerShape::Sphere:
        {
            float dx = position.x - trigger.center.x;
            float dy = position.y - trigger.center.y;
            float dz = position.z - trigger.center.z;
            float distSq = dx * dx + dy * dy + dz * dz;
            return distSq <= (trigger.radius * trigger.radius);
        }

        case TriggerShape::AABB:
        {
            float dx = std::abs(position.x - trigger.center.x);
            float dy = std::abs(position.y - trigger.center.y);
            float dz = std::abs(position.z - trigger.center.z);
            return dx <= trigger.halfExtents.x && dy <= trigger.halfExtents.y && dz <= trigger.halfExtents.z;
        }

        default:
            return false;
        }
    }

} // namespace Spark::World
