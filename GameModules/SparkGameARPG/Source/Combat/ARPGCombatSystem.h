/**
 * @file ARPGCombatSystem.h
 * @brief Real-time ARPG combat with damage types, resistances, and AoE
 * @author Spark Engine Team
 * @date 2026
 *
 * Handles hit resolution, damage calculation with resistance reduction,
 * critical strikes, area-of-effect attacks, and damage-over-time effects.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/ARPGEnums.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ARPG
{

    /// @brief A single instance of damage to be resolved
    struct DamageInstance
    {
        ARPGDamageType damageType = ARPGDamageType::Physical;
        float baseDamage = 0.0f;
        float critChance = 0.05f;    ///< 0.0 - 1.0
        float critMultiplier = 1.5f; ///< Multiplier on critical hit
        bool isAoE = false;
        float aoeRadius = 0.0f; ///< Radius of area effect
        uint32_t sourceId = 0;  ///< Entity that dealt the damage
        uint32_t targetId = 0;  ///< Entity receiving the damage
    };

    /// @brief Damage-over-time effect ticking on a target
    struct DotEffect
    {
        uint32_t targetId = 0;
        ARPGDamageType damageType = ARPGDamageType::Poison;
        float damagePerTick = 0.0f;
        float tickInterval = 1.0f; ///< Seconds between ticks
        float remainingDuration = 0.0f;
        float timeSinceLastTick = 0.0f;
    };

    /// @brief Per-entity resistance profile (percentage reduction per damage type)
    struct ResistanceProfile
    {
        std::array<float, static_cast<size_t>(ARPGDamageType::Count)> resistances{};
    };

    /// @brief Result of a damage calculation
    struct DamageResult
    {
        float finalDamage = 0.0f;
        bool wasCritical = false;
        bool wasResisted = false;
        ARPGDamageType damageType = ARPGDamageType::Physical;
    };

    /**
     * @brief Real-time combat system with damage types, crits, AoE, and DoTs
     */
    class ARPGCombatSystem
    {
      public:
        ARPGCombatSystem() = default;
        ~ARPGCombatSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Shutdown();
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void RenderDebugUI();

        // === Combat actions ===
        DamageResult PerformAttack(const DamageInstance& attack, const ResistanceProfile& targetResists);
        float CalculateEffectiveDamage(float baseDamage, ARPGDamageType type, const ResistanceProfile& resists) const;
        void ApplyDot(const DotEffect& dot);

        // === Queries ===
        size_t GetActiveDotCount() const { return m_activeDots.size(); }
        size_t GetAttacksProcessed() const { return m_totalAttacksProcessed; }
        std::string GetCombatStatusString() const;

      private:
        void ProcessDots(float deltaTime);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<DotEffect> m_activeDots;
        size_t m_totalAttacksProcessed = 0;
        size_t m_totalCrits = 0;

        static constexpr float MAX_RESISTANCE = 0.75f; ///< Cap at 75% reduction
    };

} // namespace ARPG
