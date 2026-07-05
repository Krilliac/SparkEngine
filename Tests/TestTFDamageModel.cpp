/**
 * @file TestTFDamageModel.cpp
 * @brief TERRAFRONT damage-model math, reimplemented standalone per DESIGN §4
 *        and verified against the module's authoritative rules
 *        (GameModules/SparkGameMMOFPS/Source/Game/TFDamageSystem.cpp).
 *
 * Module sources are NOT compiled into SparkTests, so this file re-derives the
 * pure math and asserts the design-contract numbers:
 *   - shield absorbs first, spill goes to health, health clamps at 0
 *   - friendly fire: same faction, not self => 50% damage
 *   - shield regen: 80/s after the faction regen delay (default 6s)
 *   - TTK examples: Cyclone-9 (112 dmg) vs 500+500 pool = 9 body shots,
 *     5 headshots (x2), TTK inside the ~0.6-1.0s design band
 *   - faction trait multipliers (damageMult/rofMult) shift shots-to-kill
 *
 * If these tests disagree with TFDamageSystem.cpp, the MODULE (or DESIGN.md)
 * changed — do not silently re-tune the constants here.
 */

#include "TestFramework.h"

#include <algorithm>
#include <cmath>

namespace
{
    // --- constants from DESIGN §4 / TFDamageSystem.cpp -----------------------
    constexpr float kFriendlyFireMult  = 0.5f;
    constexpr float kShieldRegenPerSec = 80.0f;
    constexpr float kRegenDelaySec     = 6.0f;

    struct Pools
    {
        float health = 500.0f, maxHealth = 500.0f;
        float shield = 500.0f, maxShield = 500.0f;
        float lastDamageAt = -1.0e9f;
        bool  noRegen = false;
    };

    /// Mirror of TFDamageSystem::ServerApplyDamage's pool math.
    /// Returns true if the hit killed the target.
    bool ApplyDamage(Pools& p, float amount, float now,
                     bool sameFaction = false, bool selfInflicted = false)
    {
        if (amount <= 0.0f || p.health <= 0.0f)
            return false;
        if (sameFaction && !selfInflicted)
            amount *= kFriendlyFireMult;

        const float toShield = std::min(p.shield, amount);
        p.shield -= toShield;
        const float remaining = amount - toShield;
        p.health = std::max(0.0f, p.health - remaining);
        p.lastDamageAt = now;
        return p.health <= 0.0f;
    }

    /// Mirror of TFDamageSystem::FixedUpdate's regen step (one fixed tick).
    void RegenTick(Pools& p, float now, float dt,
                   float delaySec = kRegenDelaySec, float rate = kShieldRegenPerSec)
    {
        if (p.noRegen || p.shield >= p.maxShield || p.health <= 0.0f)
            return;
        if (now - p.lastDamageAt < delaySec)
            return;
        p.shield = std::min(p.maxShield, p.shield + rate * dt);
    }

    /// Body shots to drop a (health+shield) pool with a flat per-shot damage.
    int ShotsToKill(float totalPool, float perShot)
    {
        return static_cast<int>(std::ceil(totalPool / perShot));
    }

    /// Seconds from first to last shot at a given rounds/minute.
    float TimeToKill(int shots, float rofRpm)
    {
        return (shots - 1) * (60.0f / rofRpm);
    }

    /// Simulate a full engagement; returns shots actually needed.
    int SimulateKill(Pools p, float perShot, bool sameFaction = false)
    {
        int shots = 0;
        while (p.health > 0.0f && shots < 1000)
        {
            ++shots;
            if (ApplyDamage(p, perShot, static_cast<float>(shots), sameFaction))
                return shots;
        }
        return shots;
    }

    // Weapon numbers from Assets/MMOFPS/Data/weapons.json + factions.json.
    constexpr float kCyclone9Damage  = 112.0f;  // MRA rifle, base body damage
    constexpr float kCyclone9RofRpm  = 750.0f;
    constexpr float kHeadshotMult    = 2.0f;
    constexpr float kMraDamageMult   = 0.92f;
    constexpr float kMraRofMult      = 1.10f;
    constexpr float kAucDamageMult   = 1.15f;
    constexpr float kMagnateDamage   = 143.0f;  // AUC rifle, base body damage
    constexpr float kDefaultPool     = 1000.0f; // 500 HP + 500 shield
} // namespace

// ============================================================================
// Shield-first absorb
// ============================================================================

TEST(TFDamage_ShieldAbsorbsFirst)
{
    Pools p;
    ApplyDamage(p, 300.0f, 0.0f);
    EXPECT_NEAR(p.shield, 200.0f, 0.001f);
    EXPECT_NEAR(p.health, 500.0f, 0.001f); // health untouched while shield holds
}

