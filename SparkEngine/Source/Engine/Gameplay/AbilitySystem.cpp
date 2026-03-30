/**
 * @file AbilitySystem.cpp
 * @brief Implementation of the pipeline-based ability/spell system
 */

#include "AbilitySystem.h"
#include "../../Utils/DeferredDeletion.h"
#include "../../Utils/LogMacros.h"
#include "../ECS/Components/GameplayComponents.h"
#include "../Events/EventSystem.h"

#include <algorithm>
#include <random>

// World is defined in Components.h (umbrella) — we need the full definition here
#include "../ECS/Components.h"

namespace Spark::Gameplay
{

    // ============================================================================
    // Singleton
    // ============================================================================

    AbilitySystem& AbilitySystem::GetInstance()
    {
        static AbilitySystem instance;
        return instance;
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    void AbilitySystem::Initialize(EventBus* eventBus)
    {
        m_eventBus = eventBus;
        m_abilities.clear();
        m_auras.clear();
        m_procs.clear();
        m_activeCasts.clear();
        m_activeAuras.clear();
        m_cooldowns.clear();

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[AbilitySystem] Initialized");
    }

    void AbilitySystem::Shutdown()
    {
        m_activeCasts.clear();
        m_activeAuras.clear();
        m_cooldowns.clear();
        m_eventBus = nullptr;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[AbilitySystem] Shut down");
    }

    // ============================================================================
    // Registration
    // ============================================================================

    void AbilitySystem::RegisterAbility(const AbilityDefinition& def)
    {
        if (def.id == 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[AbilitySystem] Cannot register ability with id 0");
            return;
        }
        m_abilities[def.id] = def;
    }

    void AbilitySystem::RegisterAura(const AuraDefinition& def)
    {
        if (def.id == 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[AbilitySystem] Cannot register aura with id 0");
            return;
        }
        m_auras[def.id] = def;
    }

    void AbilitySystem::RegisterProc(const ProcDefinition& def)
    {
        m_procs.push_back(def);
    }

    const AbilityDefinition* AbilitySystem::GetAbilityDef(AbilityID id) const
    {
        auto it = m_abilities.find(id);
        return (it != m_abilities.end()) ? &it->second : nullptr;
    }

    const AuraDefinition* AbilitySystem::GetAuraDef(AuraID id) const
    {
        auto it = m_auras.find(id);
        return (it != m_auras.end()) ? &it->second : nullptr;
    }

    // ============================================================================
    // Cast Pipeline
    // ============================================================================

    bool AbilitySystem::CastAbility(World& world, uint32_t caster, AbilityID abilityId, uint32_t target)
    {
        const AbilityDefinition* def = GetAbilityDef(abilityId);
        if (!def)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[AbilitySystem] Unknown ability id %u", abilityId);
            return false;
        }

        // Already casting something — cannot double-cast
        if (m_activeCasts.contains(caster))
        {
            return false;
        }

        // Cooldown check
        auto& casterCooldowns = m_cooldowns[caster];
        auto cdIt = casterCooldowns.find(abilityId);
        if (cdIt != casterCooldowns.end() && cdIt->second > 0.0f)
        {
            return false;
        }

        // Target validation
        if (def->requiresTarget && def->targetType != AbilityTargetType::Self && target == 0)
        {
            return false;
        }

        // Self-target overrides
        uint32_t resolvedTarget = target;
        if (def->targetType == AbilityTargetType::Self)
        {
            resolvedTarget = caster;
        }

        // Build the cast instance
        AbilityCastInstance cast;
        cast.abilityId = abilityId;
        cast.casterId = caster;
        cast.targetId = resolvedTarget;
        cast.castProgress = 0.0f;
        cast.channelRemaining = def->channelDuration;

        // Instant cast — skip directly to launch
        if (def->castTime <= 0.0f)
        {
            cast.phase = CastPhase::Launching;
        }
        else
        {
            cast.phase = CastPhase::Casting;
        }

        m_activeCasts[caster] = cast;

        // Start cooldown
        casterCooldowns[abilityId] = def->cooldown;

        // Proc: OnAbilityCast
        ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnAbilityCast), caster, resolvedTarget,
                     AbilitySchool::Physical);

        return true;
    }

    void AbilitySystem::InterruptCast(uint32_t caster)
    {
        auto it = m_activeCasts.find(caster);
        if (it == m_activeCasts.end())
        {
            return;
        }

        CastPhase phase = it->second.phase;
        if (phase == CastPhase::Casting || phase == CastPhase::Channeling)
        {
            it->second.phase = CastPhase::Interrupted;
        }
    }

    bool AbilitySystem::IsCasting(uint32_t caster) const
    {
        auto it = m_activeCasts.find(caster);
        if (it == m_activeCasts.end())
        {
            return false;
        }
        CastPhase phase = it->second.phase;
        return phase == CastPhase::Casting || phase == CastPhase::Channeling || phase == CastPhase::Preparing;
    }

    // ============================================================================
    // Update
    // ============================================================================

    void AbilitySystem::Update(World& world, float deltaTime)
    {
        // Tick cooldowns
        for (auto& [entityId, cdMap] : m_cooldowns)
        {
            for (auto& [abilityId, remaining] : cdMap)
            {
                if (remaining > 0.0f)
                {
                    remaining -= deltaTime;
                }
            }
        }

        UpdateCasts(world, deltaTime);
        UpdateAuras(world, deltaTime);
    }

    void AbilitySystem::UpdateCasts(World& world, float dt)
    {
        // Collect entities to remove after iteration
        Spark::DeferredQueue<uint32_t> toRemove;

        for (auto& [entityId, cast] : m_activeCasts)
        {
            const AbilityDefinition* def = GetAbilityDef(cast.abilityId);
            if (!def)
            {
                cast.phase = CastPhase::Failed;
            }

            // Skip all phases that require a valid definition when def is null
            if (!def && cast.phase != CastPhase::Completed && cast.phase != CastPhase::Failed &&
                cast.phase != CastPhase::Interrupted)
            {
                cast.phase = CastPhase::Failed;
            }

            switch (cast.phase)
            {
            case CastPhase::Casting:
            {
                cast.castProgress += dt;
                if (cast.castProgress >= def->castTime)
                {
                    cast.phase = CastPhase::Launching;
                }
                break;
            }

            case CastPhase::Launching:
            {
                // Resolve all effects and transition
                ProcessEffects(world, *def, cast.casterId, cast.targetId);

                if (def->isChanneled && def->channelDuration > 0.0f)
                {
                    cast.phase = CastPhase::Channeling;
                    cast.channelRemaining = def->channelDuration;
                }
                else
                {
                    cast.phase = CastPhase::Completed;
                }
                break;
            }

            case CastPhase::Channeling:
            {
                cast.channelRemaining -= dt;
                if (cast.channelRemaining <= 0.0f)
                {
                    cast.phase = CastPhase::Completed;
                }
                break;
            }

            case CastPhase::Completed:
            case CastPhase::Failed:
            case CastPhase::Interrupted:
                toRemove.MarkForDeletion(entityId);
                break;

            case CastPhase::Preparing:
                // Transition to casting on next frame (allows one-frame setup)
                cast.phase = CastPhase::Casting;
                break;
            }
        }

        toRemove.Flush([&](uint32_t& id) { m_activeCasts.erase(id); });
    }

    void AbilitySystem::UpdateAuras(World& world, float dt)
    {
        for (auto& [entityId, auraList] : m_activeAuras)
        {
            for (auto& aura : auraList)
            {
                if (aura.markedForRemoval)
                {
                    continue;
                }

                const AuraDefinition* def = GetAuraDef(aura.definitionId);
                if (!def)
                {
                    aura.markedForRemoval = true;
                    continue;
                }

                // Tick proc internal cooldown
                if (aura.procCooldownTimer > 0.0f)
                {
                    aura.procCooldownTimer -= dt;
                }

                // Duration countdown (skip permanent auras)
                if (!def->isPermanent)
                {
                    aura.remainingDuration -= dt;
                    if (aura.remainingDuration <= 0.0f)
                    {
                        aura.markedForRemoval = true;
                        continue;
                    }
                }

                // Periodic tick (DoT / HoT)
                if (def->tickInterval > 0.0f && def->valuePerTick != 0.0f)
                {
                    aura.tickTimer += dt;
                    while (aura.tickTimer >= def->tickInterval)
                    {
                        aura.tickTimer -= def->tickInterval;
                        TickAura(world, entityId, aura, *def);
                    }
                }
            }

            // Erase-remove auras marked for removal
            std::erase_if(auraList, [](const ActiveAura& a) { return a.markedForRemoval; });
        }
    }

    // ============================================================================
    // Effect Resolution
    // ============================================================================

    void AbilitySystem::ProcessEffects(World& world, const AbilityDefinition& def, uint32_t caster, uint32_t target)
    {
        for (const auto& effect : def.effects)
        {
            ApplyEffect(world, effect, caster, target);
        }
    }

    void AbilitySystem::ApplyEffect(World& world, const AbilityEffect& effect, uint32_t caster, uint32_t target)
    {
        switch (effect.type)
        {
        case EffectType::Damage:
        {
            float amount = effect.baseValue * effect.scaling;
            if (amount <= 0.0f)
            {
                break;
            }

            HealthComponent* hp = world.GetComponent<HealthComponent>(static_cast<entt::entity>(target));
            if (!hp)
            {
                break;
            }

            bool wasDead = hp->isDead;
            hp->TakeDamage(amount);

            // Publish damage event
            if (m_eventBus)
            {
                Spark::EntityDamagedEvent dmgEvent;
                dmgEvent.entityId = target;
                dmgEvent.damage = amount;
                dmgEvent.damageSource = "ability";
                m_eventBus->Publish(dmgEvent);
            }

            // Proc: OnDealDamage for caster, OnTakeDamage for target
            ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnDealDamage), caster, target, effect.school);
            ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnTakeDamage), target, caster, effect.school);

            // Kill check
            if (!wasDead && hp->isDead)
            {
                if (m_eventBus)
                {
                    Spark::EntityKilledEvent killEvent;
                    killEvent.entityId = target;
                    killEvent.killerId = caster;
                    killEvent.cause = "ability";
                    m_eventBus->Publish(killEvent);
                }
                ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnKill), caster, target, effect.school);
                ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnDeath), target, caster, effect.school);
            }
            break;
        }

        case EffectType::Heal:
        {
            float amount = effect.baseValue * effect.scaling;
            if (amount <= 0.0f)
            {
                break;
            }

            HealthComponent* hp = world.GetComponent<HealthComponent>(static_cast<entt::entity>(target));
            if (!hp)
            {
                break;
            }

            hp->Heal(amount);

            ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnHeal), caster, target, effect.school);
            break;
        }

        case EffectType::ApplyAura:
        {
            if (effect.auraId != 0)
            {
                ApplyAura(world, target, effect.auraId, caster);
            }
            break;
        }

        case EffectType::RemoveAura:
        {
            if (effect.auraId != 0)
            {
                RemoveAura(target, effect.auraId);
            }
            break;
        }

        // These effect types are stubs for game-specific implementation
        case EffectType::Summon:
        case EffectType::Teleport:
        case EffectType::ModifyAttribute:
        case EffectType::ApplyForce:
        case EffectType::SpawnProjectile:
        case EffectType::Custom:
            break;
        }
    }

    // Aura management methods (ApplyAura, RemoveAura, TickAura, ProcessProcs)
    // are in AbilityAuras.cpp

} // namespace Spark::Gameplay
