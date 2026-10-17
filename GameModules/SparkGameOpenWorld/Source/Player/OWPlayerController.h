/**
 * @file OWPlayerController.h
 * @brief Open world player input: walking, sprinting, turning, and world interaction
 * @author Spark Engine Team
 * @date 2026
 *
 * Turns keyboard input (or injected test input) into movement across the authored
 * biome regions and into interactions with the gathering, dynamic-event, and
 * settlement systems. The controller owns only input state; position, stamina,
 * and region live in OWPlayerSystem.
 */

#pragma once

#include "Spark/IEngineContext.h"

#include <cstdint>

namespace OpenWorld
{
    class OWPlayerSystem;
    class OWWorldSetup;
    class OWGatheringSystem;
    class OWDynamicEventSystem;
    class OWSettlementSystem;

    /// @brief What a single interaction did
    struct InteractResult
    {
        enum class Kind : uint8_t
        {
            None,
            Harvest,
            JoinEvent,
            VisitSettlement
        };

        Kind kind = Kind::None;
        uint32_t id = 0;     ///< Node, event, or settlement id
        uint32_t amount = 0; ///< Resources harvested (Harvest only)
    };

    /**
     * @brief Player movement and interaction controller for the open world
     *
     * Keys (sampled in Update when the host provides input): W/S forward and back,
     * A/D strafe, Q/E turn, Shift sprint, F interact. Without host input the values
     * set through SetMoveInput/SetTurnInput persist, which is how automated runs drive it.
     */
    class OWPlayerController
    {
      public:
        static constexpr float kWalkSpeed = 6.0f;             ///< m/s
        static constexpr float kSprintSpeed = 11.0f;          ///< m/s
        static constexpr float kKeyboardTurnRate = 120.0f;    ///< deg/s while Q or E is held
        static constexpr float kSprintRecoverStamina = 25.0f; ///< stamina needed to sprint again after exhaustion
        static constexpr float kHarvestRange = 4.0f;          ///< m
        static constexpr float kEventJoinRange = 25.0f;       ///< m

        void Initialize(Spark::IEngineContext* context, OWPlayerSystem& player, const OWWorldSetup& world,
                        OWGatheringSystem& gathering, OWDynamicEventSystem& events, OWSettlementSystem& settlements);

        /// @brief forward/strafe in [-1, 1] (+strafe is to the right); sprint requests the sprint speed
        void SetMoveInput(float forward, float strafe, bool sprint);
        /// @brief Turn rate in degrees per second (+ is clockwise, toward east from north)
        void SetTurnInput(float yawDegreesPerSecond);

        /// @brief Sample host input; an F press runs TryInteract
        void Update(float deltaTime);
        /// @brief Turn and move the player one fixed step, staying inside the authored regions
        void FixedUpdate(float fixedDeltaTime);

        /// @brief Interact with the world at the player's position. Priority: a resource node
        ///        within kHarvestRange, then an unjoined event within kEventJoinRange, then the
        ///        settlement the player stands in.
        InteractResult TryInteract();

      private:
        OWPlayerSystem* m_player{nullptr};
        const OWWorldSetup* m_world{nullptr};
        OWGatheringSystem* m_gathering{nullptr};
        OWDynamicEventSystem* m_events{nullptr};
        OWSettlementSystem* m_settlements{nullptr};
        Spark::IEngineContext* m_context{nullptr};

        float m_forward{0.0f};
        float m_strafe{0.0f};
        float m_turnRate{0.0f};
        bool m_sprintRequested{false};
        bool m_sprintExhausted{false};
        bool m_interactHeld{false};
    };

} // namespace OpenWorld
