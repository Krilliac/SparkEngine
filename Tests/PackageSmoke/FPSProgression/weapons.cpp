/** @file weapons.cpp
 * @brief Exercise the FPS weapon configuration used by Player and GameDebugUI through the public SDK.
 */
#include <Spark/WeaponTypes.h>
#include "Enums/GameSystemEnums.h"
#include "Projectiles/WeaponStats.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>
#include <type_traits>

using SparkEditor::WeaponType;

namespace
{
    struct WeaponName
    {
        WeaponType type;
        int value;
        const char* name;
    };

    constexpr std::array names{
        WeaponName{WeaponType::PISTOL, 0, "Pistol"},
        WeaponName{WeaponType::RIFLE, 1, "Rifle"},
        WeaponName{WeaponType::SHOTGUN, 2, "Shotgun"},
        WeaponName{WeaponType::ROCKET_LAUNCHER, 3, "Rocket Launcher"},
        WeaponName{WeaponType::GRENADE_LAUNCHER, 4, "Grenade Launcher"},
        WeaponName{WeaponType::SNIPER_RIFLE, 5, "Sniper Rifle"},
        WeaponName{WeaponType::SUBMACHINE_GUN, 6, "Submachine Gun"},
        WeaponName{WeaponType::ASSAULT_RIFLE, 7, "Assault Rifle"},
        WeaponName{WeaponType::MACHINE_GUN, 8, "Machine Gun"},
        WeaponName{WeaponType::FLAMETHROWER, 9, "Flamethrower"},
        WeaponName{WeaponType::PLASMA_RIFLE, 10, "Plasma Rifle"},
        WeaponName{WeaponType::LASER_CANNON, 11, "Laser Cannon"},
        WeaponName{WeaponType::RAILGUN, 12, "Railgun"},
        WeaponName{WeaponType::MINIGUN, 13, "Minigun"},
        WeaponName{WeaponType::CROSSBOW, 14, "Crossbow"},
        WeaponName{WeaponType::BOW, 15, "Bow"},
        WeaponName{WeaponType::THROWING_KNIFE, 16, "Throwing Knife"},
        WeaponName{WeaponType::MELEE_WEAPON, 17, "Melee Weapon"},
        WeaponName{WeaponType::GRAPPLING_HOOK, 50, "Grappling Hook"},
        WeaponName{WeaponType::SCANNER, 51, "Scanner"},
        WeaponName{WeaponType::REPAIR_TOOL, 52, "Repair Tool"},
        WeaponName{WeaponType::MEDICAL_TOOL, 53, "Medical Tool"},
        WeaponName{WeaponType::CUSTOM_1, 100, "Custom Weapon 1"},
        WeaponName{WeaponType::CUSTOM_2, 101, "Custom Weapon 2"},
        WeaponName{WeaponType::CUSTOM_3, 102, "Custom Weapon 3"},
    };

    static_assert(std::is_same_v<std::underlying_type_t<WeaponType>, int>);
    static_assert(static_cast<int>(WeaponType::COUNT) == 103);
    static_assert(
        []
        {
            for (const auto& entry : names)
                if (static_cast<int>(entry.type) != entry.value)
                    return false;
            return true;
        }());

    // Expected balance data must not use the production constructor: otherwise
    // a constructor regression could corrupt both the actual and expected rows.
    struct ExpectedStats
    {
        WeaponType Type;
        float Damage;
        float FireRate;
        int MagazineSize;
        float ReloadTime;
        float MuzzleVelocity;
        float Accuracy;
    };

