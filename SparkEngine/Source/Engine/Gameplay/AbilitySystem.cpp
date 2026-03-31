/**
 * @file AbilitySystem.cpp
 * @brief Implementation of the pipeline-based ability/spell system
 */

#include "AbilitySystem.h"
#include "../../Utils/DeferredDeletion.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/SparkConsole.h"
#include "../ECS/Components/GameplayComponents.h"
#include "../Events/EventSystem.h"

#include "../../Utils/MathUtils.h"

#include <algorithm>
#include <cmath>
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
        SPARK_LOG_INFO(Spark::LogCategory::Game, "AbilitySystem initialized");
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
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Registered ability id=%u", def.id);
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
            SPARK_LOG_WARN(Spark::LogCategory::Game, "CastAbility failed: unknown ability id %u", abilityId);
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
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Ability %u cast by entity %u on target %u (phase=%s)", abilityId,
                       caster, resolvedTarget, (def->castTime <= 0.0f) ? "instant" : "casting");

        // Start cooldown
        casterCooldowns[abilityId] = def->cooldown;

        // Proc: OnAbilityCast
        ProcessProcs(world, static_cast<uint32_t>(ProcTrigger::OnAbilityCast), caster, resolvedTarget,
                     AbilitySchool::Physical);
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Ability %u cooldown started: %.1fs", abilityId, def->cooldown);

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
            SPARK_LOG_INFO(Spark::LogCategory::Game, "Cast interrupted for entity %u (ability %u)", caster,
                           it->second.abilityId);
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
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "Cast failed: missing ability definition for id %u (entity %u)", cast.abilityId,
                                entityId);
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

        case EffectType::Summon:
        {
            // Spawn a new entity near the caster using customParam as template ID.
            // The spawned entity gets a HealthComponent initialized from baseValue.
            Transform* casterTf = world.GetComponent<Transform>(static_cast<entt::entity>(caster));
            if (!casterTf)
            {
                break;
            }

            auto& registry = world.GetRegistry();
            auto summon = registry.create();

            // Place the summon slightly in front of the caster
            Transform summonTf;
            float yawRad = casterTf->rotation.y * MathUtils::DEG_TO_RAD;
            summonTf.position.x = casterTf->position.x + std::sin(yawRad) * 2.0f;
            summonTf.position.y = casterTf->position.y;
            summonTf.position.z = casterTf->position.z + std::cos(yawRad) * 2.0f;
            registry.emplace<Transform>(summon, summonTf);

            HealthComponent hp;
            hp.maxHealth = (effect.baseValue > 0.0f) ? effect.baseValue : 100.0f;
            hp.health = hp.maxHealth;
            registry.emplace<HealthComponent>(summon, hp);

            SPARK_LOG_INFO(Spark::LogCategory::Game, "AbilitySystem: Summoned entity %u (template %u) for caster %u",
                           static_cast<uint32_t>(summon), effect.customParam, caster);
            break;
        }

        case EffectType::Teleport:
        {
            // Teleport the target to a position encoded in the effect.
            // baseValue = distance forward (along facing direction).
            // If baseValue is 0, teleport to the caster's position.
            Transform* targetTf = world.GetComponent<Transform>(static_cast<entt::entity>(target));
            if (!targetTf)
            {
                break;
            }

            if (effect.baseValue > 0.0f && target != caster)
            {
                // Teleport forward along the target's facing direction
                float yawRad = targetTf->rotation.y * MathUtils::DEG_TO_RAD;
                targetTf->position.x += std::sin(yawRad) * effect.baseValue;
                targetTf->position.z += std::cos(yawRad) * effect.baseValue;
            }
            else if (target != caster)
            {
                // Teleport target to caster's position
                Transform* casterTf = world.GetComponent<Transform>(static_cast<entt::entity>(caster));
                if (casterTf)
                {
                    targetTf->position = casterTf->position;
                }
            }

            SPARK_LOG_INFO(Spark::LogCategory::Game, "AbilitySystem: Teleported entity %u", target);
            break;
        }

        case EffectType::ModifyAttribute:
        {
            // Modify the target's max health as a generic attribute modification.
            // baseValue = flat change, scaling = percentage multiplier.
            HealthComponent* hp = world.GetComponent<HealthComponent>(static_cast<entt::entity>(target));
            if (!hp)
            {
                break;
            }

            float flatChange = effect.baseValue;
            float pctMultiplier = effect.scaling;

            hp->maxHealth = hp->maxHealth * pctMultiplier + flatChange;
            if (hp->maxHealth < 1.0f)
            {
                hp->maxHealth = 1.0f;
            }
            // Clamp current health to new max
            if (hp->health > hp->maxHealth)
            {
                hp->health = hp->maxHealth;
            }

            SPARK_LOG_DEBUG(Spark::LogCategory::Game, "AbilitySystem: Modified attribute on entity %u (maxHealth=%.1f)",
                            target, hp->maxHealth);
            break;
        }

        case EffectType::ApplyForce:
        {
            // Apply a physics impulse to the target entity.
            // baseValue = force magnitude, customParam encodes direction (0=away from caster, 1=up, 2=toward caster).
            Transform* targetTf = world.GetComponent<Transform>(static_cast<entt::entity>(target));
            Transform* casterTf = world.GetComponent<Transform>(static_cast<entt::entity>(caster));
            RigidBodyComponent* rb = world.GetComponent<RigidBodyComponent>(static_cast<entt::entity>(target));
            if (!targetTf || !rb)
            {
                break;
            }

            float magnitude = effect.baseValue * effect.scaling;
            DirectX::XMFLOAT3 forceDir{0.0f, 0.0f, 0.0f};

            if (effect.customParam == 1)
            {
                // Upward force (knockup)
                forceDir.y = magnitude;
            }
            else if (effect.customParam == 2 && casterTf)
            {
                // Pull toward caster
                float dx = casterTf->position.x - targetTf->position.x;
                float dz = casterTf->position.z - targetTf->position.z;
                float dist = std::sqrt(dx * dx + dz * dz);
                if (dist > 0.001f)
                {
                    forceDir.x = (dx / dist) * magnitude;
                    forceDir.z = (dz / dist) * magnitude;
                }
            }
            else if (casterTf)
            {
                // Knockback away from caster (default)
                float dx = targetTf->position.x - casterTf->position.x;
                float dz = targetTf->position.z - casterTf->position.z;
                float dist = std::sqrt(dx * dx + dz * dz);
                if (dist > 0.001f)
                {
                    forceDir.x = (dx / dist) * magnitude;
                    forceDir.z = (dz / dist) * magnitude;
                }
                forceDir.y = magnitude * 0.3f; // slight upward component
            }

            // Write impulse into the RigidBodyComponent's velocity for physics to pick up
            rb->linearVelocity.x += forceDir.x;
            rb->linearVelocity.y += forceDir.y;
            rb->linearVelocity.z += forceDir.z;

            SPARK_LOG_DEBUG(Spark::LogCategory::Game, "AbilitySystem: Applied force (%.1f, %.1f, %.1f) to entity %u",
                            forceDir.x, forceDir.y, forceDir.z, target);
            break;
        }

        case EffectType::SpawnProjectile:
        {
            // Spawn a projectile entity from the caster toward the target.
            // baseValue = damage, customParam = projectile template ID.
            Transform* casterTf = world.GetComponent<Transform>(static_cast<entt::entity>(caster));
            if (!casterTf)
            {
                break;
            }

            auto& registry = world.GetRegistry();
            auto projectile = registry.create();

            // Spawn at caster position, aimed toward target
            Transform projTf;
            projTf.position = casterTf->position;
            projTf.position.y += 1.0f; // offset to roughly chest height
            projTf.rotation = casterTf->rotation;

            // If there's a target, face it
            Transform* targetTf = world.GetComponent<Transform>(static_cast<entt::entity>(target));
            if (targetTf)
            {
                float dx = targetTf->position.x - casterTf->position.x;
                float dz = targetTf->position.z - casterTf->position.z;
                if (std::abs(dx) > 0.01f || std::abs(dz) > 0.01f)
                {
                    projTf.rotation.y = std::atan2(dx, dz) * MathUtils::RAD_TO_DEG;
                }
            }

            registry.emplace<Transform>(projectile, projTf);

            ProjectileComponent projComp;
            projComp.damage = effect.baseValue * effect.scaling;
            projComp.speed = 30.0f;
            projComp.ownerEntityId = caster;
            projComp.maxLifetime = 5.0f;

            // Set direction toward target if available
            if (targetTf)
            {
                float dx = targetTf->position.x - casterTf->position.x;
                float dy = targetTf->position.y - casterTf->position.y;
                float dz = targetTf->position.z - casterTf->position.z;
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dist > 0.001f)
                {
                    projComp.direction = {dx / dist, dy / dist, dz / dist};
                }
            }
            else
            {
                // Fire forward along caster's facing
                float yawRad2 = casterTf->rotation.y * MathUtils::DEG_TO_RAD;
                projComp.direction = {std::sin(yawRad2), 0.0f, std::cos(yawRad2)};
            }

            registry.emplace<ProjectileComponent>(projectile, projComp);

            SPARK_LOG_INFO(Spark::LogCategory::Game,
                           "AbilitySystem: Spawned projectile %u (template %u, damage %.1f) from caster %u",
                           static_cast<uint32_t>(projectile), effect.customParam, projComp.damage, caster);
            break;
        }

        case EffectType::Custom:
            // Custom effects are left as extension points for game modules.
            // Game code can subscribe to events or override the ability pipeline.
            SPARK_LOG_DEBUG(Spark::LogCategory::Game,
                            "AbilitySystem: Custom effect (param=%u) on entity %u from caster %u", effect.customParam,
                            target, caster);
            break;
        }
    }

    // Aura management methods (ApplyAura, RemoveAura, TickAura, ProcessProcs)
    // are in AbilityAuras.cpp

} // namespace Spark::Gameplay
