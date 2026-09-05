/**
 * @file FPSStateRules.h
 * @brief Invariant predicates the FPS module registers with InvalidStateDetector.
 *
 * The FPS module's actors (Player, Enemy) are GameObjects with their own health
 * fields, not ECS entities, so a rule written against HealthComponent can never
 * fire here. These predicates are stated over the values the module actually has
 * and are shared by the "dead player" and "dead enemy" rules.
 */

#pragma once

namespace Spark
{
    namespace FPSStateRules
    {
        /// Squared horizontal speed above which a corpse counts as "still moving" (m/s)^2.
        inline constexpr float kMovingSpeedSquaredThreshold = 1.0f;

        /**
         * @brief A dead actor must not retain horizontal motion.
         * @return true when the state is invalid and should be reported.
         */
        inline bool DeadActorIsMoving(float health, float velocityX, float velocityZ)
        {
            if (health > 0.0f)
                return false;
            const float speedSquared = velocityX * velocityX + velocityZ * velocityZ;
            return speedSquared > kMovingSpeedSquaredThreshold;
        }

        /**
         * @brief A dead actor must have been deactivated by its death handler.
         * @return true when the state is invalid and should be reported.
         */
        inline bool DeadActorStillActive(float health, bool isActive)
        {
            return health <= 0.0f && isActive;
        }
    } // namespace FPSStateRules
} // namespace Spark
