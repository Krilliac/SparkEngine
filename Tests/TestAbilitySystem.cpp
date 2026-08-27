// TestAbilitySystem.cpp - Tests for ability casting, auras, procs, and cooldowns

#include "TestFramework.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone reimplementation of ability system types for testing
// (mirrors Spark::Gameplay from AbilitySystem.h)
// ============================================================================

namespace
{

    using AbilityID = uint32_t;
    using AuraID = uint32_t;
    using EntityID = uint32_t;

    enum class AbilityTargetType : uint8_t
    {
        Self,
        SingleTarget,
        AreaOfEffect,
        Cone,
        Projectile
    };
    enum class AuraType : uint8_t
    {
        Buff,
        Debuff,
        DamageOverTime,
        HealOverTime,
        Shield,
        ModifySpeed,
        Stun
    };
    enum class AuraStackType : uint8_t
    {
        None,
        Stacking,
        Separate
    };
    enum class CastPhase : uint8_t
    {
        Preparing,
        Casting,
        Launching,
        Channeling,
        Completed,
        Failed,
        Interrupted
    };

    struct AbilityDefinition
    {
        AbilityID id = 0;
        std::string name;
        AbilityTargetType targetType = AbilityTargetType::Self;
        float castTime = 0.0f;
        float cooldown = 0.0f;
        float resourceCost = 0.0f;
    };

    struct AuraDefinition
    {
        AuraID id = 0;
        std::string name;
        AuraType type = AuraType::Buff;
        AuraStackType stackType = AuraStackType::None;
        int maxStacks = 1;
        float duration = 0.0f;
        float tickInterval = 0.0f;
    };

    struct ActiveAura
    {
        AuraID definitionID = 0;
        EntityID target = 0;
        int stacks = 1;
        float remainingDuration = 0.0f;
        float tickTimer = 0.0f;
        int totalTicks = 0;
    };

    struct CastState
    {
        AbilityID abilityID = 0;
        CastPhase phase = CastPhase::Preparing;
        float elapsed = 0.0f;
        float totalCastTime = 0.0f;
    };

    struct ProcTrigger
    {
        AbilityID triggerAbilityID = 0;
        AbilityID procAbilityID = 0;
        float chance = 1.0f;
    };

    struct AbilitySystem
    {
        std::unordered_map<AbilityID, AbilityDefinition> abilities;
        std::unordered_map<AuraID, AuraDefinition> auraDefs;
        std::vector<ActiveAura> activeAuras;
        std::vector<ProcTrigger> procTriggers;
        std::unordered_map<AbilityID, float> cooldowns;
        CastState currentCast{};
        bool isCasting = false;
        float currentResource = 100.0f;

        void RegisterAbility(const AbilityDefinition& def) { abilities[def.id] = def; }
        void RegisterAura(const AuraDefinition& def) { auraDefs[def.id] = def; }
        void RegisterProc(const ProcTrigger& proc) { procTriggers.push_back(proc); }

        const AbilityDefinition* GetAbility(AbilityID id) const
        {
            auto it = abilities.find(id);
            return it != abilities.end() ? &it->second : nullptr;
        }

        const AuraDefinition* GetAuraDef(AuraID id) const
        {
            auto it = auraDefs.find(id);
            return it != auraDefs.end() ? &it->second : nullptr;
        }

        bool StartCast(AbilityID id)
        {
            auto* def = GetAbility(id);
            if (!def)
                return false;
            auto cdIt = cooldowns.find(id);
            if (cdIt != cooldowns.end() && cdIt->second > 0.0f)
                return false;
            if (currentResource < def->resourceCost)
                return false;

            currentResource -= def->resourceCost;
            if (def->castTime <= 0.0f)
            {
                currentCast = {id, CastPhase::Completed, 0.0f, 0.0f};
                isCasting = false;
                cooldowns[id] = def->cooldown;
            }
            else
            {
                currentCast = {id, CastPhase::Casting, 0.0f, def->castTime};
                isCasting = true;
            }
            return true;
        }

        void InterruptCast()
        {
            if (isCasting)
            {
                currentCast.phase = CastPhase::Interrupted;
                isCasting = false;
            }
        }

