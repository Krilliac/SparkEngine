/**
 * @file RPGCombatSystem.cpp
 * @brief Action combat, cooldowns, combos, damage formulas, and knockback
 */

#include "RPGCombatSystem.h"
#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <random>
#include "Utils/LogMacros.h"

namespace RPG
{

    // Thread-local RNG for crit calculations
    static std::mt19937& GetRNG()
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        return rng;
    }

    bool RPGCombatSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[RPG] Combat system initialized");
        return true;
    }

    void RPGCombatSystem::Update(float deltaTime)
    {
        UpdateCooldowns(deltaTime);
        UpdateCombos(deltaTime);
        UpdateEncounters(deltaTime);
    }

    void RPGCombatSystem::FixedUpdate(float fixedDeltaTime)
    {
        // Physics-rate knockback application would go here when integrated
        // with the engine's PhysicsSystem via m_context->GetPhysics()
        (void)fixedDeltaTime;
    }

    void RPGCombatSystem::Shutdown()
    {
        m_cooldowns.clear();
        m_combos.clear();
        m_encounters.clear();
    }

    // === Combat flow ===

    uint32_t RPGCombatSystem::StartEncounter(uint32_t attackerId, uint32_t defenderId)
    {
        CombatEncounter encounter;
        encounter.encounterId = m_nextEncounterId++;
        encounter.attackerId = attackerId;
        encounter.defenderId = defenderId;
        encounter.isActive = true;
        encounter.elapsedTime = 0.0f;

        uint32_t id = encounter.encounterId;
        m_encounters[id] = encounter;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[RPG] Combat encounter %s started", std::to_string(id).c_str());
        return id;
    }

    void RPGCombatSystem::EndEncounter(uint32_t encounterId)
    {
        auto it = m_encounters.find(encounterId);
        if (it != m_encounters.end())
        {
            it->second.isActive = false;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[RPG] Combat encounter %s ended",
                           std::to_string(encounterId).c_str());
        }
    }

    size_t RPGCombatSystem::GetActiveCombatCount() const
    {
        size_t count = 0;
        for (const auto& [id, enc] : m_encounters)
        {
            if (enc.isActive)
                count++;
        }
        return count;
    }

    // === Damage calculation ===

    DamageResult RPGCombatSystem::CalculateDamage(float baseDamage, DamageType type, float attackStat,
                                                  float defenseStat, const ResistanceProfile& resistances,
                                                  const ComboState& combo) const
    {
        DamageResult result;
        result.type = type;
        result.comboMultiplier = combo.damageMultiplier;

        // Raw damage: base + stat scaling
        // Attack stat adds 2% per point, defense reduces by 1% per point
        float attackMod = 1.0f + (attackStat * 0.02f);
        float defenseMod = std::max(0.1f, 1.0f - (defenseStat * 0.01f));
        result.rawDamage = baseDamage * attackMod;

        // Crit check
        std::uniform_real_distribution<float> critDist(0.0f, 1.0f);
        if (critDist(GetRNG()) < BASE_CRIT_CHANCE)
        {
            result.rawDamage *= CRIT_MULTIPLIER;
            result.isCritical = true;
        }

        // Apply combo multiplier
        result.rawDamage *= combo.damageMultiplier;

        // Apply resistance
        auto typeIndex = static_cast<size_t>(type);
        if (typeIndex < resistances.values.size())
        {
            result.resistanceReduction = resistances.values[typeIndex];
        }
        float resistMod = std::max(0.0f, 1.0f - result.resistanceReduction);

        // Final mitigated damage
        result.mitigatedDamage = result.rawDamage * defenseMod * resistMod;
        result.mitigatedDamage = std::max(1.0f, result.mitigatedDamage); // Minimum 1 damage

        // Knockback proportional to damage
        result.knockbackForce = CalculateKnockback(result.mitigatedDamage, type);

        return result;
    }

    // === Ability cooldowns ===

    bool RPGCombatSystem::UseAbility(uint32_t characterId, uint32_t abilityId, float cooldown)
    {
        if (!IsAbilityReady(characterId, abilityId))
            return false;

        auto& cooldowns = m_cooldowns[characterId];

        // Remove expired entry if present, then add fresh cooldown
        cooldowns.erase(std::remove_if(cooldowns.begin(), cooldowns.end(),
                                       [abilityId](const AbilityCooldown& cd) { return cd.abilityId == abilityId; }),
                        cooldowns.end());

        cooldowns.push_back({abilityId, cooldown, cooldown});
        return true;
    }

    bool RPGCombatSystem::IsAbilityReady(uint32_t characterId, uint32_t abilityId) const
    {
        auto it = m_cooldowns.find(characterId);
        if (it == m_cooldowns.end())
            return true;

        for (const auto& cd : it->second)
        {
            if (cd.abilityId == abilityId && cd.remaining > 0.0f)
                return false;
        }
        return true;
    }

    // === Combo system ===

    void RPGCombatSystem::RegisterHit(uint32_t characterId)
    {
        auto& combo = m_combos[characterId];

        if (combo.hitCount < ComboState::MAX_COMBO)
        {
            combo.hitCount++;
            combo.damageMultiplier = 1.0f + combo.hitCount * ComboState::MULTIPLIER_PER_HIT;
        }

        // Reset the combo window timer
        combo.comboTimer = ComboState::COMBO_WINDOW;
    }

    void RPGCombatSystem::ResetCombo(uint32_t characterId)
    {
        m_combos[characterId] = {};
    }

    const ComboState* RPGCombatSystem::GetComboState(uint32_t characterId) const
    {
        auto it = m_combos.find(characterId);
        return it != m_combos.end() ? &it->second : nullptr;
    }

    // === Knockback ===

    float RPGCombatSystem::CalculateKnockback(float damage, DamageType type) const
    {
        float base = damage * KNOCKBACK_SCALE;

        // Elemental types have different knockback profiles
        switch (type)
        {
        case DamageType::Physical:
            return base * 1.0f;
        case DamageType::Fire:
            return base * 0.8f;
        case DamageType::Ice:
            return base * 0.5f; // Ice slows rather than pushes
        case DamageType::Lightning:
            return base * 1.2f; // Lightning has strong knockback
        case DamageType::Holy:
            return base * 0.6f;
        case DamageType::Shadow:
            return base * 0.4f;
        default:
            return base;
        }
    }

    // === Internal updates ===

    void RPGCombatSystem::UpdateCooldowns(float deltaTime)
    {
        for (auto& [charId, cooldowns] : m_cooldowns)
        {
            for (auto& cd : cooldowns)
            {
                if (cd.remaining > 0.0f)
                    cd.remaining = std::max(0.0f, cd.remaining - deltaTime);
            }

            // Remove fully expired cooldowns
            cooldowns.erase(std::remove_if(cooldowns.begin(), cooldowns.end(),
                                           [](const AbilityCooldown& cd) { return cd.remaining <= 0.0f; }),
                            cooldowns.end());
        }
    }

    void RPGCombatSystem::UpdateCombos(float deltaTime)
    {
        for (auto it = m_combos.begin(); it != m_combos.end();)
        {
            auto& combo = it->second;
            if (combo.hitCount > 0)
            {
                combo.comboTimer -= deltaTime;
                if (combo.comboTimer <= 0.0f)
                {
                    // Combo expired
                    it = m_combos.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    void RPGCombatSystem::UpdateEncounters(float deltaTime)
    {
        for (auto it = m_encounters.begin(); it != m_encounters.end();)
        {
            if (!it->second.isActive)
            {
                it = m_encounters.erase(it);
            }
            else
            {
                it->second.elapsedTime += deltaTime;
                ++it;
            }
        }
    }

    void RPGCombatSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RPG Combat System"))
        {
            ImGui::Text("Active encounters: %zu", GetActiveCombatCount());
            ImGui::Text("Active combos: %zu", m_combos.size());
            ImGui::Text("Cooldown sets: %zu", m_cooldowns.size());

            if (ImGui::TreeNode("Encounters"))
            {
                for (const auto& [id, enc] : m_encounters)
                {
                    ImGui::Text("[%u] Attacker:%u vs Defender:%u (%.1fs)", id, enc.attackerId, enc.defenderId,
                                enc.elapsedTime);
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Combos"))
            {
                for (const auto& [charId, combo] : m_combos)
                {
                    ImGui::Text("Char %u: %d hits (x%.2f) timer:%.1fs", charId, combo.hitCount, combo.damageMultiplier,
                                combo.comboTimer);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RPG
