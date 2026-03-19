/**
 * @file MaterialEffects.h
 * @brief Material interaction effect system
 * @author Spark Engine Team
 * @date 2026
 *
 * Maps (InteractionType, SurfaceType) pairs to effects such as particle spawns,
 * sound events, and decals. When a bullet hits metal, a footstep lands on wood,
 * or an explosion reaches glass, this system resolves the appropriate audiovisual
 * response.
 *
 * @threadsafety Main thread only.
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Spark::Gameplay
{

    // ============================================================================
    // Interaction & Surface Enums
    // ============================================================================

    /**
     * @brief Type of physical interaction that triggers a material effect.
     */
    enum class InteractionType : uint8_t
    {
        BulletImpact,
        Footstep,
        Explosion,
        MeleeHit,
        ObjectDrop
    };

    /**
     * @brief Surface material type for effect resolution.
     */
    enum class SurfaceType : uint8_t
    {
        Default,
        Metal,
        Wood,
        Stone,
        Dirt,
        Grass,
        Water,
        Glass,
        Flesh,
        Concrete
    };

    // ============================================================================
    // MaterialEffect — data describing the audiovisual response
    // ============================================================================

    /**
     * @brief Describes the particle, sound, and decal effects for a material interaction.
     */
    struct MaterialEffect
    {
        std::string particleEffect; ///< Particle effect name to spawn at impact point.
        std::string soundEvent;     ///< Sound event name to play at impact point.
        std::string decalType;      ///< Decal type name to project onto the surface.
        float particleScale = 1.0f; ///< Scale multiplier for the spawned particle.
        float soundVolume = 1.0f;   ///< Volume multiplier for the sound event (0.0–1.0).
        float decalSize = 0.2f;     ///< World-space size of the projected decal.
    };

    // ============================================================================
    // MaterialEffectSystem
    // ============================================================================

    /**
     * @brief Resolves and triggers material interaction effects.
     *
     * At startup, game code registers effects for (interaction, surface) pairs.
     * At runtime, gameplay systems call TriggerEffect() when an interaction occurs,
     * and this system looks up and dispatches the appropriate particle/sound/decal.
     *
     * Uses a flat map keyed by (interaction << 8 | surface) for O(1) lookup.
     */
    class MaterialEffectSystem
    {
      public:
        static MaterialEffectSystem& GetInstance()
        {
            static MaterialEffectSystem s;
            return s;
        }

        /**
         * @brief Initialize the system and clear any previously registered effects.
         */
        void Initialize();

        /**
         * @brief Shut down and release all registered effects.
         */
        void Shutdown();

        /**
         * @brief Register an effect for a specific (interaction, surface) pair.
         * @param interaction The interaction type (e.g. BulletImpact).
         * @param surface     The surface type (e.g. Metal).
         * @param effect      The effect data to associate with this pair.
         *
         * Overwrites any previously registered effect for the same pair.
         */
        void RegisterEffect(InteractionType interaction, SurfaceType surface, const MaterialEffect& effect);

        /**
         * @brief Look up the effect for a given interaction and surface.
         * @param interaction The interaction type.
         * @param surface     The surface type.
         * @return Pointer to the registered effect, or nullptr if none is registered.
         */
        const MaterialEffect* GetEffect(InteractionType interaction, SurfaceType surface) const;

        /**
         * @brief Trigger the effect for an interaction at a world position.
         * @param interaction The interaction type.
         * @param surface     The surface type.
         * @param position    World-space position of the interaction.
         * @param normal      Surface normal at the interaction point.
         *
         * Looks up the registered effect and dispatches particle, sound, and decal
         * creation. If no effect is registered, falls back to the Default surface.
         * If no default exists either, the call is silently ignored.
         */
        void TriggerEffect(InteractionType interaction, SurfaceType surface, const XMFLOAT3& position,
                           const XMFLOAT3& normal);

        /** @brief Get the number of registered effect entries. */
        uint32_t GetRegisteredEffectCount() const { return static_cast<uint32_t>(m_effects.size()); }

      private:
        MaterialEffectSystem() = default;
        ~MaterialEffectSystem() = default;

        MaterialEffectSystem(const MaterialEffectSystem&) = delete;
        MaterialEffectSystem& operator=(const MaterialEffectSystem&) = delete;

        /** @brief Build a flat key from interaction and surface types. */
        static uint16_t MakeKey(InteractionType interaction, SurfaceType surface)
        {
            return static_cast<uint16_t>((static_cast<uint8_t>(interaction) << 8) | static_cast<uint8_t>(surface));
        }

        std::unordered_map<uint16_t, MaterialEffect> m_effects;
    };

} // namespace Spark::Gameplay
