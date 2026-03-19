/**
 * @file FormationSystem.cpp
 * @brief Implementation of the CryEngine-inspired formation management system
 */

#include "FormationSystem.h"

#include <cmath>
#include <algorithm>

namespace Spark::AI
{

    // =========================================================================
    // Constants
    // =========================================================================

    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr float kTwoPi = 2.0f * kPi;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void FormationSystem::Initialize()
    {
        m_formations.clear();
        m_nextID = 1;
        m_initialized = true;
    }

    void FormationSystem::Shutdown()
    {
        m_formations.clear();
        m_initialized = false;
    }

    // =========================================================================
    // Formation Creation / Destruction
    // =========================================================================

    uint32_t FormationSystem::CreateFormation(FormationType type, uint32_t leaderEntity, uint32_t memberCount,
                                              float spacing)
    {
        if (!m_initialized || memberCount == 0)
        {
            return 0;
        }

        uint32_t id = m_nextID++;

        FormationInstance instance;
        instance.id = id;
        instance.type = type;
        instance.leaderEntity = leaderEntity;
        instance.center = {0.0f, 0.0f, 0.0f};
        instance.heading = 0.0f;
        instance.slots = GenerateSlots(type, memberCount, spacing);

        m_formations[id] = std::move(instance);
        return id;
    }

    void FormationSystem::DisbandFormation(uint32_t formationID)
    {
        m_formations.erase(formationID);
    }

    // =========================================================================
    // Slot Assignment
    // =========================================================================

    int FormationSystem::AssignToSlot(uint32_t formationID, uint32_t entityID)
    {
        auto it = m_formations.find(formationID);
        if (it == m_formations.end())
        {
            return -1;
        }

        auto& slots = it->second.slots;
        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (!slots[i].occupied)
            {
                slots[i].assignedEntity = entityID;
                slots[i].occupied = true;
                return static_cast<int>(i);
            }
        }

        return -1; // No free slot
    }

    void FormationSystem::RemoveFromFormation(uint32_t formationID, uint32_t entityID)
    {
        auto it = m_formations.find(formationID);
        if (it == m_formations.end())
        {
            return;
        }

        for (auto& slot : it->second.slots)
        {
            if (slot.occupied && slot.assignedEntity == entityID)
            {
                slot.assignedEntity = 0;
                slot.occupied = false;
                return;
            }
        }
    }

    // =========================================================================
    // Query
    // =========================================================================

    XMFLOAT3 FormationSystem::GetSlotWorldPosition(uint32_t formationID, int slotIndex) const
    {
        auto it = m_formations.find(formationID);
        if (it == m_formations.end())
        {
            return {0.0f, 0.0f, 0.0f};
        }

        const auto& formation = it->second;
        if (slotIndex < 0 || slotIndex >= static_cast<int>(formation.slots.size()))
        {
            return {0.0f, 0.0f, 0.0f};
        }

        // Rotate the local offset by the formation heading (Y-axis rotation)
        const auto& local = formation.slots[slotIndex].localOffset;
        float cosH = std::cos(formation.heading);
        float sinH = std::sin(formation.heading);

        XMFLOAT3 world;
        world.x = formation.center.x + local.x * cosH - local.z * sinH;
        world.y = formation.center.y + local.y;
        world.z = formation.center.z + local.x * sinH + local.z * cosH;

        return world;
    }

    const FormationInstance* FormationSystem::GetFormation(uint32_t formationID) const
    {
        auto it = m_formations.find(formationID);
        if (it == m_formations.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    // =========================================================================
    // Update
    // =========================================================================

    void FormationSystem::Update([[maybe_unused]] float deltaTime)
    {
        // In a full integration, this would query the ECS world for each
        // leader's Transform to update center and heading. For now, the
        // caller is expected to set formation center/heading externally
        // or this will be wired in when integrated with the ECS World.
    }

    // =========================================================================
    // Slot Generation
    // =========================================================================

    std::vector<FormationSlot> FormationSystem::GenerateSlots(FormationType type, uint32_t count, float spacing) const
    {
        std::vector<FormationSlot> slots(count);

        switch (type)
        {
        case FormationType::Line:
        {
            // Slots spread left-to-right, centered on the formation origin
            float halfWidth = (static_cast<float>(count) - 1.0f) * spacing * 0.5f;
            for (uint32_t i = 0; i < count; ++i)
            {
                slots[i].localOffset.x = static_cast<float>(i) * spacing - halfWidth;
                slots[i].localOffset.y = 0.0f;
                slots[i].localOffset.z = 0.0f;
            }
            break;
        }

        case FormationType::Wedge:
        {
            // V-shape: alternating left and right, each row further back
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t row = (i / 2) + 1;
                float side = (i % 2 == 0) ? -1.0f : 1.0f;
                slots[i].localOffset.x = side * static_cast<float>(row) * spacing * 0.5f;
                slots[i].localOffset.y = 0.0f;
                slots[i].localOffset.z = -static_cast<float>(row) * spacing;
            }
            break;
        }

        case FormationType::Column:
        {
            // Single file behind the leader
            for (uint32_t i = 0; i < count; ++i)
            {
                slots[i].localOffset.x = 0.0f;
                slots[i].localOffset.y = 0.0f;
                slots[i].localOffset.z = -static_cast<float>(i + 1) * spacing;
            }
            break;
        }

        case FormationType::Circle:
        {
            // Evenly distributed around a ring
            float radius = spacing * static_cast<float>(count) / kTwoPi;
            if (radius < spacing)
            {
                radius = spacing;
            }
            for (uint32_t i = 0; i < count; ++i)
            {
                float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(count);
                slots[i].localOffset.x = radius * std::cos(angle);
                slots[i].localOffset.y = 0.0f;
                slots[i].localOffset.z = radius * std::sin(angle);
            }
            break;
        }

        case FormationType::Spread:
        {
            // Grid-like spread with slight staggering
            uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(count))));
            float halfWidth = (static_cast<float>(cols) - 1.0f) * spacing * 0.5f;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t col = i % cols;
                uint32_t row = i / cols;
                float stagger = (row % 2 == 1) ? spacing * 0.5f : 0.0f;
                slots[i].localOffset.x = static_cast<float>(col) * spacing - halfWidth + stagger;
                slots[i].localOffset.y = 0.0f;
                slots[i].localOffset.z = -static_cast<float>(row) * spacing;
            }
            break;
        }
        }

        return slots;
    }

} // namespace Spark::AI