        void ApplyAura(AuraID auraID, EntityID target)
        {
            auto* def = GetAuraDef(auraID);
            if (!def)
                return;
            if (def->stackType == AuraStackType::Stacking)
            {
                for (auto& aura : activeAuras)
                {
                    if (aura.definitionID == auraID && aura.target == target)
                    {
                        if (aura.stacks < def->maxStacks)
                            aura.stacks++;
                        aura.remainingDuration = def->duration;
                        return;
                    }
                }
            }
            activeAuras.push_back({auraID, target, 1, def->duration, 0.0f, 0});
        }

        bool RemoveAura(AuraID auraID, EntityID target)
        {
            auto it = std::remove_if(activeAuras.begin(), activeAuras.end(), [&](const ActiveAura& a)
                                     { return a.definitionID == auraID && a.target == target; });
            if (it == activeAuras.end())
                return false;
            activeAuras.erase(it, activeAuras.end());
            return true;
        }

        void RemoveAllAuras(EntityID target)
        {
            auto it = std::remove_if(activeAuras.begin(), activeAuras.end(),
                                     [&](const ActiveAura& a) { return a.target == target; });
            activeAuras.erase(it, activeAuras.end());
        }

        bool HasAura(AuraID auraID, EntityID target) const
        {
            for (const auto& a : activeAuras)
                if (a.definitionID == auraID && a.target == target)
                    return true;
            return false;
        }

        int GetAuraStacks(AuraID auraID, EntityID target) const
        {
            for (const auto& a : activeAuras)
                if (a.definitionID == auraID && a.target == target)
                    return a.stacks;
            return 0;
        }

        void Update(float dt)
        {
            if (isCasting)
            {
                currentCast.elapsed += dt;
                if (currentCast.elapsed >= currentCast.totalCastTime)
                {
                    currentCast.phase = CastPhase::Completed;
                    isCasting = false;
                    if (auto* def = GetAbility(currentCast.abilityID))
                        cooldowns[currentCast.abilityID] = def->cooldown;
                }
            }
            for (auto& aura : activeAuras)
            {
                aura.remainingDuration -= dt;
                if (auto* def = GetAuraDef(aura.definitionID); def && def->tickInterval > 0.0f)
                {
                    aura.tickTimer += dt;
                    while (aura.tickTimer >= def->tickInterval)
                    {
                        aura.tickTimer -= def->tickInterval;
                        aura.totalTicks++;
                    }
                }
            }
            auto it = std::remove_if(activeAuras.begin(), activeAuras.end(),
                                     [](const ActiveAura& a) { return a.remainingDuration <= 0.0f; });
            activeAuras.erase(it, activeAuras.end());
            for (auto& [id, cd] : cooldowns)
            {
                cd -= dt;
                if (cd < 0.0f)
                    cd = 0.0f;
            }
        }
    };

} // namespace

// ============================================================================
// Ability Registration and Lookup
// ============================================================================

TEST(Ability_RegisterAndRetrieve)
{
    AbilitySystem sys;
    sys.RegisterAbility({1, "Fireball", AbilityTargetType::SingleTarget, 1.5f, 0.0f, 0.0f});

    const auto* result = sys.GetAbility(1);
    ASSERT_TRUE(result != nullptr);
    EXPECT_EQ(result->name, std::string("Fireball"));
    EXPECT_EQ(static_cast<int>(result->targetType), static_cast<int>(AbilityTargetType::SingleTarget));
    EXPECT_NEAR(result->castTime, 1.5f, 0.001f);
    EXPECT_TRUE(sys.GetAbility(999) == nullptr);
}

TEST(Aura_RegisterAndRetrieve)
{
    AbilitySystem sys;
    sys.RegisterAura({10, "Burning", AuraType::DamageOverTime, AuraStackType::None, 1, 8.0f, 2.0f});

    const auto* result = sys.GetAuraDef(10);
    ASSERT_TRUE(result != nullptr);
    EXPECT_EQ(result->name, std::string("Burning"));
    EXPECT_EQ(static_cast<int>(result->type), static_cast<int>(AuraType::DamageOverTime));
    EXPECT_NEAR(result->duration, 8.0f, 0.001f);
    EXPECT_TRUE(sys.GetAuraDef(999) == nullptr);
}

// ============================================================================
// Casting
// ============================================================================

