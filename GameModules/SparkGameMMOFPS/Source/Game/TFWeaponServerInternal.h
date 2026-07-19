/**
 * @file TFWeaponServerInternal.h
 * @brief Shared internals for the TFWeaponServer*.cpp split parts: the
 *        TF_DamageEvent damageKind convention shared by the validated-fire
 *        entry (hitscan pellets) and the server projectile step. Include only
 *        from the TFWeaponServer translation units.
 */
#pragma once

#include <cstdint>

namespace Terrafront
{
    namespace WeaponServerDetail
    {

        inline constexpr uint8_t kDamageKindBullet = 0;
        inline constexpr uint8_t kDamageKindExplosive = 1;

    } // namespace WeaponServerDetail
} // namespace Terrafront
