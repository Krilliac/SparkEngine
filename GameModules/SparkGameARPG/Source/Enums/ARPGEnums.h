/**
 * @file ARPGEnums.h
 * @brief Enumerations for ARPG gameplay systems
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains all enum types used by the ARPG game module: hero classes,
 * damage types, item rarity/slots, monster ranks, skill types, and
 * dungeon difficulty tiers.
 */

#pragma once

#include <cstdint>

namespace ARPG
{

    /**
     * @brief Playable hero classes inspired by classic ARPGs
     */
    enum class ARPGHeroClass : uint8_t
    {
        Barbarian = 0,
        Sorceress = 1,
        Necromancer = 2,
        Paladin = 3,
        Amazon = 4,
        Rogue = 5,
        Count = 6
    };

    /**
     * @brief Damage types for combat and resistance calculations
     */
    enum class ARPGDamageType : uint8_t
    {
        Physical = 0,
        Fire = 1,
        Cold = 2,
        Lightning = 3,
        Poison = 4,
        Arcane = 5,
        Holy = 6,
        Count = 7
    };

    /**
     * @brief Item rarity tiers affecting affix count and power
     */
    enum class ARPGItemRarity : uint8_t
    {
        Normal = 0,
        Magic = 1,
        Rare = 2,
        Legendary = 3,
        Set = 4,
        Unique = 5,
        Count = 6
    };

    /**
     * @brief Equipment slot positions on a hero
     */
    enum class ARPGItemSlot : uint8_t
    {
        Helm = 0,
        Chest = 1,
        Gloves = 2,
        Boots = 3,
        Belt = 4,
        Ring1 = 5,
        Ring2 = 6,
        Amulet = 7,
        MainHand = 8,
        OffHand = 9,
        Count = 10
    };

    /**
     * @brief Monster threat ranking
     */
    enum class ARPGMonsterRank : uint8_t
    {
        Normal = 0,
        Champion = 1,
        Elite = 2,
        MiniBoss = 3,
        Boss = 4,
        Count = 5
    };

    /**
     * @brief Skill activation types
     */
    enum class ARPGSkillType : uint8_t
    {
        Active = 0,
        Passive = 1,
        Aura = 2,
        Channeled = 3,
        Summon = 4,
        Count = 5
    };

    /**
     * @brief Dungeon difficulty tiers with increasing rewards and danger
     */
    enum class ARPGDungeonTier : uint8_t
    {
        Normal = 0,
        Nightmare = 1,
        Hell = 2,
        Inferno = 3,
        Count = 4
    };

} // namespace ARPG
