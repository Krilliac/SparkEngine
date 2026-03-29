/**
 * @file ARPGCombatSystem.cpp
 * @brief Real-time ARPG combat resolution, resistance calculation, and DoT processing
 */

#include "ARPGCombatSystem.h"
#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <random>
#include <sstream>
#include "Utils/LogMacros.h"

namespace ARPG
{

    // Thread-local RNG for combat rolls
    static std::mt19937& GetCombatRNG()
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        return rng;
    }

    bool ARPGCombatSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        m_activeDots.clear();
        m_totalAttacksProcessed = 0;
        m_totalCrits = 0;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[ARPG] Combat system initialized");
        return true;
    }

    void ARPGCombatSystem::Shutdown()
    {
        m_activeDots.clear();
    }

    void ARPGCombatSystem::Update(float deltaTime)
    {
        ProcessDots(deltaTime);
    }

    void ARPGCombatSystem::FixedUpdate([[maybe_unused]] float fixedDeltaTime)
    {
        // Future: collision-based hit detection in fixed timestep
    }

    // === Damage calculation ===

    float ARPGCombatSystem::CalculateEffectiveDamage(float baseDamage, ARPGDamageType type,
                                                     const ResistanceProfile& resists) const
    {
        auto typeIndex = static_cast<size_t>(type);
        if (typeIndex >= resists.resistances.size())
            return baseDamage;

        // Resistance is a percentage reduction, capped at MAX_RESISTANCE
        float resistance = std::clamp(resists.resistances[typeIndex], 0.0f, MAX_RESISTANCE);
        return baseDamage * (1.0f - resistance);
    }

    DamageResult ARPGCombatSystem::PerformAttack(const DamageInstance& attack, const ResistanceProfile& targetResists)
    {
        m_totalAttacksProcessed++;

        DamageResult result;
        result.damageType = attack.damageType;

        float damage = attack.baseDamage;

        // Roll for critical hit
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        float critRoll = dist(GetCombatRNG());

        if (critRoll < attack.critChance)
        {
            damage *= attack.critMultiplier;
            result.wasCritical = true;
            m_totalCrits++;
        }

        // Apply resistance reduction
        float effectiveDamage = CalculateEffectiveDamage(damage, attack.damageType, targetResists);

        // Check if heavily resisted (more than 50% reduced)
        if (effectiveDamage < damage * 0.5f)
            result.wasResisted = true;

        result.finalDamage = std::max(effectiveDamage, 1.0f); // Minimum 1 damage
        return result;
    }

    // === Damage over time ===

    void ARPGCombatSystem::ApplyDot(const DotEffect& dot)
    {
        m_activeDots.push_back(dot);
    }

    void ARPGCombatSystem::ProcessDots(float deltaTime)
    {
        for (auto& dot : m_activeDots)
        {
            dot.remainingDuration -= deltaTime;
            dot.timeSinceLastTick += deltaTime;

            if (dot.timeSinceLastTick >= dot.tickInterval)
            {
                dot.timeSinceLastTick -= dot.tickInterval;
                // In a full implementation, this would look up the target entity
                // and apply dot.damagePerTick reduced by their resistances
            }
        }

        // Remove expired DoTs
        std::erase_if(m_activeDots, [](const DotEffect& dot) { return dot.remainingDuration <= 0.0f; });
    }

    // === Queries ===

    std::string ARPGCombatSystem::GetCombatStatusString() const
    {
        std::ostringstream ss;
        ss << "=== ARPG Combat Status ===\n";
        ss << "Attacks processed: " << m_totalAttacksProcessed << "\n";
        ss << "Critical hits: " << m_totalCrits << "\n";
        ss << "Active DoTs: " << m_activeDots.size() << "\n";
        ss << "Max resistance cap: " << static_cast<int>(MAX_RESISTANCE * 100.0f) << "%\n";
        return ss.str();
    }

    void ARPGCombatSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("ARPG Combat System"))
        {
            ImGui::Text("Attacks: %zu | Crits: %zu | Active DoTs: %zu", m_totalAttacksProcessed, m_totalCrits,
                        m_activeDots.size());

            if (!m_activeDots.empty() && ImGui::TreeNode("Active DoTs"))
            {
                for (size_t i = 0; i < m_activeDots.size(); ++i)
                {
                    const auto& dot = m_activeDots[i];
                    ImGui::Text("[%zu] Target:%u Dmg:%.1f/tick Remaining:%.1fs", i, dot.targetId, dot.damagePerTick,
                                dot.remainingDuration);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace ARPG