TEST(Ability_InstantCast)
{
    AbilitySystem sys;
    sys.RegisterAbility({1, "MeleeStrike", AbilityTargetType::Self, 0.0f, 0.0f, 0.0f});

    EXPECT_TRUE(sys.StartCast(1));
    EXPECT_FALSE(sys.isCasting);
    EXPECT_EQ(static_cast<int>(sys.currentCast.phase), static_cast<int>(CastPhase::Completed));
}

TEST(Ability_CastTimeProgression)
{
    AbilitySystem sys;
    sys.RegisterAbility({2, "Fireball", AbilityTargetType::SingleTarget, 2.0f, 0.0f, 0.0f});

    sys.StartCast(2);
    EXPECT_TRUE(sys.isCasting);
    EXPECT_EQ(static_cast<int>(sys.currentCast.phase), static_cast<int>(CastPhase::Casting));

    sys.Update(1.0f);
    EXPECT_TRUE(sys.isCasting);
    EXPECT_NEAR(sys.currentCast.elapsed, 1.0f, 0.001f);

    sys.Update(1.0f);
    EXPECT_FALSE(sys.isCasting);
    EXPECT_EQ(static_cast<int>(sys.currentCast.phase), static_cast<int>(CastPhase::Completed));
}

TEST(Ability_InterruptCasting)
{
    AbilitySystem sys;
    sys.RegisterAbility({3, "Heal", AbilityTargetType::Self, 3.0f, 0.0f, 0.0f});

    sys.StartCast(3);
    EXPECT_TRUE(sys.isCasting);
    sys.Update(1.0f);
    sys.InterruptCast();
    EXPECT_FALSE(sys.isCasting);
    EXPECT_EQ(static_cast<int>(sys.currentCast.phase), static_cast<int>(CastPhase::Interrupted));
}

// ============================================================================
// Aura Application, Removal, and Stacking
// ============================================================================

TEST(Aura_ApplyAndVerifyActive)
{
    AbilitySystem sys;
    sys.RegisterAura({10, "Shield", AuraType::Shield, AuraStackType::None, 1, 10.0f, 0.0f});

    sys.ApplyAura(10, 42);
    EXPECT_TRUE(sys.HasAura(10, 42));
    EXPECT_FALSE(sys.HasAura(10, 99));
}

TEST(Aura_Remove)
{
    AbilitySystem sys;
    sys.RegisterAura({20, "Slow", AuraType::ModifySpeed, AuraStackType::None, 1, 5.0f, 0.0f});

    sys.ApplyAura(20, 1);
    EXPECT_TRUE(sys.HasAura(20, 1));
    EXPECT_TRUE(sys.RemoveAura(20, 1));
    EXPECT_FALSE(sys.HasAura(20, 1));
    EXPECT_FALSE(sys.RemoveAura(20, 1));
}

TEST(Aura_StackingMaxStacks)
{
    AbilitySystem sys;
    sys.RegisterAura({30, "Poison", AuraType::DamageOverTime, AuraStackType::Stacking, 3, 6.0f, 0.0f});

    sys.ApplyAura(30, 5);
    EXPECT_EQ(sys.GetAuraStacks(30, 5), 1);
    sys.ApplyAura(30, 5);
    EXPECT_EQ(sys.GetAuraStacks(30, 5), 2);
    sys.ApplyAura(30, 5);
    EXPECT_EQ(sys.GetAuraStacks(30, 5), 3);
    sys.ApplyAura(30, 5); // Should not exceed max
    EXPECT_EQ(sys.GetAuraStacks(30, 5), 3);
}

TEST(Aura_DurationExpiryOnUpdate)
{
    AbilitySystem sys;
    sys.RegisterAura({40, "Haste", AuraType::Buff, AuraStackType::None, 1, 3.0f, 0.0f});

    sys.ApplyAura(40, 7);
    EXPECT_TRUE(sys.HasAura(40, 7));
    sys.Update(2.0f);
    EXPECT_TRUE(sys.HasAura(40, 7));
    sys.Update(1.5f);
    EXPECT_FALSE(sys.HasAura(40, 7));
}

TEST(Aura_TickDamageOverTime)
{
    AbilitySystem sys;
    sys.RegisterAura({50, "Ignite", AuraType::DamageOverTime, AuraStackType::None, 1, 10.0f, 2.0f});

    sys.ApplyAura(50, 3);
    sys.Update(5.0f); // 5 seconds = 2 complete ticks at 2s interval
    EXPECT_TRUE(sys.HasAura(50, 3));

    int ticks = 0;
    for (const auto& aura : sys.activeAuras)
        if (aura.definitionID == 50 && aura.target == 3)
            ticks = aura.totalTicks;
    EXPECT_EQ(ticks, 2);
}

