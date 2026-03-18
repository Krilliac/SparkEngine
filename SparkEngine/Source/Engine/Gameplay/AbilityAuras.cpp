/**
 * @file AbilityAuras.cpp
 * @brief Aura management, tick processing, and proc system for AbilitySystem
 *
 * Split from AbilitySystem.cpp. Contains: ApplyAura, RemoveAura, GetActiveAuras,
 * HasAura, GetAuraStacks, TickAura (DoT/HoT), and ProcessProcs.
 */

#include "AbilitySystem.h"
#include "../../Utils/SparkConsole.h"
#include "../ECS/Components.h"
#include "../ECS/Components/GameplayComponents.h"
#include "../Events/EventSystem.h"

#include <algorithm>
#include <random>

namespace Spark::Gameplay
{

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
