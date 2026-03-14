/**
 * @file AIDirector.h
 * @brief Dynamic difficulty and pacing system (inspired by Left 4 Dead's AI Director)
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * The AI Director monitors player performance metrics (kills, deaths, health,
 * ammo, accuracy, pacing) and dynamically adjusts game parameters to maintain
 * optimal tension. It controls enemy spawn rates, difficulty multipliers,
 * resource availability, and event triggers.
 *
 * ## Architecture
 * ```
 * AIDirector
 *   ├── m_playerMetrics      (rolling window of player performance data)
 *   ├── m_intensityCurve     (current tension level 0.0 - 1.0)
 *   ├── m_difficultyParams   (adjustable game parameters)
 *   └── Update(deltaTime)
 *         ├── SamplePlayerMetrics()  → reads kills, health, ammo from ECS
 *         ├── UpdateIntensity()      → adjusts intensity based on metrics
 *         └── ApplyDifficulty()      → modifies spawn rates, AI stats, resources
 * ```
 *
 * ## Usage
 * @code
 *   AIDirector director;
 *   director.SetDesiredIntensityRange(0.3f, 0.7f);
 *   director.SetBuildUpDuration(60.0f);  // 60s build-up phase
 *   director.SetPeakDuration(20.0f);     // 20s peak phase
 *   director.SetRelaxDuration(30.0f);    // 30s relax phase
 *
 *   // Per frame:
 *   director.Update(world, deltaTime);
 *   float spawnRate = director.GetCurrentSpawnRate();
 *   float dmgMult   = director.GetDamageMultiplier();
 * @endcode
 */

#pragma once

#include "../ECS/Systems/ECSystems.h"
#include "../../Utils/StateMachine.h"

#include <string>
#include <vector>
#include <cstdint>

namespace Spark::AI
{

    // =============================================================================
    // DirectorPhase
    // =============================================================================

    /**
 * @brief Phases of the AI Director's intensity cycle.
 */
    enum class DirectorPhase
    {
        BuildUp,   ///< Gradually increasing intensity (spawning more enemies)
        Peak,      ///< Maximum intensity (boss encounters, swarms)
        Relax,     ///< Cool-down period (fewer enemies, more resources)
        Transition ///< Blending between phases
    };

    // =============================================================================
    // PlayerPerformanceMetrics
    // =============================================================================

    /**
 * @brief Snapshot of player performance used by the director.
 */
    struct PlayerPerformanceMetrics
    {
        float healthPercent = 1.0f;    ///< Player health / max health
        float ammoPercent = 1.0f;      ///< Current ammo / max ammo
        float accuracy = 0.5f;         ///< Hit rate (0.0 - 1.0)
        int killsRecent = 0;           ///< Kills in the last sample window
        int deathsRecent = 0;          ///< Deaths in the last sample window
        float damageDealtRecent = 0;   ///< Damage dealt in the last window
        float damageTakenRecent = 0;   ///< Damage taken in the last window
        float timeSinceLastCombat = 0; ///< Seconds since last enemy encounter
    };

    // =============================================================================
    // DifficultyParameters
    // =============================================================================

    /**
 * @brief Adjustable game parameters controlled by the director.
 */
    struct DifficultyParameters
    {
        float enemySpawnRate = 1.0f;          ///< Multiplier for enemy spawn frequency
        float enemyHealthMultiplier = 1.0f;   ///< Multiplier for enemy health
        float enemyDamageMultiplier = 1.0f;   ///< Multiplier for enemy damage
        float enemyAccuracyMultiplier = 1.0f; ///< Multiplier for enemy accuracy
        float resourceSpawnRate = 1.0f;       ///< Multiplier for health/ammo pickups
        int maxConcurrentEnemies = 10;        ///< Maximum enemies active at once
        float specialEnemyChance = 0.1f;      ///< Probability of spawning elite enemies
    };

    // =============================================================================
    // AIDirector
    // =============================================================================

