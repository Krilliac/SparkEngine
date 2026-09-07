/** @file abilities.cpp
 * @brief Exercise the production FPS class ability state through the isolated public SDK consumer.
 */
#include "Game/ClassAbilityState.h"

#include <iostream>

int main()
{
    int failures = 0;
    auto check = [&failures](bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failures;
        }
    };

    Spark::AbilityState ability;
    ability.type = SparkEditor::ClassAbility::JETPACK;
    check(ability.isReady && !ability.isActive && ability.GetCooldownProgress() == 1.0f, "Initial readiness");
    check(!ability.Activate(24.0f) && ability.isReady && !ability.isActive, "Insufficient energy must not activate");
    check(ability.Activate(25.0f) && ability.isActive && !ability.isReady && ability.durationRemaining == 5.0f,
          "Exact energy cost must activate");
    check(!ability.Activate(100.0f), "Active ability must not activate twice");
    ability.Update(2.0f);
    check(ability.isActive && ability.durationRemaining == 3.0f && ability.cooldownRemaining == 0.0f,
          "Active duration countdown");
    ability.Update(3.0f);
    // Preserve the class manager's existing full-frame cooldown decrement on expiry.
    check(!ability.isActive && !ability.isReady && ability.durationRemaining == 0.0f &&
              ability.cooldownRemaining == 7.0f,
          "Expiry starts cooldown and applies the same frame delta");
    check(!ability.Activate(100.0f), "Cooldown must block activation");
    ability.Update(2.0f);
    check(ability.GetCooldownProgress() == 0.5f, "Cooldown progress");
    ability.Update(10.0f);
    check(ability.isReady && ability.cooldownRemaining == 0.0f, "Cooldown overshoot clamps to zero");
    check(ability.Activate(100.0f), "Ability can activate again after cooldown");
    ability.Deactivate();
    check(!ability.isActive && ability.durationRemaining == 0.0f && ability.cooldownRemaining == 10.0f,
          "Explicit cancellation starts full cooldown");
    ability.energyCost = 40.0f;
    ability.cooldownMax = 20.0f;
    ability.durationMax = 8.0f;
    ability.Reset();
    check(ability.isReady && !ability.isActive && ability.cooldownRemaining == 0.0f &&
              ability.durationRemaining == 0.0f && ability.type == SparkEditor::ClassAbility::JETPACK &&
              ability.energyCost == 40.0f && ability.cooldownMax == 20.0f && ability.durationMax == 8.0f,
          "Reset clears runtime state and retains configuration");
    ability.cooldownMax = 0.0f;
    check(ability.GetCooldownProgress() == 1.0f, "Zero cooldown progress avoids division by zero");
    ability.cooldownMax = -1.0f;
    check(ability.GetCooldownProgress() == 1.0f, "Negative cooldown progress fallback");
    return failures == 0 ? 0 : 1;
}
