/**
 * @file FPSComponents.h
 * @brief ECS components for FPS gameplay: Decals, Projectiles, Interactions
 * @author Spark Engine Team
 * @date 2025
 *
 * Split from the monolithic Components.h for faster compilation and
 * clearer separation of concerns.
 */

#pragma once
#include "../../../Core/Platform.h"
#include "../../../Utils/OpaqueHandle.h"
#include "../../../Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>

// =============================================================================
// DecalComponent — Projected texture decals (bullet holes, blood, scorch marks)
// =============================================================================

/**
 * @struct DecalComponent
 * @brief Renders a projected texture onto nearby surfaces.
 *
 * Used for bullet impact marks, blood splatter, scorch marks, and other
 * surface effects common in FPS games. The decal is projected along the
 * entity's forward axis (-Z in local space) onto surfaces within range.
 *
 * @note Decals fade out over their lifetime and are automatically removed
 *       by the DecalSystem when `remainingLifetime` reaches zero.
 */
struct DecalComponent
{
    /// Path to the decal texture atlas or individual texture
    std::string texturePath;

    /// Category for grouping and limits (e.g. "bullet", "blood", "explosion")
    std::string category = "generic";

    /// Projection half-extents: width (X), height (Y), depth (Z)
    DirectX::XMFLOAT3 size{0.1f, 0.1f, 0.05f};

    /// Tint color applied to the decal texture (RGBA)
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};

    /// Total lifetime in seconds (0 = permanent)
    float lifetime = 30.0f;

    /// Time remaining before removal (counts down each frame)
    float remainingLifetime = 30.0f;

    /// Duration of the fade-out at end of life (seconds)
    float fadeOutDuration = 2.0f;

    /// Normal of the surface the decal is projected onto
    DirectX::XMFLOAT3 surfaceNormal{0.0f, 1.0f, 0.0f};

    /// Whether the decal should receive lighting or be unlit
    bool receiveLighting = true;

    /// Sort priority for overlapping decals (higher draws on top)
    int sortOrder = 0;

    /**
     * @brief Get the current opacity based on remaining lifetime.
     * @return Opacity in [0, 1] range, fading during the last fadeOutDuration seconds.
     */
    float GetCurrentOpacity() const
    {
        if (lifetime <= 0.0f)
            return color.w; // Permanent decal
        if (remainingLifetime <= 0.0f)
            return 0.0f;
        if (fadeOutDuration > 0.0f && remainingLifetime < fadeOutDuration)
            return color.w * (remainingLifetime / fadeOutDuration);
        return color.w;
    }
};

// =============================================================================
// ProjectileComponent — Bullet/rocket/grenade projectile tracking
// =============================================================================

/**
 * @struct ProjectileComponent
 * @brief Tracks an in-flight projectile (bullet, rocket, grenade, etc.).
 *
 * The ProjectileSystem advances projectiles each frame using either
 * ray-cast (hitscan) or physics-based (ballistic) movement. On impact
 * the system spawns decals, particle effects, and applies damage.
 */
struct ProjectileComponent
{
    /// Movement model for the projectile
    enum class MovementType
    {
        Hitscan,  ///< Instant ray-cast; hits on the frame it's fired
        Ballistic ///< Physics-based arc with gravity
    };

    /// What happens when the projectile hits a surface
    enum class ImpactBehavior
    {
        Destroy, ///< Destroy on first impact
        Bounce,  ///< Ricochet off surfaces (reduces bounces remaining)
        Pierce,  ///< Pass through targets (reduces pierces remaining)
        Stick    ///< Attach to the hit surface/entity
    };

    MovementType movementType = MovementType::Ballistic;
    ImpactBehavior impactBehavior = ImpactBehavior::Destroy;

    /// Travel direction (normalized, world-space)
    DirectX::XMFLOAT3 direction{0.0f, 0.0f, 1.0f};

    /// Speed in units per second
    float speed = 100.0f;

    /// Gravity multiplier for ballistic projectiles (0 = no gravity)
    float gravityScale = 1.0f;

    /// Base damage dealt on impact
    float damage = 25.0f;

    /// Explosion radius (0 = no splash damage)
    float explosionRadius = 0.0f;

    /// Maximum travel distance before auto-destroy
    float maxRange = 500.0f;

    /// Distance traveled so far
    float distanceTraveled = 0.0f;

    /// Maximum lifetime in seconds (safety net)
    float maxLifetime = 10.0f;

    /// Time alive so far
    float age = 0.0f;

    /// Remaining bounces for ricochet projectiles
    int bouncesRemaining = 0;

    /// Remaining pierces for penetrating projectiles
    int piercesRemaining = 0;