TEST(Aura_MultipleOnSameTarget)
{
    AbilitySystem sys;
    sys.RegisterAura({60, "Strength", AuraType::Buff, AuraStackType::None, 1, 30.0f, 0.0f});
    sys.RegisterAura({61, "Weakness", AuraType::Debuff, AuraStackType::None, 1, 15.0f, 0.0f});

    sys.ApplyAura(60, 10);
    sys.ApplyAura(61, 10);
    EXPECT_TRUE(sys.HasAura(60, 10));
    EXPECT_TRUE(sys.HasAura(61, 10));
    EXPECT_EQ(static_cast<int>(sys.activeAuras.size()), 2);
}

TEST(Aura_RemoveAllClearsEverything)
{
    AbilitySystem sys;
    sys.RegisterAura({70, "Regen", AuraType::HealOverTime, AuraStackType::None, 1, 20.0f, 0.0f});
    sys.RegisterAura({71, "Stun", AuraType::Stun, AuraStackType::None, 1, 3.0f, 0.0f});

    sys.ApplyAura(70, 15);
    sys.ApplyAura(71, 15);
    EXPECT_EQ(static_cast<int>(sys.activeAuras.size()), 2);

    sys.RemoveAllAuras(15);
    EXPECT_EQ(static_cast<int>(sys.activeAuras.size()), 0);
    EXPECT_FALSE(sys.HasAura(70, 15));
    EXPECT_FALSE(sys.HasAura(71, 15));
}

// ============================================================================
// Proc System, Cooldowns, and Resource Cost
// ============================================================================

TEST(Proc_RegisterAndChance)
{
    AbilitySystem sys;
    sys.RegisterProc({1, 100, 0.25f});

    EXPECT_EQ(static_cast<int>(sys.procTriggers.size()), 1);
    EXPECT_EQ(sys.procTriggers[0].triggerAbilityID, (uint32_t)1);
    EXPECT_EQ(sys.procTriggers[0].procAbilityID, (uint32_t)100);
    EXPECT_NEAR(sys.procTriggers[0].chance, 0.25f, 0.001f);
}

TEST(Ability_CooldownTracking)
{
    AbilitySystem sys;
    sys.RegisterAbility({5, "Frostbolt", AbilityTargetType::Self, 0.0f, 4.0f, 0.0f});

    EXPECT_TRUE(sys.StartCast(5));
    EXPECT_FALSE(sys.StartCast(5)); // On cooldown
    sys.Update(2.0f);
    EXPECT_FALSE(sys.StartCast(5)); // Still on cooldown
    sys.Update(2.5f);
    EXPECT_TRUE(sys.StartCast(5)); // Cooldown expired
}

TEST(Ability_ResourceCostCheck)
{
    AbilitySystem sys;
    sys.currentResource = 30.0f;
    sys.RegisterAbility({8, "BigSpell", AbilityTargetType::Self, 0.0f, 0.0f, 50.0f});

    EXPECT_FALSE(sys.StartCast(8)); // Not enough resource
    EXPECT_NEAR(sys.currentResource, 30.0f, 0.001f);

    sys.currentResource = 75.0f;
    EXPECT_TRUE(sys.StartCast(8));
    EXPECT_NEAR(sys.currentResource, 25.0f, 0.001f);
}

// ============================================================================
// Effect Type Tests (mirror production AbilitySystem::ApplyEffect)
// ============================================================================

namespace
{
    enum class EffectType : uint8_t
    {
        Damage,
        Heal,
        ApplyAura,
        RemoveAura,
        Summon,
        Teleport,
        ModifyAttribute,
        ApplyForce,
        SpawnProjectile,
        Custom
    };

    struct AbilityEffect
    {
        EffectType type = EffectType::Damage;
        float baseValue = 0.0f;
        float scaling = 1.0f;
        uint32_t customParam = 0;
    };

    struct HealthData
    {
        float health = 100.0f;
        float maxHealth = 100.0f;
        bool isDead = false;
    };

