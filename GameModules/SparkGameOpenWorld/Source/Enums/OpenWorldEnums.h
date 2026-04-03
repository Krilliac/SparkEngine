/**
 * @file OpenWorldEnums.h
 * @brief Enumerations for open world gameplay systems
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains all enum types used by the open world game module: biomes,
 * wildlife, resources, settlements, exploration, player survival,
 * dynamic events, and crafting.
 */

#pragma once

#include <cstdint>

namespace OpenWorld
{

    /// @brief World biome types affecting terrain, weather, wildlife, and resources
    enum class Biome : uint8_t
    {
        Grasslands = 0,
        Forest = 1,
        Mountains = 2,
        Desert = 3,
        Tundra = 4,
        Swamp = 5,
        Coast = 6,
        Volcanic = 7,
        Count = 8
    };

    /// @brief Wildlife species categories
    enum class AnimalType : uint8_t
    {
        Deer = 0,
        Wolf = 1,
        Bear = 2,
        Boar = 3,
        Eagle = 4,
        Horse = 5,
        Rabbit = 6,
        Elk = 7,
        Fox = 8,
        Bison = 9,
        MountainLion = 10,
        Snake = 11,
        Count = 12
    };

    /// @brief Wildlife behavioral state
    enum class AnimalBehavior : uint8_t
    {
        Grazing = 0,
        Roaming = 1,
        Fleeing = 2,
        Stalking = 3,
        Attacking = 4,
        Sleeping = 5,
        Following = 6, ///< Tamed, following player
        Count = 7
    };

    /// @brief Wildlife diet classification (drives predator-prey interactions)
    enum class DietType : uint8_t
    {
        Herbivore = 0,
        Carnivore = 1,
        Omnivore = 2,
        Count = 3
    };

    /// @brief Harvestable resource categories
    enum class ResourceType : uint8_t
    {
        Wood = 0,
        Stone = 1,
        Iron = 2,
        Herbs = 3,
        Fiber = 4,
        Hide = 5,
        Meat = 6,
        Water = 7,
        Gold = 8,
        Crystal = 9,
        Clay = 10,
        Count = 11
    };

    /// @brief Crafting recipe categories
    enum class CraftingCategory : uint8_t
    {
        Tools = 0,
        Weapons = 1,
        Armor = 2,
        Consumables = 3,
        Building = 4,
        Alchemy = 5,
        Count = 6
    };

    /// @brief Point of interest types on the world map
    enum class POIType : uint8_t
    {
        Landmark = 0,
        Ruins = 1,
        Cave = 2,
        Shrine = 3,
        Viewpoint = 4,
        Camp = 5,
        Dungeon = 6,
        Village = 7,
        FastTravel = 8,
        TreasureCache = 9,
        Count = 10
    };

    /// @brief Discovery state for map fog / exploration progress
    enum class DiscoveryState : uint8_t
    {
        Undiscovered = 0,
        Revealed = 1, ///< Seen from viewpoint but not visited
        Visited = 2,
        Completed = 3, ///< All objectives/secrets found
        Count = 4
    };

    /// @brief Settlement types in the world
    enum class SettlementType : uint8_t
    {
        Village = 0,
        Town = 1,
        Outpost = 2,
        Farmstead = 3,
        PlayerCamp = 4,
        BanditCamp = 5,
        Count = 6
    };

    /// @brief NPC roles within settlements
    enum class NPCRole : uint8_t
    {
        Merchant = 0,
        Blacksmith = 1,
        Innkeeper = 2,
        Guard = 3,
        Farmer = 4,
        Hunter = 5,
        Elder = 6,
        Wanderer = 7,
        Count = 8
    };

    /// @brief Player survival stat categories
    enum class SurvivalStat : uint8_t
    {
        Health = 0,
        Stamina = 1,
        Hunger = 2,
        Thirst = 3,
        Temperature = 4,
        Count = 5
    };

    /// @brief Dynamic world event types
    enum class WorldEventType : uint8_t
    {
        BanditRaid = 0,
        WildlifeStampede = 1,
        MerchantCaravan = 2,
        WeatherStorm = 3,
        VolcanicEruption = 4,
        DragonSighting = 5,
        HarvestFestival = 6,
        AncientAwakening = 7,
        Count = 8
    };

    /// @brief Event lifecycle state
    enum class EventState : uint8_t
    {
        Dormant = 0,
        Approaching = 1,
        Active = 2,
        Resolving = 3,
        Completed = 4,
        Count = 5
    };

    /// @brief Temperature range affecting survival
    enum class TemperatureZone : uint8_t
    {
        Freezing = 0,
        Cold = 1,
        Temperate = 2,
        Warm = 3,
        Hot = 4,
        Scorching = 5,
        Count = 6
    };

    /// @brief Time-of-day periods that affect gameplay
    enum class DayPeriod : uint8_t
    {
        Dawn = 0,
        Morning = 1,
        Midday = 2,
        Afternoon = 3,
        Dusk = 4,
        Night = 5,
        Count = 6
    };

    /// @brief Player camp upgrade tiers
    enum class CampTier : uint8_t
    {
        Campfire = 0,  ///< Basic fire, no shelter
        Lean_To = 1,   ///< Basic shelter from rain
        Tent = 2,      ///< Full weather protection, storage
        Cabin = 3,     ///< Permanent structure, crafting stations
        Homestead = 4, ///< Full base with all facilities
        Count = 5
    };

} // namespace OpenWorld
