/**
 * @file ClassAbilityState.cpp
 * @brief Production FPS class ability activation and cooldown transitions.
 */

#include "ClassAbilityState.h"

namespace Spark
{
    // ============================================================================
    // AbilityState Implementation
    // ============================================================================

    void AbilityState::Update(float dt)
    {
        if (isActive)
        {
            durationRemaining -= dt;
            if (durationRemaining <= 0.0f)
            {
                Deactivate();
            }
        }

        if (cooldownRemaining > 0.0f)
        {
            cooldownRemaining -= dt;
            if (cooldownRemaining <= 0.0f)
            {
                cooldownRemaining = 0.0f;
                isReady = true;
            }
        }
    }

    bool AbilityState::Activate(float currentEnergy)
    {
        if (!isReady || isActive || currentEnergy < energyCost)
            return false;

        isActive = true;
        isReady = false;
        durationRemaining = durationMax;
        return true;
    }

    void AbilityState::Deactivate()
    {
        isActive = false;
        durationRemaining = 0.0f;
        cooldownRemaining = cooldownMax;
    }

    void AbilityState::Reset()
    {
        isActive = false;
        isReady = true;
        cooldownRemaining = 0.0f;
        durationRemaining = 0.0f;
    }

    float AbilityState::GetCooldownProgress() const
    {
        if (cooldownMax <= 0.0f)
            return 1.0f;
        return 1.0f - (cooldownRemaining / cooldownMax);
    }

} // namespace Spark