    struct Position
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float rotY = 0.0f; // yaw in degrees
    };

    struct Velocity
    {
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    };

    void ApplyEffectTest(const AbilityEffect& effect, HealthData* targetHp, Position* targetPos, Position* casterPos,
                         Velocity* targetVel, int& summonCount, int& projectileCount)
    {
        switch (effect.type)
        {
        case EffectType::Damage:
        {
            float amount = effect.baseValue * effect.scaling;
            if (targetHp && amount > 0.0f)
            {
                targetHp->health = (std::max)(targetHp->health - amount, 0.0f);
                targetHp->isDead = (targetHp->health <= 0.0f);
            }
            break;
        }
        case EffectType::Heal:
        {
            float amount = effect.baseValue * effect.scaling;
            if (targetHp && amount > 0.0f && !targetHp->isDead)
            {
                targetHp->health = (std::min)(targetHp->health + amount, targetHp->maxHealth);
            }
            break;
        }
        case EffectType::Summon:
        {
            if (casterPos)
            {
                summonCount++;
            }
            break;
        }
        case EffectType::Teleport:
        {
            if (targetPos && casterPos && effect.baseValue > 0.0f)
            {
                float yawRad = targetPos->rotY * 3.14159f / 180.0f;
                targetPos->x += std::sin(yawRad) * effect.baseValue;
                targetPos->z += std::cos(yawRad) * effect.baseValue;
            }
            break;
        }
        case EffectType::ModifyAttribute:
        {
            if (targetHp)
            {
                targetHp->maxHealth = targetHp->maxHealth * effect.scaling + effect.baseValue;
                if (targetHp->maxHealth < 1.0f)
                    targetHp->maxHealth = 1.0f;
                if (targetHp->health > targetHp->maxHealth)
                    targetHp->health = targetHp->maxHealth;
            }
            break;
        }
        case EffectType::ApplyForce:
        {
            if (targetVel && targetPos && casterPos)
            {
                float magnitude = effect.baseValue * effect.scaling;
                if (effect.customParam == 1)
                {
                    targetVel->vy += magnitude;
                }
                else
                {
                    float dx = targetPos->x - casterPos->x;
                    float dz = targetPos->z - casterPos->z;
                    float dist = std::sqrt(dx * dx + dz * dz);
                    if (dist > 0.001f)
                    {
                        targetVel->vx += (dx / dist) * magnitude;
                        targetVel->vz += (dz / dist) * magnitude;
                    }
                }
            }
            break;
        }
        case EffectType::SpawnProjectile:
        {
            if (casterPos)
            {
                projectileCount++;
            }
            break;
        }
        default:
            break;
        }
    }
} // namespace

TEST(Effect_DamageReducesHealth)
{
    HealthData hp{100.0f, 100.0f, false};
    AbilityEffect eff{EffectType::Damage, 30.0f, 1.0f, 0};
    int s = 0, p = 0;
    ApplyEffectTest(eff, &hp, nullptr, nullptr, nullptr, s, p);
    EXPECT_NEAR(hp.health, 70.0f, 0.001f);
    EXPECT_FALSE(hp.isDead);
}

TEST(Effect_DamageKills)
{
    HealthData hp{20.0f, 100.0f, false};
    AbilityEffect eff{EffectType::Damage, 25.0f, 1.0f, 0};
    int s = 0, p = 0;
    ApplyEffectTest(eff, &hp, nullptr, nullptr, nullptr, s, p);
    EXPECT_NEAR(hp.health, 0.0f, 0.001f);
    EXPECT_TRUE(hp.isDead);
}

TEST(Effect_HealRestoresHealth)
{
    HealthData hp{50.0f, 100.0f, false};
    AbilityEffect eff{EffectType::Heal, 30.0f, 1.0f, 0};
    int s = 0, p = 0;
    ApplyEffectTest(eff, &hp, nullptr, nullptr, nullptr, s, p);
    EXPECT_NEAR(hp.health, 80.0f, 0.001f);
}

TEST(Effect_HealClampsToMax)
{
    HealthData hp{90.0f, 100.0f, false};
    AbilityEffect eff{EffectType::Heal, 50.0f, 1.0f, 0};
    int s = 0, p = 0;
    ApplyEffectTest(eff, &hp, nullptr, nullptr, nullptr, s, p);
    EXPECT_NEAR(hp.health, 100.0f, 0.001f);
}

