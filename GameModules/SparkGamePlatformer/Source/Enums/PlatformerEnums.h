/**
 * @file PlatformerEnums.h
 * @brief Enumerations for 3D platformer gameplay systems
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains all enum types used by the Platformer game module: player states,
 * platform types, collectibles, hazards, level themes, and power-ups.
 */

#pragma once

#include <cstdint>

namespace Platformer
{

    /**
     * @brief Player movement and action states
     */
    enum class PlayerState : uint8_t
    {
        Idle = 0,
        Running = 1,
        Jumping = 2,
        DoubleJumping = 3,
        WallSliding = 4,
        WallJumping = 5,
        Dashing = 6,
        Falling = 7,
        GroundPounding = 8,
        Climbing = 9,
        Dead = 10,
        Count = 11
    };

    /**
     * @brief Types of platforms with different behaviors
     */
    enum class PlatformType : uint8_t
    {
        Static = 0,       ///< Fixed in place
        Moving = 1,       ///< Travels between waypoints
        Falling = 2,      ///< Collapses after player stands on it
        Bouncy = 3,       ///< Launches player upward on contact
        Conveyor = 4,     ///< Pushes player in a direction
        Disappearing = 5, ///< Blinks in and out on a timer
        Rotating = 6,     ///< Rotates around an axis
        Count = 7
    };

    /**
     * @brief Types of collectible items
     */
    enum class CollectibleType : uint8_t
    {
        Coin = 0,         ///< Basic currency, abundant
        Gem = 1,          ///< Bonus score, harder to reach
        Star = 2,         ///< 3 per level, unlock progression gates
        HealthPickup = 3, ///< Restores one life
        AbilityOrb = 4,   ///< Unlocks a new movement ability
        Key = 5,          ///< Opens locked doors or gates
        ExtraLife = 6,    ///< Grants an additional life
        Count = 7
    };

    /**
     * @brief Types of environmental hazards
     */
    enum class HazardType : uint8_t
    {
        Spikes = 0,     ///< Instant damage on contact
        Lava = 1,       ///< Damage over time, kills on submersion
        Crusher = 2,    ///< Moving platform that squishes player
        Sawblade = 3,   ///< Rotating hazard following a path
        Projectile = 4, ///< Timed projectile launcher
        WindZone = 5,   ///< Pushes player in a direction
        LaserBeam = 6,  ///< Toggling on/off beam
        Count = 7
    };

    /**
     * @brief Visual themes for levels
     */
    enum class LevelTheme : uint8_t
    {
        Grasslands = 0,
        Desert = 1,
        Snow = 2,
        Lava = 3,
        Sky = 4,
        Underground = 5,
        Factory = 6,
        Count = 7
    };

    /**
     * @brief Temporary power-up effects
     */
    enum class PowerUpType : uint8_t
    {
        DoubleJump = 0, ///< Enables double jump ability
        SpeedBoost = 1, ///< Increases movement speed
        Magnet = 2,     ///< Attracts nearby collectibles
        Shield = 3,     ///< Absorbs one hit without damage
        GhostMode = 4,  ///< Pass through hazards temporarily
        Count = 5
    };

} // namespace Platformer
