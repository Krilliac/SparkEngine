/**
 * @file MMOEnums.h
 * @brief Enumerations for MMO gameplay systems
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains all enum types used by the MMO game module's gameplay systems:
 * guilds, parties, crafting, reputation, dungeons, world bosses, etc.
 */

#pragma once

#include <cstdint>

namespace MMO
{

    /**
     * @brief Guild rank levels with ascending privilege
     */
    enum class GuildRank : uint8_t
    {
        Initiate = 0, ///< New member, minimal permissions
        Member = 1,   ///< Standard guild member
        Veteran = 2,  ///< Experienced member with extra access
        Officer = 3,  ///< Management duties, can invite/kick
        Leader = 4,   ///< Guild owner with full control
        Count = 5
    };

    /**
     * @brief Party role assignments for group content
     */
    enum class PartyRole : uint8_t
    {
        None = 0,
        Tank = 1,
        Healer = 2,
        DPS = 3,
        Support = 4,
        Count = 5
    };

    /**
     * @brief Loot distribution rules for party/raid content
     */
    enum class LootRule : uint8_t
    {
        FreeForAll = 0,
        RoundRobin = 1,
        NeedBeforeGreed = 2,
        MasterLooter = 3,
        Count = 4
    };

    /**
     * @brief Crafting station types required for recipes
     */
    enum class CraftingStation : uint8_t
    {
        None = 0,
        Forge = 1,
        AlchemyLab = 2,
        Workbench = 3,
        Enchanting = 4,
        CookingFire = 5,
        Count = 6
    };

    /**
     * @brief Crafting skill disciplines
     */
    enum class CraftingDiscipline : uint8_t
    {
        Weaponsmithing = 0,
        Armorsmithing = 1,
        Alchemy = 2,
        Engineering = 3,
        Cooking = 4,
        Enchanting = 5,
        Count = 6
    };

    /**
     * @brief Reputation standing tiers with a faction
     */
    enum class ReputationTier : uint8_t
    {
        Hated = 0,
        Hostile = 1,
        Unfriendly = 2,
        Neutral = 3,
        Friendly = 4,
        Honored = 5,
        Revered = 6,
        Exalted = 7,
        Count = 8
    };

    /**
     * @brief Dungeon difficulty modes
     */
    enum class DungeonDifficulty : uint8_t
    {
        Normal = 0,
        Heroic = 1,
        Mythic = 2,
        Legendary = 3,
        Count = 4
    };

    /**
     * @brief World boss phases for multi-stage encounters
     */
    enum class BossPhase : uint8_t
    {
        Dormant = 0,
        Spawning = 1,
        Phase1 = 2,
        Transition = 3,
        Phase2 = 4,
        Phase3 = 5,
        Defeated = 6,
        Count = 7
    };

    /**
     * @brief Auction house listing duration
     */
    enum class AuctionDuration : uint8_t
    {
        Short = 0,  ///< 12 hours
        Medium = 1, ///< 24 hours
        Long = 2,   ///< 48 hours
        Count = 3
    };

    /**
     * @brief Talent tree specialization paths
     */
    enum class TalentSpec : uint8_t
    {
        Offense = 0,
        Defense = 1,
        Utility = 2,
        Count = 3
    };

    /**
     * @brief Item rarity tiers for loot and crafting output
     */
    enum class ItemRarity : uint8_t
    {
        Common = 0,
        Uncommon = 1,
        Rare = 2,
        Epic = 3,
        Legendary = 4,
        Count = 5
    };

    /**
     * @brief Item categories for inventory filtering
     */
    enum class ItemCategory : uint8_t
    {
        Weapon = 0,
        Armor = 1,
        Consumable = 2,
        Material = 3,
        Quest = 4,
        Misc = 5,
        Count = 6
    };

    /**
     * @brief Achievement categories
     */
    enum class AchievementCategory : uint8_t
    {
        Story = 0,
        Combat = 1,
        Exploration = 2,
        Collection = 3,
        Social = 4,
        Crafting = 5,
        PvP = 6,
        Dungeon = 7,
        Count = 8
    };

    /**
     * @brief Guild permission flags (bitfield)
     */
    enum class GuildPermission : uint32_t
    {
        None = 0,
        Invite = 1 << 0,
        Kick = 1 << 1,
        Promote = 1 << 2,
        EditMotd = 1 << 3,
        WithdrawBank = 1 << 4,
        StartEvent = 1 << 5,
        OfficerChat = 1 << 6,
        All = 0xFFFFFFFF
    };

    inline GuildPermission operator|(GuildPermission a, GuildPermission b)
    {
        return static_cast<GuildPermission>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    inline bool HasPermission(GuildPermission perms, GuildPermission flag)
    {
        return (static_cast<uint32_t>(perms) & static_cast<uint32_t>(flag)) != 0;
    }

} // namespace MMO
