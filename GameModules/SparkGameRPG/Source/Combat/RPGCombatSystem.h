/**
 * @file RPGCombatSystem.h
 * @brief Action-based combat with cooldowns, combos, and elemental damage
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides action-based (not pure turn-based) combat with ability cooldowns,
 * damage calculation using stats/equipment/damage types, elemental resistances,
 * combo chains that boost damage, and physics integration for knockback.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RPGEnums.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RPG
{

    /// @brief Tracks cooldown state for an ability
    struct AbilityCooldown
    {
        uint32_t abilityId = 0;
        float remaining = 0.0f;
        float total = 0.0f;
    };

    /// @brief Elemental resistance profile (0.0 = no resist, 1.0 = immune)
    struct ResistanceProfile
    {
        std::array<float, static_cast<size_t>(DamageType::Count)> values{};
    };

    /// @brief Tracks an active combo chain
    struct ComboState
    {
        int hitCount = 0;
        float comboTimer = 0.0f;       ///< Time remaining to continue the combo
        float damageMultiplier = 1.0f; ///< Current combo damage bonus

        static constexpr float COMBO_WINDOW = 2.0f;        ///< Seconds to land next hit
        static constexpr float MULTIPLIER_PER_HIT = 0.15f; ///< +15% per combo hit
        static constexpr int MAX_COMBO = 8;                ///< Max hits in a chain
    };

    /// @brief Result of a single damage calculation
    struct DamageResult
    {
        float rawDamage = 0.0f;
        float mitigatedDamage = 0.0f;
        float resistanceReduction = 0.0f;
        float comboMultiplier = 1.0f;
        DamageType type = DamageType::Physical;
        bool isCritical = false;
        float knockbackForce = 0.0f;
    };

    /// @brief Represents an active combat encounter
    struct CombatEncounter
    {
        uint32_t encounterId = 0;
        uint32_t attackerId = 0;
        uint32_t defenderId = 0;
        bool isActive = true;
        float elapsedTime = 0.0f;
    };

    /**
     * @brief Action-based combat with cooldowns, combos, and physics knockback
     */
    class RPGCombatSystem
    {
      public:
        RPGCombatSystem() = default;
        ~RPGCombatSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // === Combat flow ===
        uint32_t StartEncounter(uint32_t attackerId, uint32_t defenderId);
        void EndEncounter(uint32_t encounterId);
        size_t GetActiveCombatCount() const;

        // === Damage calculation ===
        DamageResult CalculateDamage(float baseDamage, DamageType type, float attackStat, float defenseStat,
                                     const ResistanceProfile& resistances, const ComboState& combo) const;

        // === Ability cooldowns ===
        bool UseAbility(uint32_t characterId, uint32_t abilityId, float cooldown);
        bool IsAbilityReady(uint32_t characterId, uint32_t abilityId) const;

        // === Combo system ===
        void RegisterHit(uint32_t characterId);
        void ResetCombo(uint32_t characterId);
        const ComboState* GetComboState(uint32_t characterId) const;

        /// @brief Remove all transient combat state owned by a character being destroyed.
        void ClearCharacterState(uint32_t characterId);

        // === Knockback (physics integration) ===
        float CalculateKnockback(float damage, DamageType type) const;

      private:
        void UpdateCooldowns(float deltaTime);
        void UpdateCombos(float deltaTime);
        void UpdateEncounters(float deltaTime);

        Spark::IEngineContext* m_context{nullptr};

        // characterId -> list of active cooldowns
        std::unordered_map<uint32_t, std::vector<AbilityCooldown>> m_cooldowns;

        // characterId -> combo state
        std::unordered_map<uint32_t, ComboState> m_combos;

        // Active encounters
        std::unordered_map<uint32_t, CombatEncounter> m_encounters;
        uint32_t m_nextEncounterId = 1;

        static constexpr float CRIT_MULTIPLIER = 1.5f;
        static constexpr float BASE_CRIT_CHANCE = 0.05f;
        static constexpr float KNOCKBACK_SCALE = 0.1f;
    };

} // namespace RPG
