/**
 * @file ProximityTriggerSystem.h
 * @brief CryEngine-inspired spatial trigger volume system
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides axis-aligned bounding box (AABB) and sphere trigger volumes that
 * fire enter/exit callbacks when entities move in or out. Designed for gameplay
 * events like area transitions, ambient audio zones, and scripted encounters.
 *
 * @threadsafety Main thread only.
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Spark::World
{

    // ============================================================================
    // Trigger Types
    // ============================================================================

    /**
     * @brief Callback signature invoked when an entity enters or exits a trigger.
     * @param triggerID ID of the trigger volume.
     * @param entityID  ID of the entity that entered or exited.
     */
    using TriggerCallback = std::function<void(uint32_t triggerID, uint32_t entityID)>;

    /**
     * @brief Shape of a trigger volume.
     */
    enum class TriggerShape : uint8_t
    {
        Sphere, ///< Spherical trigger defined by center + radius.
        AABB    ///< Axis-aligned box defined by center + half-extents.
    };

    // ============================================================================
    // TriggerVolume — a single trigger in the world
    // ============================================================================

    /**
     * @brief A spatial volume that fires callbacks on entity enter/exit.
     */
    struct TriggerVolume
    {
        uint32_t id = 0;
        TriggerShape shape = TriggerShape::Sphere;
        XMFLOAT3 center{0.0f, 0.0f, 0.0f};
        XMFLOAT3 halfExtents{1.0f, 1.0f, 1.0f}; ///< For AABB shape.
        float radius = 1.0f;                    ///< For Sphere shape.
        bool enabled = true;
        TriggerCallback onEnter; ///< Called when an entity enters.
        TriggerCallback onExit;  ///< Called when an entity exits.
    };

    // ============================================================================
    // Entity position for trigger testing
    // ============================================================================

    /**
     * @brief Lightweight entity position used for trigger overlap tests.
     */
    struct EntityPosition
    {
        uint32_t entityID = 0;
        XMFLOAT3 position{0.0f, 0.0f, 0.0f};
    };

    // ============================================================================
    // ProximityTriggerSystem — singleton trigger management
    // ============================================================================

    /**
     * @brief Manages trigger volumes and fires enter/exit events each frame.
     *
     * Each frame, call Update() with the current entity positions. The system
     * tracks which entities are inside each trigger and fires onEnter/onExit
     * callbacks on state transitions.
     */
    class ProximityTriggerSystem
    {
      public:
        static ProximityTriggerSystem& GetInstance()
        {
            static ProximityTriggerSystem s;
            return s;
        }

        /** @brief Initialize the system, clearing all triggers and state. */
        void Initialize();

        /** @brief Shut down and release all trigger data. */
        void Shutdown();

        // -- Trigger management --

        /**
         * @brief Create a sphere trigger volume.
         * @param center   World-space center of the sphere.
         * @param radius   Sphere radius.
         * @param onEnter  Callback fired when an entity enters.
         * @param onExit   Callback fired when an entity exits.
         * @return The new trigger's unique ID.
         */
        uint32_t CreateSphereTrigger(const XMFLOAT3& center, float radius, TriggerCallback onEnter,
                                     TriggerCallback onExit);

        /**
         * @brief Create an AABB trigger volume.
         * @param center      World-space center of the box.
         * @param halfExtents Half-size along each axis.
         * @param onEnter     Callback fired when an entity enters.
         * @param onExit      Callback fired when an entity exits.
         * @return The new trigger's unique ID.
         */
        uint32_t CreateAABBTrigger(const XMFLOAT3& center, const XMFLOAT3& halfExtents, TriggerCallback onEnter,
                                   TriggerCallback onExit);

        /**
         * @brief Remove a trigger volume by ID.
         * @param triggerID The trigger to remove.
         */
        void RemoveTrigger(uint32_t triggerID);

        /**
         * @brief Enable or disable a trigger without removing it.
         * @param triggerID The trigger to modify.
         * @param enabled   New enabled state.
         */
        void EnableTrigger(uint32_t triggerID, bool enabled);

        // -- Per-frame update --

        /**
         * @brief Test all entities against all triggers and fire enter/exit events.
         * @param entityPositions Current positions of all trackable entities.
         */
        void Update(const std::vector<EntityPosition>& entityPositions);

        /** @brief Get the number of active (enabled) triggers. */
        uint32_t GetActiveTriggerCount() const;

        /** @brief Get the total number of triggers (enabled + disabled). */
        uint32_t GetTotalTriggerCount() const { return static_cast<uint32_t>(m_triggers.size()); }

      private:
        ProximityTriggerSystem() = default;
        ~ProximityTriggerSystem() = default;

        ProximityTriggerSystem(const ProximityTriggerSystem&) = delete;
        ProximityTriggerSystem& operator=(const ProximityTriggerSystem&) = delete;

        /** @brief Test whether a point is inside a trigger volume. */
        static bool IsInsideTrigger(const TriggerVolume& trigger, const XMFLOAT3& position);

        /// All registered triggers, keyed by ID.
        std::unordered_map<uint32_t, TriggerVolume> m_triggers;

        /// Per-trigger set of entity IDs currently inside.
        std::unordered_map<uint32_t, std::unordered_set<uint32_t>> m_occupants;

        /// Next auto-assigned trigger ID.
        uint32_t m_nextID = 1;
    };

} // namespace Spark::World
