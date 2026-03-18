/**
 * @file AbilitySystem.cpp
 * @brief Implementation of the pipeline-based ability/spell system
 */

#include "AbilitySystem.h"
#include "../../Utils/SparkConsole.h"
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

        Spark::SimpleConsole::GetInstance().LogInfo("[AbilitySystem] Initialized");
    }

    void AbilitySystem::Shutdown()
    {
        m_activeCasts.clear();
        m_activeAuras.clear();
        m_cooldowns.clear();
        m_eventBus = nullptr;

        Spark::SimpleConsole::GetInstance().LogInfo("[AbilitySystem] Shut down");
    }

    // ============================================================================
    // Registration
    // ============================================================================

    void AbilitySystem::RegisterAbility(const AbilityDefinition& def)
    {
        if (def.id == 0)
        {
            Spark::SimpleConsole::GetInstance().LogWarning("[AbilitySystem] Cannot register ability with id 0");
            return;
        }
        m_abilities[def.id] = def;
    }

    void AbilitySystem::RegisterAura(const AuraDefinition& def)
    {
        if (def.id == 0)
        {
            Spark::SimpleConsole::GetInstance().LogWarning("[AbilitySystem] Cannot register aura with id 0");
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
            Spark::SimpleConsole::GetInstance().LogWarning("[AbilitySystem] Unknown ability id " +
                                                           std::to_string(abilityId));
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
        std::vector<uint32_t> toRemove;

        for (auto& [entityId, cast] : m_activeCasts)
        {
            const AbilityDefinition* def = GetAbilityDef(cast.abilityId);
            if (!def)
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
                toRemove.push_back(entityId);
                break;

            case CastPhase::Preparing:
                // Transition to casting on next frame (allows one-frame setup)
                cast.phase = CastPhase::Casting;
                break;
            }
        }

        for (uint32_t id : toRemove)
        {
            m_activeCasts.erase(id);
        }
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

    // ============================================================================
    // Aura Management
    // ============================================================================

    void AbilitySystem::ApplyAura(World& world, uint32_t target, AuraID auraId, uint32_t caster)
    {
        const AuraDefinition* def = GetAuraDef(auraId);
        if (!def)
        {
            Spark::SimpleConsole::GetInstance().LogWarning("[AbilitySystem] Unknown aura id " + std::to_string(auraId));
            return;
        }

        auto& auraList = m_activeAuras[target];

        // Check for existing aura of the same type
        auto existing = std::find_if(auraList.begin(), auraList.end(),
                                     [auraId](const ActiveAura& a) { return a.definitionId == auraId; });

        if (existing != auraList.end())
        {
            switch (def->stackType)
            {
            case AuraStackType::None:
                // Refresh duration
                existing->remainingDuration = def->duration;
                existing->casterId = caster;
                return;

            case AuraStackType::Stacking:
                // Add a stack (up to max), refresh duration
                if (existing->currentStacks < def->maxStacks)
                {
                    existing->currentStacks++;
                }
                existing->remainingDuration = def->duration;
                existing->casterId = caster;
                return;

            case AuraStackType::Separate:
                // Fall through to create a new independent instance
                break;
            }
        }

        // Create new aura instance
        ActiveAura aura;
        aura.definitionId = auraId;
        aura.casterId = caster;
        aura.remainingDuration = def->duration;
        aura.tickTimer = 0.0f;
        aura.currentStacks = 1;
        aura.markedForRemoval = false;

        // Check if this aura has proc definitions and set up charges
        for (const auto& proc : m_procs)
        {
            if (proc.sourceAuraId == auraId)
            {
                aura.procChargesRemaining = proc.charges;
                break;
            }
        }

        auraList.push_back(aura);

        // Suppress unused parameter warning — world may be used by future aura-apply hooks
        (void)world;
    }

    void AbilitySystem::RemoveAura(uint32_t target, AuraID auraId)
    {
        auto it = m_activeAuras.find(target);
        if (it == m_activeAuras.end())
        {
            return;
        }

        auto& auraList = it->second;
        std::erase_if(auraList, [auraId](const ActiveAura& a) { return a.definitionId == auraId; });
    }

    void AbilitySystem::RemoveAllAuras(uint32_t target)
    {
        m_activeAuras.erase(target);
    }

    std::vector<ActiveAura> AbilitySystem::GetActiveAuras(uint32_t target) const
    {
        auto it = m_activeAuras.find(target);
        if (it != m_activeAuras.end())
        {
            return it->second;
        }
        return {};
    }

    bool AbilitySystem::HasAura(uint32_t target, AuraID auraId) const
    {
        auto it = m_activeAuras.find(target);
        if (it == m_activeAuras.end())
        {
            return false;
        }

        const auto& auraList = it->second;
        return std::any_of(auraList.begin(), auraList.end(),
                           [auraId](const ActiveAura& a) { return a.definitionId == auraId; });
    }

    int AbilitySystem::GetAuraStacks(uint32_t target, AuraID auraId) const
    {
        auto it = m_activeAuras.find(target);
        if (it == m_activeAuras.end())
        {
            return 0;
        }

        const auto& auraList = it->second;
        for (const auto& aura : auraList)
        {
            if (aura.definitionId == auraId)
            {
                return aura.currentStacks;
            }
        }
        return 0;
    }

    // ============================================================================
    // Aura Tick (DoT / HoT)
    // ============================================================================

    void AbilitySystem::TickAura(World& world, uint32_t target, ActiveAura& aura, const AuraDefinition& def)
    {
        float tickValue = def.valuePerTick * aura.currentStacks;

        if (def.type == AuraType::DamageOverTime)
        {
            HealthComponent* hp = world.GetComponent<HealthComponent>(static_cast<entt::entity>(target));
            if (hp && !hp->isDead)
            {
                bool wasDead = hp->isDead;
                hp->TakeDamage(tickValue);

                if (m_eventBus)
                {
                    Spark::EntityDamagedEvent dmgEvent;
                    dmgEvent.entityId = target;
                    dmgEvent.damage = tickValue;
                    dmgEvent.damageSource = "aura_dot";
                    m_eventBus->Publish(dmgEvent);
                }

                ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnDealDamage), aura.casterId, target,
                             def.school);
                ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnTakeDamage), target, aura.casterId,
                             def.school);

                if (!wasDead && hp->isDead)
                {
                    if (m_eventBus)
                    {
                        Spark::EntityKilledEvent killEvent;
                        killEvent.entityId = target;
                        killEvent.killerId = aura.casterId;
                        killEvent.cause = "aura_dot";
                        m_eventBus->Publish(killEvent);
                    }
                    ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnKill), aura.casterId, target, def.school);
                }
            }
        }
        else if (def.type == AuraType::HealOverTime)
        {
            HealthComponent* hp = world.GetComponent<HealthComponent>(static_cast<entt::entity>(target));
            if (hp && !hp->isDead)
            {
                hp->Heal(tickValue);
                ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnHeal), aura.casterId, target, def.school);
            }
        }
    }

    // ============================================================================
    // Proc Processing
    // ============================================================================

    void AbilitySystem::ProcessProcs(World& world, uint32_t triggerMask, uint32_t source, uint32_t target,
                                     AbilitySchool school)
    {
        auto auraIt = m_activeAuras.find(source);
        if (auraIt == m_activeAuras.end())
        {
            return;
        }

        // Thread-local RNG for proc chance rolls
        static thread_local std::mt19937 rng{std::random_device{}()};
        static thread_local std::uniform_real_distribution<float> dist(0.0f, 100.0f);

        for (auto& aura : auraIt->second)
        {
            if (aura.markedForRemoval)
            {
                continue;
            }

            // Find matching proc definitions for this aura
            for (const auto& proc : m_procs)
            {
                if (proc.sourceAuraId != aura.definitionId)
                {
                    continue;
                }
                if ((proc.triggerMask & triggerMask) == 0)
                {
                    continue;
                }

                // School mask filter
                uint32_t schoolBit = 1u << static_cast<uint32_t>(school);
                if ((proc.schoolMask & schoolBit) == 0)
                {
                    continue;
                }

                // Internal cooldown check
                if (aura.procCooldownTimer > 0.0f)
                {
                    continue;
                }

                // Chance roll
                if (proc.chance < 100.0f && dist(rng) > proc.chance)
                {
                    continue;
                }

                // Fire the proc — cast the triggered ability
                CastAbility(world, source, proc.triggeredAbilityId, target);

                // Apply internal cooldown
                if (proc.cooldown > 0.0f)
                {
                    aura.procCooldownTimer = proc.cooldown;
                }

                // Consume charges
                if (proc.charges > 0)
                {
                    aura.procChargesRemaining--;
                    if (aura.procChargesRemaining <= 0)
                    {
                        aura.markedForRemoval = true;
                    }
                }
            }
        }
    }

} // namespace Spark::Gameplay