    /**
 * @class AIDirector
 * @brief Dynamic difficulty adjustment system.
 *
 * Monitors player performance and adjusts game parameters to maintain
 * a target intensity curve. The director cycles through Build-Up → Peak →
 * Relax phases, creating a natural rhythm of tension and relief.
 */
    class AIDirector
    {
      public:
        AIDirector();
        ~AIDirector() = default;

        /**
     * @brief Update the director for one frame.
     * @param world     ECS world for reading player/enemy state.
     * @param deltaTime Frame time in seconds.
     */
        void Update(World& world, float deltaTime);

        // --- Configuration ---

        /**
     * @brief Set the desired intensity range.
     * @param minIntensity Minimum target intensity (relax phase).
     * @param maxIntensity Maximum target intensity (peak phase).
     */
        void SetDesiredIntensityRange(float minIntensity, float maxIntensity);

        /** @brief Set duration of the build-up phase in seconds. */
        void SetBuildUpDuration(float seconds) { m_buildUpDuration = seconds; }

        /** @brief Set duration of the peak phase in seconds. */
        void SetPeakDuration(float seconds) { m_peakDuration = seconds; }

        /** @brief Set duration of the relax phase in seconds. */
        void SetRelaxDuration(float seconds) { m_relaxDuration = seconds; }

        /** @brief Enable or disable the director. */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        /** @brief Check if the director is enabled. */
        bool IsEnabled() const { return m_enabled; }

        // --- Query current state ---

        /** @brief Get the current intensity level (0.0 - 1.0). */
        float GetCurrentIntensity() const { return m_currentIntensity; }

        /** @brief Get the current phase. */
        DirectorPhase GetCurrentPhase() const { return m_currentPhase; }

        /** @brief Get the current difficulty parameters. */
        const DifficultyParameters& GetDifficultyParams() const { return m_difficultyParams; }

        /** @brief Get the current enemy spawn rate. */
        float GetCurrentSpawnRate() const { return m_difficultyParams.enemySpawnRate; }

        /** @brief Get the current damage multiplier. */
        float GetDamageMultiplier() const { return m_difficultyParams.enemyDamageMultiplier; }

        /** @brief Get the maximum concurrent enemies allowed. */
        int GetMaxConcurrentEnemies() const { return m_difficultyParams.maxConcurrentEnemies; }

        /** @brief Get the latest player performance metrics. */
        const PlayerPerformanceMetrics& GetPlayerMetrics() const { return m_playerMetrics; }

        // --- Console integration ---

        /** @brief Get director status (console integration). */
        std::string Console_GetStatus() const;

        /** @brief Force a specific phase (console integration / debugging). */
        void Console_ForcePhase(DirectorPhase phase);

        /** @brief Set intensity manually (console integration / debugging). */
        void Console_SetIntensity(float intensity);

      private:
        void SamplePlayerMetrics(World& world);
        void UpdateIntensity(float deltaTime);
        void ApplyDifficulty();
        float CalculatePlayerSkillLevel() const;

        bool m_enabled = true;

        // Intensity curve
        float m_currentIntensity = 0.0f;
        float m_targetIntensity = 0.0f;
        float m_minIntensity = 0.2f;
        float m_maxIntensity = 0.8f;

        // Phase timing
        DirectorPhase m_currentPhase = DirectorPhase::BuildUp;
        Spark::StateMachine<DirectorPhase> m_phaseFSM; ///< Drives phase cycling lifecycle
        float m_phaseTimer = 0.0f;
        float m_buildUpDuration = 60.0f;
        float m_peakDuration = 20.0f;
        float m_relaxDuration = 30.0f;

        // Player analysis
        PlayerPerformanceMetrics m_playerMetrics;
        float m_playerSkillEstimate = 0.5f;
        float m_metricsSampleTimer = 0.0f;
        float m_metricsSampleInterval = 2.0f;

        // Output parameters
        DifficultyParameters m_difficultyParams;
        DifficultyParameters m_baseDifficultyParams;
    };

} // namespace Spark::AI
