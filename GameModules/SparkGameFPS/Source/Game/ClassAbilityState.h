/**
 * @file ClassAbilityState.h
 * @brief FPS class ability runtime state without private engine dependencies.
 */

#pragma once

#include "Enums/GameSystemEnums.h"

namespace Spark
{
    /**
 * @brief Ability runtime state
 */
    struct AbilityState
    {
        SparkEditor::ClassAbility type = SparkEditor::ClassAbility::NONE;
        float cooldownMax = 10.0f;      ///< Max cooldown in seconds
        float cooldownRemaining = 0.0f; ///< Current cooldown remaining
        float durationMax = 5.0f;       ///< Max active duration
        float durationRemaining = 0.0f; ///< Current active time remaining
        float energyCost = 25.0f;       ///< Energy/resource cost to activate
        bool isActive = false;          ///< Currently active
        bool isReady = true;            ///< Off cooldown and ready to use

        /** @brief Advance active duration and cooldown by the frame delta in seconds. */
        void Update(float dt);
        /** @brief Activate when ready and the supplied energy meets the configured cost. */
        bool Activate(float currentEnergy);
        /** @brief End the active duration and begin a full cooldown. */
        void Deactivate();
        /** @brief Clear runtime state while preserving the ability configuration. */
        void Reset();
        /** @brief Return cooldown completion; nonpositive cooldown maxima return one. */
        float GetCooldownProgress() const;
    };

} // namespace Spark