TEST(Effect_SummonCreatesEntity)
{
    Position casterPos{10.0f, 0.0f, 10.0f, 90.0f};
    AbilityEffect eff{EffectType::Summon, 50.0f, 1.0f, 1};
    int s = 0, p = 0;
    ApplyEffectTest(eff, nullptr, nullptr, &casterPos, nullptr, s, p);
    EXPECT_EQ(s, 1);
}

TEST(Effect_TeleportMovesTarget)
{
    Position targetPos{0.0f, 0.0f, 0.0f, 0.0f}; // facing +Z (yaw=0)
    Position casterPos{10.0f, 0.0f, 10.0f, 0.0f};
    AbilityEffect eff{EffectType::Teleport, 5.0f, 1.0f, 0};
    int s = 0, p = 0;
    ApplyEffectTest(eff, nullptr, &targetPos, &casterPos, nullptr, s, p);
    EXPECT_NEAR(targetPos.z, 5.0f, 0.001f); // Moved 5 units along +Z
    EXPECT_NEAR(targetPos.x, 0.0f, 0.01f);  // No lateral movement
}

TEST(Effect_ModifyAttributeIncreasesMaxHealth)
{
    HealthData hp{100.0f, 100.0f, false};
    AbilityEffect eff{EffectType::ModifyAttribute, 50.0f, 1.0f, 0}; // +50 flat
    int s = 0, p = 0;
    ApplyEffectTest(eff, &hp, nullptr, nullptr, nullptr, s, p);
    EXPECT_NEAR(hp.maxHealth, 150.0f, 0.001f);
}

TEST(Effect_ModifyAttributeClampsHealth)
{
    HealthData hp{100.0f, 100.0f, false};
    AbilityEffect eff{EffectType::ModifyAttribute, -60.0f, 1.0f, 0}; // -60 flat
    int s = 0, p = 0;
    ApplyEffectTest(eff, &hp, nullptr, nullptr, nullptr, s, p);
    EXPECT_NEAR(hp.maxHealth, 40.0f, 0.001f);
    EXPECT_NEAR(hp.health, 40.0f, 0.001f); // Clamped to new max
}

TEST(Effect_ApplyForceKnockback)
{
    Position targetPos{5.0f, 0.0f, 5.0f, 0.0f};
    Position casterPos{0.0f, 0.0f, 0.0f, 0.0f};
    Velocity targetVel{0.0f, 0.0f, 0.0f};
    AbilityEffect eff{EffectType::ApplyForce, 10.0f, 1.0f, 0}; // knockback
    int s = 0, p = 0;
    ApplyEffectTest(eff, nullptr, &targetPos, &casterPos, &targetVel, s, p);
    // Force should push target away from caster (positive direction)
    EXPECT_TRUE(targetVel.vx > 0.0f);
    EXPECT_TRUE(targetVel.vz > 0.0f);
}

TEST(Effect_ApplyForceKnockup)
{
    Position targetPos{5.0f, 0.0f, 5.0f, 0.0f};
    Position casterPos{0.0f, 0.0f, 0.0f, 0.0f};
    Velocity targetVel{0.0f, 0.0f, 0.0f};
    AbilityEffect eff{EffectType::ApplyForce, 15.0f, 1.0f, 1}; // knockup (customParam=1)
    int s = 0, p = 0;
    ApplyEffectTest(eff, nullptr, &targetPos, &casterPos, &targetVel, s, p);
    EXPECT_NEAR(targetVel.vy, 15.0f, 0.001f);
    EXPECT_NEAR(targetVel.vx, 0.0f, 0.001f);
}

TEST(Effect_SpawnProjectileCreatesEntity)
{
    Position casterPos{0.0f, 0.0f, 0.0f, 45.0f};
    AbilityEffect eff{EffectType::SpawnProjectile, 50.0f, 1.0f, 1};
    int s = 0, p = 0;
    ApplyEffectTest(eff, nullptr, nullptr, &casterPos, nullptr, s, p);
    EXPECT_EQ(p, 1);
}

TEST(Effect_DamageWithScaling)
{
    HealthData hp{100.0f, 100.0f, false};
    AbilityEffect eff{EffectType::Damage, 20.0f, 1.5f, 0}; // 20 * 1.5 = 30
    int s = 0, p = 0;
    ApplyEffectTest(eff, &hp, nullptr, nullptr, nullptr, s, p);
    EXPECT_NEAR(hp.health, 70.0f, 0.001f);
}