TEST(TFDamage_OverflowSpillsIntoHealth)
{
    Pools p;
    ApplyDamage(p, 650.0f, 0.0f); // 500 shield + 150 health
    EXPECT_NEAR(p.shield, 0.0f, 0.001f);
    EXPECT_NEAR(p.health, 350.0f, 0.001f);
}

TEST(TFDamage_ExactShieldBreakLeavesHealthIntact)
{
    Pools p;
    ApplyDamage(p, 500.0f, 0.0f);
    EXPECT_NEAR(p.shield, 0.0f, 0.001f);
    EXPECT_NEAR(p.health, 500.0f, 0.001f);
}

TEST(TFDamage_HealthClampsAtZero_NeverNegative)
{
    Pools p;
    const bool killed = ApplyDamage(p, 99999.0f, 0.0f);
    EXPECT_TRUE(killed);
    EXPECT_NEAR(p.health, 0.0f, 0.0f);
    EXPECT_GE(p.shield, 0.0f);
}

TEST(TFDamage_ExactLethalKills)
{
    Pools p;
    p.shield = 0.0f;
    p.health = 112.0f;
    EXPECT_TRUE(ApplyDamage(p, 112.0f, 0.0f));
}

TEST(TFDamage_DeadTargetTakesNoFurtherDamage)
{
    Pools p;
    ApplyDamage(p, 99999.0f, 0.0f);
    Pools after = p;
    EXPECT_FALSE(ApplyDamage(after, 100.0f, 1.0f)); // no double-kill
    EXPECT_NEAR(after.shield, p.shield, 0.0f);
}

// ============================================================================
// Friendly fire (DESIGN §4: ON at 50%)
// ============================================================================

TEST(TFDamage_FriendlyFireHalved)
{
    Pools p;
    ApplyDamage(p, 300.0f, 0.0f, /*sameFaction=*/true);
    EXPECT_NEAR(p.shield, 350.0f, 0.001f); // only 150 absorbed
}

TEST(TFDamage_SelfDamageIsNotReduced)
{
    // TFDamageSystem: friendly requires attackerPawn != victim — rocket-jumping
    // yourself hurts at full price.
    Pools p;
    ApplyDamage(p, 300.0f, 0.0f, /*sameFaction=*/true, /*selfInflicted=*/true);
    EXPECT_NEAR(p.shield, 200.0f, 0.001f);
}

TEST(TFDamage_FriendlyFireDoublesShotsToKill)
{
    const int hostile  = SimulateKill(Pools{}, kCyclone9Damage, false);
    const int friendly = SimulateKill(Pools{}, kCyclone9Damage, true);
    EXPECT_EQ(hostile, 9);
    EXPECT_EQ(friendly, 18); // exactly double: 1000/56 -> ceil = 18
}

// ============================================================================
// TTK contract examples (DESIGN §4: TTK ~0.6-1.0s)
// ============================================================================

TEST(TFDamage_Cyclone9_NineBodyShots_AtBaseDamage)
{
    // ceil(1000 / 112) == 9; after 8 shots 104 pool remains.
    EXPECT_EQ(ShotsToKill(kDefaultPool, kCyclone9Damage), 9);
    EXPECT_EQ(SimulateKill(Pools{}, kCyclone9Damage), 9);

    Pools p;
    for (int i = 0; i < 8; ++i)
        EXPECT_FALSE(ApplyDamage(p, kCyclone9Damage, static_cast<float>(i)));
    EXPECT_NEAR(p.health + p.shield, kDefaultPool - 8.0f * kCyclone9Damage, 0.01f);
    EXPECT_TRUE(ApplyDamage(p, kCyclone9Damage, 9.0f));
}

TEST(TFDamage_Cyclone9_FiveHeadshots)
{
    EXPECT_EQ(ShotsToKill(kDefaultPool, kCyclone9Damage * kHeadshotMult), 5);
    EXPECT_EQ(SimulateKill(Pools{}, kCyclone9Damage * kHeadshotMult), 5);
}

TEST(TFDamage_Cyclone9_TTKInsideDesignBand)
{
    // Base stats: 9 shots @ 750 rpm -> 8 * 0.080s = 0.64s.
    const float ttkBase = TimeToKill(9, kCyclone9RofRpm);
    EXPECT_NEAR(ttkBase, 0.64f, 0.005f);

    // With MRA faction traits (dmg x0.92 -> 10 shots, rof x1.10 -> 825 rpm):
    // 9 * 60/825 = 0.6545s. Both land in the design TTK band.
    const int shotsTrait = ShotsToKill(kDefaultPool, kCyclone9Damage * kMraDamageMult);
    EXPECT_EQ(shotsTrait, 10);
    const float ttkTrait = TimeToKill(shotsTrait, kCyclone9RofRpm * kMraRofMult);

    EXPECT_GE(ttkBase, 0.5f);
    EXPECT_LE(ttkBase, 1.1f);
    EXPECT_GE(ttkTrait, 0.5f);
    EXPECT_LE(ttkTrait, 1.1f);
}

