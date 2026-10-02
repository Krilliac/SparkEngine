/**
 * @file TFDamageRules.h
 * @brief Pure pool rules behind TFDamageSystem (DESIGN §4): friendly-fire
 *        scaling, shield-first absorb, and delayed shield regen.
 *
 * Header-only and free of module context so SparkTests asserts the exact rules
 * the server applies, not a copy of them.
 */
#pragma once

#include <algorithm>

namespace Terrafront::DamageRules
{
    inline constexpr float kFriendlyFireMult = 0.5f; // DESIGN §4: friendly fire ON at 50%
    inline constexpr float kShieldRegenPerSec = 80.0f;

    /// Same-faction damage is friendly fire unless the victim hurt itself.
    [[nodiscard]] inline constexpr bool IsFriendlyFire(bool sameFaction, bool selfInflicted)
    {
        return sameFaction && !selfInflicted;
    }

    [[nodiscard]] inline constexpr float ScaleFriendlyFire(float amount, bool friendlyFire)
    {
        return friendlyFire ? amount * kFriendlyFireMult : amount;
    }

    /// Shield absorbs first, the spill goes to health, and health clamps at 0.
    /// A non-positive hit or an already-dead target is a no-op.
    /// @return true when this hit dropped health to 0.
    inline bool ApplyShieldFirst(float& shield, float& health, float amount)
    {
        if (amount <= 0.0f || health <= 0.0f)
            return false;
        const float toShield = std::min(shield, amount);
        shield -= toShield;
        health = std::max(0.0f, health - (amount - toShield));
        return health <= 0.0f;
    }

    /// One fixed tick of shield regen: nothing for a dead or no-regen pawn, a
    /// full shield, or while the regen delay since the last hit is running.
    /// @return true when the shield changed.
    inline bool TickShieldRegen(float& shield, float maxShield, float health, bool noRegen, double clock,
                                double lastDamageAt, float regenDelaySec, float dt)
    {
        if (noRegen || shield >= maxShield || health <= 0.0f)
            return false;
        if (clock - lastDamageAt < regenDelaySec)
            return false;
        shield = std::min(maxShield, shield + kShieldRegenPerSec * dt);
        return true;
    }
} // namespace Terrafront::DamageRules