    bool SameStats(const WeaponStats& actual, const ExpectedStats& expected)
    {
        return actual.Type == expected.Type && actual.Damage == expected.Damage &&
               actual.FireRate == expected.FireRate && actual.MagazineSize == expected.MagazineSize &&
               actual.ReloadTime == expected.ReloadTime && actual.MuzzleVelocity == expected.MuzzleVelocity &&
               actual.Accuracy == expected.Accuracy;
    }
} // namespace

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

    // Preserve the shipped balance table while relocating its dependencies.
    const std::array defaults{
        ExpectedStats{WeaponType::PISTOL, 25, 450, 15, 2, 350, 0.85f},
        ExpectedStats{WeaponType::RIFLE, 35, 600, 30, 2.5f, 800, 0.75f},
        ExpectedStats{WeaponType::SHOTGUN, 80, 120, 8, 3, 400, 0.45f},
        ExpectedStats{WeaponType::ROCKET_LAUNCHER, 200, 60, 4, 4, 300, 0.95f},
        ExpectedStats{WeaponType::GRENADE_LAUNCHER, 150, 90, 6, 3.5f, 250, 0.70f},
        ExpectedStats{WeaponType::SNIPER_RIFLE, 120, 60, 5, 3.5f, 1200, 0.98f},
        ExpectedStats{WeaponType::SUBMACHINE_GUN, 18, 900, 40, 2.2f, 300, 0.60f},
        ExpectedStats{WeaponType::ASSAULT_RIFLE, 30, 700, 30, 2.8f, 750, 0.70f},
        ExpectedStats{WeaponType::MACHINE_GUN, 40, 800, 100, 5, 850, 0.65f},
        ExpectedStats{WeaponType::FLAMETHROWER, 15, 1200, 200, 4, 50, 0.80f},
        ExpectedStats{WeaponType::PLASMA_RIFLE, 45, 300, 20, 3, 600, 0.88f},
        ExpectedStats{WeaponType::LASER_CANNON, 60, 180, 12, 4, 0, 0.95f},
        ExpectedStats{WeaponType::RAILGUN, 180, 30, 3, 5, 2000, 0.99f},
        ExpectedStats{WeaponType::MINIGUN, 25, 3000, 500, 8, 600, 0.50f},
        ExpectedStats{WeaponType::CROSSBOW, 85, 45, 1, 3, 400, 0.90f},
        ExpectedStats{WeaponType::BOW, 50, 120, 1, 1.5f, 300, 0.85f},
        ExpectedStats{WeaponType::THROWING_KNIFE, 40, 180, 6, 2, 250, 0.75f},
        ExpectedStats{WeaponType::MELEE_WEAPON, 60, 120, 0, 0, 0, 0.95f},
    };
    for (const auto& expected : defaults)
        check(SameStats(GetDefaultWeaponStats(expected.Type), expected), WeaponTypeToString(expected.Type));
    for (const auto& entry : names)
    {
        check(std::string_view{WeaponTypeToString(entry.type)} == entry.name, "Weapon display name changed");
        if (entry.value >= 50)
            check(SameStats(GetDefaultWeaponStats(entry.type), defaults[0]), "Utility/custom pistol fallback changed");
    }
    for (const auto type : {static_cast<WeaponType>(-1), static_cast<WeaponType>(49), WeaponType::COUNT})
    {
        check(SameStats(GetDefaultWeaponStats(type), defaults[0]), "Unknown weapon fallback changed");
        check(std::string_view{WeaponTypeToString(type)} == "Unknown", "Unknown weapon name changed");
    }

    check(SameStats(WeaponStats{}, ExpectedStats{WeaponType::PISTOL, 10, 600, 15, 2, 300, 0.85f}),
          "Default constructor differs from its original safe values");
    const auto custom = CreateWeaponStats(WeaponType::RAILGUN, 12, 120, 7, 4, 800, 0.5f);
    check(SameStats(custom, ExpectedStats{WeaponType::RAILGUN, 12, 120, 7, 4, 800, 0.5f}) && custom.IsValid(),
          "Explicit weapon configuration changed");
    check(custom.GetShotInterval() == 0.5f && custom.GetDPS() == 24 && custom.GetEffectiveRange() == 40,
          "Derived shot interval, DPS, or range changed");
    const auto zero = CreateWeaponStats(WeaponType::PISTOL, 0, 0, 0, 0, 0, 0);
    check(zero.IsValid() && zero.GetShotInterval() == 1 && zero.GetDPS() == 0 && zero.GetEffectiveRange() == 0,
          "Zero-valued boundary or shot interval fallback changed");
    for (const auto field : {&WeaponStats::Damage, &WeaponStats::FireRate, &WeaponStats::ReloadTime,
                             &WeaponStats::MuzzleVelocity, &WeaponStats::Accuracy})
    {
        auto invalid = custom;
        invalid.*field = -1;
        check(!invalid.IsValid(), "Negative weapon statistic was accepted");
        invalid.*field = std::numeric_limits<float>::quiet_NaN();
        check(!invalid.IsValid(), "NaN weapon statistic was accepted");
    }
    auto invalid = custom;
    invalid.MagazineSize = -1;
    check(!invalid.IsValid(), "Negative magazine capacity was accepted");
    invalid = custom;
    invalid.Accuracy = 1;
    check(invalid.IsValid(), "Perfect accuracy was rejected");
    invalid.Accuracy = 1.01f;
    check(!invalid.IsValid(), "Accuracy above one was accepted");
    invalid = custom;
    invalid.FireRate = -1;
    check(invalid.GetShotInterval() == 1, "Negative fire rate safety fallback changed");
    return failures == 0 ? 0 : 1;
}
