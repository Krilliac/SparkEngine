/**
 * @file MaterialEffects.cpp
 * @brief Material interaction effect system implementation
 */

#include "MaterialEffects.h"

namespace Spark::Gameplay
{

    // ============================================================================
    // Initialization / Shutdown
    // ============================================================================

    void MaterialEffectSystem::Initialize()
    {
        m_effects.clear();
    }

    void MaterialEffectSystem::Shutdown()
    {
        m_effects.clear();
    }

    // ============================================================================
    // Registration
    // ============================================================================

    void MaterialEffectSystem::RegisterEffect(InteractionType interaction, SurfaceType surface,
                                              const MaterialEffect& effect)
    {
        uint16_t key = MakeKey(interaction, surface);
        m_effects.insert_or_assign(key, effect);
    }

    // ============================================================================
    // Lookup
    // ============================================================================

    const MaterialEffect* MaterialEffectSystem::GetEffect(InteractionType interaction, SurfaceType surface) const
    {
        uint16_t key = MakeKey(interaction, surface);
        auto it = m_effects.find(key);
        if (it != m_effects.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    // ============================================================================
    // Trigger
    // ============================================================================

    void MaterialEffectSystem::TriggerEffect(InteractionType interaction, SurfaceType surface, const XMFLOAT3& position,
                                             const XMFLOAT3& normal)
    {
        // Look up the specific (interaction, surface) pair first
        const MaterialEffect* effect = GetEffect(interaction, surface);

        // Fall back to the Default surface type if no specific effect is registered
        if (effect == nullptr && surface != SurfaceType::Default)
        {
            effect = GetEffect(interaction, SurfaceType::Default);
        }

        if (effect == nullptr)
        {
            return;
        }

        // Dispatch particle effect
        // Integration point: forward to the engine's particle system when available.
        // ParticleSystem::GetInstance().Spawn(effect->particleEffect, position, normal, effect->particleScale);
        (void)position;
        (void)normal;

        // Dispatch sound event
        // Integration point: forward to the engine's audio system when available.
        // AudioSystem::GetInstance().PlayEvent(effect->soundEvent, position, effect->soundVolume);

        // Dispatch decal
        // Integration point: forward to the engine's decal system when available.
        // DecalSystem::GetInstance().Project(effect->decalType, position, normal, effect->decalSize);
    }

} // namespace Spark::Gameplay
