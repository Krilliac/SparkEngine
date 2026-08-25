/**
 * @file ARPGAbilityCatalog.h
 * @brief Engine-native ability, aura, and proc definitions for SparkGameARPG.
 */

#pragma once

#include "Engine/Gameplay/AbilitySystem.h"

#include <cstdint>

namespace ARPG
{
    /** @brief Stable IDs and registration routine for the ARPG engine ability bridge. */
    struct ARPGAbilityCatalog
    {
        static constexpr uint32_t FIREBALL_ABILITY_ID = 0xA0010001u;
        static constexpr uint32_t WHIRLWIND_ABILITY_ID = 0xA0010002u;
        static constexpr uint32_t RAISE_SKELETON_ABILITY_ID = 0xA0010003u;
        static constexpr uint32_t HOLY_LIGHT_ABILITY_ID = 0xA0010004u;
        static constexpr uint32_t HOLY_SHIELD_AURA_ID = 0xA0020001u;
        static constexpr uint32_t BONE_ARMOR_AURA_ID = 0xA0020002u;
        static constexpr uint32_t POISON_AURA_ID = 0xA0020003u;
        static constexpr uint32_t FIRE_MASTERY_AURA_ID = 0xA0020004u;

        /**
         * @brief Register or refresh the complete ARPG catalog.
         *
         * Ability and aura definitions overwrite by stable ID. The proc registry is
         * append-only, so the reserved Fire Mastery aura doubles as a hot-reload sentinel.
         */
        static void Register(Spark::Gameplay::AbilitySystem& abilities)
        {
            using namespace Spark::Gameplay;

            const bool definitionsAlreadyRegistered = abilities.GetAuraDef(FIRE_MASTERY_AURA_ID) != nullptr;

            AbilityDefinition fireball;
            fireball.id = FIREBALL_ABILITY_ID;
            fireball.name = "ARPG Fireball";
            fireball.description = "Launches a fire projectile at a single target.";
            fireball.targetType = AbilityTargetType::Projectile;
            fireball.range = 24.0f;
            fireball.castTime = 0.35f;
            fireball.cooldown = 1.5f;
            fireball.resourceCost = 12.0f;
            fireball.effects.push_back({EffectType::Damage, 45.0f, 1.0f, AbilitySchool::Fire});
            abilities.RegisterAbility(fireball);

            AbilityDefinition whirlwind;
            whirlwind.id = WHIRLWIND_ABILITY_ID;
            whirlwind.name = "ARPG Whirlwind";
            whirlwind.description = "Strikes nearby enemies while the hero spins.";
            whirlwind.targetType = AbilityTargetType::AreaOfEffect;
            whirlwind.radius = 4.0f;
            whirlwind.cooldown = 4.0f;
            whirlwind.resourceCost = 18.0f;
            whirlwind.requiresTarget = false;
            whirlwind.canCastWhileMoving = true;
            whirlwind.effects.push_back({EffectType::Damage, 35.0f, 1.0f, AbilitySchool::Physical});
            abilities.RegisterAbility(whirlwind);

            AbilityDefinition raiseSkeleton;
            raiseSkeleton.id = RAISE_SKELETON_ABILITY_ID;
            raiseSkeleton.name = "ARPG Raise Skeleton";
            raiseSkeleton.description = "Summons an allied skeleton warrior.";
            raiseSkeleton.targetType = AbilityTargetType::Self;
            raiseSkeleton.cooldown = 5.0f;
            raiseSkeleton.resourceCost = 15.0f;
            raiseSkeleton.requiresTarget = false;
            raiseSkeleton.effects.push_back({EffectType::Summon, 0.0f, 1.0f, AbilitySchool::Shadow, 0, 0xA0030001u});
            abilities.RegisterAbility(raiseSkeleton);

            AbilityDefinition holyLight;
            holyLight.id = HOLY_LIGHT_ABILITY_ID;
            holyLight.name = "ARPG Holy Light";
            holyLight.description = "Restores health to the selected ally.";
            holyLight.targetType = AbilityTargetType::SingleTarget;
            holyLight.range = 18.0f;
            holyLight.castTime = 0.25f;
            holyLight.cooldown = 2.0f;
            holyLight.resourceCost = 10.0f;
            holyLight.effects.push_back({EffectType::Heal, 40.0f, 1.0f, AbilitySchool::Holy});
            abilities.RegisterAbility(holyLight);

            AuraDefinition holyShield;
            holyShield.id = HOLY_SHIELD_AURA_ID;
            holyShield.name = "ARPG Holy Shield";
            holyShield.type = AuraType::Shield;
            holyShield.school = AbilitySchool::Holy;
            holyShield.duration = 8.0f;
            holyShield.flatModifier = 40.0f;
            abilities.RegisterAura(holyShield);

            AuraDefinition boneArmor;
            boneArmor.id = BONE_ARMOR_AURA_ID;
            boneArmor.name = "ARPG Bone Armor";
            boneArmor.type = AuraType::Shield;
            boneArmor.school = AbilitySchool::Shadow;
            boneArmor.duration = 10.0f;
            boneArmor.flatModifier = 30.0f;
            abilities.RegisterAura(boneArmor);

            AuraDefinition poison;
            poison.id = POISON_AURA_ID;
            poison.name = "ARPG Poison";
            poison.type = AuraType::DamageOverTime;
            poison.school = AbilitySchool::Nature;
            poison.duration = 6.0f;
            poison.tickInterval = 1.0f;
            poison.valuePerTick = 7.0f;
            poison.stackType = AuraStackType::Stacking;
            poison.maxStacks = 3;
            abilities.RegisterAura(poison);

            AuraDefinition fireMastery;
            fireMastery.id = FIRE_MASTERY_AURA_ID;
            fireMastery.name = "ARPG Fire Mastery";
            fireMastery.type = AuraType::ModifyDamageDealt;
            fireMastery.school = AbilitySchool::Fire;
            fireMastery.duration = 0.0f;
            fireMastery.percentModifier = 0.15f;
            fireMastery.isPermanent = true;
            abilities.RegisterAura(fireMastery);

            if (!definitionsAlreadyRegistered)
            {
                ProcDefinition fireMasteryProc;
                fireMasteryProc.sourceAuraId = FIRE_MASTERY_AURA_ID;
                fireMasteryProc.triggeredAbilityId = FIREBALL_ABILITY_ID;
                fireMasteryProc.triggerMask = static_cast<uint32_t>(ProcTrigger::OnDealDamage);
                fireMasteryProc.chance = 10.0f;
                fireMasteryProc.cooldown = 2.0f;
                fireMasteryProc.schoolMask = 1u << static_cast<uint32_t>(AbilitySchool::Fire);
                abilities.RegisterProc(fireMasteryProc);
            }
        }
    };
} // namespace ARPG
