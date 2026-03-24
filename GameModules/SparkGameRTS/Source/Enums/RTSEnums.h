/**
 * @file RTSEnums.h
 * @brief Enumerations for RTS gameplay systems
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains all enum types used by the RTS game module: factions,
 * unit types, building types, resources, commands, and match states.
 */

#pragma once

#include <cstdint>

namespace RTS
{

    /**
     * @brief Playable factions with distinct units, buildings, and playstyles
     *
     * Human: versatile, balanced forces with strong infantry
     * Sentinel: advanced technology, shields, powerful but expensive units
     * Swarm: fast reproduction, cheap units, overwhelm through numbers
     */
    enum class RTSFaction : uint8_t
    {
        Human = 0,
        Sentinel = 1,
        Swarm = 2,
        Count = 3
    };

    /**
     * @brief Unit archetypes available for production
     */
    enum class RTSUnitType : uint8_t
    {
        Worker = 0,
        Marine = 1,
        Tank = 2,
        Flyer = 3,
        Scout = 4,
        Hero = 5,
        Medic = 6,
        Siege = 7,
        Count = 8
    };

    /**
     * @brief Structure types that can be constructed
     */
    enum class RTSBuildingType : uint8_t
    {
        CommandCenter = 0,
        Barracks = 1,
        Factory = 2,
        TechLab = 3,
        SupplyDepot = 4,
        Refinery = 5,
        Turret = 6,
        Starport = 7,
        Count = 8
    };

    /**
     * @brief Resource categories for the economy
     */
    enum class RTSResourceType : uint8_t
    {
        Minerals = 0,
        Gas = 1,
        Supply = 2,
        Count = 3
    };

    /**
     * @brief Commands that can be issued to units
     */
    enum class RTSCommandType : uint8_t
    {
        Move = 0,
        Attack = 1,
        Patrol = 2,
        Hold = 3,
        Build = 4,
        Gather = 5,
        Stop = 6,
        Count = 7
    };

    /**
     * @brief Match progression states
     */
    enum class RTSMatchState : uint8_t
    {
        Setup = 0,
        Playing = 1,
        Paused = 2,
        Victory = 3,
        Defeat = 4,
        Count = 5
    };

    /**
     * @brief Unit behavioral states for AI
     */
    enum class RTSUnitState : uint8_t
    {
        Idle = 0,
        Moving = 1,
        Attacking = 2,
        Gathering = 3,
        Building = 4,
        Patrolling = 5,
        Holding = 6,
        Dead = 7,
        Count = 8
    };

    /**
     * @brief Fog of war visibility levels
     */
    enum class RTSVisibility : uint8_t
    {
        Unexplored = 0,
        Fog = 1,
        Visible = 2,
        Count = 3
    };

} // namespace RTS