TEST(TFDamage_FactionTraits_ShiftShotsToKill)
{
    // MRA: high RoF, lower per-shot damage -> more shots.
    EXPECT_EQ(ShotsToKill(kDefaultPool, kCyclone9Damage * kMraDamageMult), 10);
    // AUC Magnate AR with +15% damage: 143 * 1.15 = 164.45 -> 7 shots.
    EXPECT_EQ(ShotsToKill(kDefaultPool, kMagnateDamage * kAucDamageMult), 7);
    // HLX is the 1.0 baseline by design.
    EXPECT_EQ(ShotsToKill(kDefaultPool, 125.0f * 1.0f), 8); // Helical Lance
}

// ============================================================================
// Shield regen (80/s after 6s without damage)
// ============================================================================

TEST(TFDamage_ShieldRegen_WaitsForDelay)
{
    Pools p;
    ApplyDamage(p, 300.0f, /*now=*/0.0f);
    EXPECT_NEAR(p.shield, 200.0f, 0.001f);

    // Tick up to 5.95s — still inside the 6s delay window: no regen.
    float now = 0.0f;
    const float dt = 1.0f / 60.0f;
    while (now + dt < 6.0f)
    {
        now += dt;
        RegenTick(p, now, dt);
    }
    EXPECT_NEAR(p.shield, 200.0f, 0.001f);

    // One second past the delay: ~80 shield back.
    while (now < 7.0f)
    {
        now += dt;
        RegenTick(p, now, dt);
    }
    EXPECT_NEAR(p.shield, 280.0f, 5.0f);
}

TEST(TFDamage_ShieldRegen_DamageResetsDelay)
{
    Pools p;
    ApplyDamage(p, 300.0f, 0.0f);
    // Take another hit at t=5 — the 6s window restarts from there.
    ApplyDamage(p, 50.0f, 5.0f);
    RegenTick(p, 10.9f, 1.0f / 60.0f);
    EXPECT_NEAR(p.shield, 150.0f, 0.001f); // 5+6=11s, not yet
    RegenTick(p, 11.5f, 1.0f / 60.0f);
    EXPECT_GT(p.shield, 150.0f);
}

TEST(TFDamage_ShieldRegen_ClampsAtMax)
{
    Pools p;
    p.shield = 499.0f;
    p.lastDamageAt = 0.0f;
    RegenTick(p, 100.0f, 1.0f); // one fat tick would overshoot by 79
    EXPECT_NEAR(p.shield, 500.0f, 0.001f);
}

TEST(TFDamage_ShieldRegen_SkipsDeadAndNoRegen)
{
    Pools dead;
    dead.health = 0.0f;
    dead.shield = 100.0f;
    dead.lastDamageAt = 0.0f;
    RegenTick(dead, 100.0f, 1.0f);
    EXPECT_NEAR(dead.shield, 100.0f, 0.001f);

    // Colossus: noRegen (classes.json) — shield never comes back.
    Pools col;
    col.noRegen = true;
    col.shield = 0.0f;
    col.lastDamageAt = 0.0f;
    RegenTick(col, 100.0f, 1.0f);
    EXPECT_NEAR(col.shield, 0.0f, 0.001f);
}

// ============================================================================
// Class pool variants (classes.json)
// ============================================================================

TEST(TFDamage_ClassPools_ChangeTTK)
{
    // Ghost 400+400: ceil(800/112) = 8 body shots.
    Pools ghost;
    ghost.health = ghost.maxHealth = 400.0f;
    ghost.shield = ghost.maxShield = 400.0f;
    EXPECT_EQ(SimulateKill(ghost, kCyclone9Damage), 8);

    // Bulwark 550+500: ceil(1050/112) = 10.
    Pools bulwark;
    bulwark.health = bulwark.maxHealth = 550.0f;
    bulwark.shield = bulwark.maxShield = 500.0f;
    EXPECT_EQ(SimulateKill(bulwark, kCyclone9Damage), 10);

    // Colossus 2200+0: ceil(2200/112) = 20.
    Pools colossus;
    colossus.health = colossus.maxHealth = 2200.0f;
    colossus.shield = colossus.maxShield = 0.0f;
    colossus.noRegen = true;
    EXPECT_EQ(SimulateKill(colossus, kCyclone9Damage), 20);
}