    /// Entity that fired this projectile (for friendly fire checks)
    uint32_t ownerEntityId = 0;

    /// Team ID for team damage rules
    int teamId = -1;

    /// Particle effect to spawn on impact
    std::string impactEffectName;

    /// Decal to spawn on surface impact
    std::string impactDecalPath;

    /// Trail particle effect name
    std::string trailEffectName;

    /**
     * @brief Check if the projectile has exceeded its range or lifetime.
     * @return true if the projectile should be destroyed.
     */
    bool IsExpired() const { return distanceTraveled >= maxRange || age >= maxLifetime; }

    /**
     * @brief Validate that projectile parameters are within sane ranges.
     * @return true if all parameters are valid.
     */
    bool Validate() const
    {
        ASSERT_MSG(speed >= 0.0f, "Projectile speed must be non-negative");
        ASSERT_MSG(damage >= 0.0f, "Projectile damage must be non-negative");
        ASSERT_MSG(maxRange > 0.0f, "Projectile maxRange must be positive");
        ASSERT_MSG(maxLifetime > 0.0f, "Projectile maxLifetime must be positive");
        ASSERT_MSG(explosionRadius >= 0.0f, "Projectile explosionRadius must be non-negative");
        return speed >= 0.0f && damage >= 0.0f && maxRange > 0.0f && maxLifetime > 0.0f && explosionRadius >= 0.0f;
    }
};

// =============================================================================
// InteractionComponent — Interactable objects (doors, buttons, pickups)
// =============================================================================

/**
 * @struct InteractionComponent
 * @brief Marks an entity as interactable by the player or AI.
 *
 * Used for doors, switches, pickups, levers, terminals, and other
 * objects the player can interact with. The interaction system shows
 * UI prompts when the player is within range and looking at the object.
 */
struct InteractionComponent
{
    /// Type of interaction
    enum class InteractionType
    {
        Use,    ///< Press 'E' to use (doors, switches, buttons)
        Pickup, ///< Auto-pickup or press to collect (ammo, health)
        Hold,   ///< Hold to interact (defuse, hack, revive)
        Toggle  ///< Toggle on/off (lights, generators)
    };

    /// Current state of the interactable
    enum class State
    {
        Idle,     ///< Ready for interaction
        Active,   ///< Currently being interacted with (hold type)
        Cooldown, ///< Temporarily unavailable after use
        Disabled, ///< Cannot be interacted with
        Destroyed ///< Permanently unusable
    };

    InteractionType type = InteractionType::Use;
    State state = State::Idle;

    /// Display name shown in the interaction prompt (e.g. "Door", "Health Pack")
    std::string displayName;

    /// Action verb shown in the prompt (e.g. "Open", "Pick up", "Hack")
    std::string actionVerb = "Use";

    /// Maximum distance from which the player can interact
    float interactionRadius = 2.5f;

    /// Required hold duration for Hold-type interactions (seconds)
    float holdDuration = 0.0f;

    /// Current hold progress for Hold-type interactions [0, 1]
    float holdProgress = 0.0f;

    /// Cooldown duration after interaction (seconds)
    float cooldownDuration = 0.0f;

    /// Remaining cooldown time
    float cooldownRemaining = 0.0f;

    /// Number of times this can be used (-1 = unlimited)
    int usesRemaining = -1;

    /// Whether to show an outline highlight when in range
    bool showHighlight = true;

    /// Whether this interaction requires a specific key/item
    std::string requiredItemId;

    /// Script event name triggered on interaction
    std::string onInteractEvent;

    /// Sound to play on interaction
    std::string interactSoundName;

    /**
     * @brief Check if the interaction can be triggered.
     * @return true if the entity is in a state that allows interaction.
     */
    bool CanInteract() const { return state == State::Idle && (usesRemaining != 0); }

    /**
     * @brief Consume one use of the interaction.
     */
    void ConsumeUse()
    {
        ASSERT_MSG(CanInteract(), "ConsumeUse called when interaction is not available");
        if (usesRemaining > 0)
            --usesRemaining;
    }

    /**
     * @brief Validate that interaction parameters are within sane ranges.
     * @return true if all parameters are valid.
     */
    bool Validate() const
    {
        ASSERT_MSG(interactionRadius > 0.0f, "Interaction radius must be positive");
        ASSERT_MSG(holdDuration >= 0.0f, "Hold duration must be non-negative");
        ASSERT_MSG(cooldownDuration >= 0.0f, "Cooldown duration must be non-negative");
        return interactionRadius > 0.0f && holdDuration >= 0.0f && cooldownDuration >= 0.0f;
    }
};
