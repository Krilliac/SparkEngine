/**
 * @file AIDirector.cpp
 * @brief Implementation of the AI Director dynamic difficulty system
 */

#include "AIDirector.h"
#include "../../Core/FaultIsolation.h"
#include "../../Utils/Validate.h"

#include "../ECS/Components/GameplayComponents.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Spark::AI
{

    AIDirector::AIDirector()
    {
        m_baseDifficultyParams = DifficultyParameters{};

        // --- Phase FSM setup ---
        m_phaseFSM.AddState(
            DirectorPhase::BuildUp,
            [this]()
            {
                SPARK_LOG_INFO(Spark::LogCategory::AI, "AIDirector: phase transition -> BuildUp");
                m_currentPhase = DirectorPhase::BuildUp;
                m_phaseTimer = 0.0f;
            },
            [this](float dt)
            {
                m_phaseTimer += dt;
                float progress = m_phaseTimer / m_buildUpDuration;
                m_targetIntensity = m_minIntensity + (m_maxIntensity - m_minIntensity) * progress;
            });

        m_phaseFSM.AddState(
            DirectorPhase::Peak,
            [this]()
            {
                SPARK_LOG_INFO(Spark::LogCategory::AI, "AIDirector: phase transition -> Peak (intensity=%.2f)",
                               m_currentIntensity);
                m_currentPhase = DirectorPhase::Peak;
                m_phaseTimer = 0.0f;
            },
            [this](float dt)
            {
                m_phaseTimer += dt;
                m_targetIntensity = m_maxIntensity;
            });

        m_phaseFSM.AddState(
            DirectorPhase::Relax,
            [this]()
            {
                SPARK_LOG_INFO(Spark::LogCategory::AI, "AIDirector: phase transition -> Relax (intensity=%.2f)",
                               m_currentIntensity);
                m_currentPhase = DirectorPhase::Relax;
                m_phaseTimer = 0.0f;
            },
            [this](float dt)
            {
                m_phaseTimer += dt;
                float progress = m_phaseTimer / m_relaxDuration;
                m_targetIntensity = m_maxIntensity - (m_maxIntensity - m_minIntensity) * progress;
            });

        m_phaseFSM.AddState(
            DirectorPhase::Transition,
            [this]()
            {
                m_currentPhase = DirectorPhase::Transition;
                m_phaseTimer = 0.0f;
            },
            [this](float dt) { m_phaseTimer += dt; });

        // Transitions
        m_phaseFSM.AddTransition(DirectorPhase::BuildUp, DirectorPhase::Peak,
                                 [this]() { return m_phaseTimer >= m_buildUpDuration; });

        m_phaseFSM.AddTransition(DirectorPhase::Peak, DirectorPhase::Relax,
                                 [this]()
                                 {
                                     float earlyExitThreshold = m_peakDuration * 0.5f;
                                     bool playerStruggling = m_playerMetrics.healthPercent < 0.2f;
                                     return m_phaseTimer >= m_peakDuration ||
                                            (m_phaseTimer >= earlyExitThreshold && playerStruggling);
                                 });

        m_phaseFSM.AddTransition(DirectorPhase::Relax, DirectorPhase::BuildUp,
                                 [this]() { return m_phaseTimer >= m_relaxDuration; });

        m_phaseFSM.AddTransition(DirectorPhase::Transition, DirectorPhase::BuildUp,
                                 [this]() { return m_phaseTimer >= 2.0f; });

        m_phaseFSM.Start(DirectorPhase::BuildUp);
    }

    void AIDirector::Update(World& world, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);
        if (!m_enabled)
        {
            return;
        }

        // Sample player metrics periodically
        m_metricsSampleTimer += deltaTime;
        if (m_metricsSampleTimer >= m_metricsSampleInterval)
        {
            SamplePlayerMetrics(world);
            m_metricsSampleTimer = 0.0f;
        }

        UpdateIntensity(deltaTime);
        ApplyDifficulty();
    }

    void AIDirector::SetDesiredIntensityRange(float minIntensity, float maxIntensity)
    {
        SPARK_WARN_IF(Spark::LogCategory::AI, minIntensity > maxIntensity,
                      "SetDesiredIntensityRange: minIntensity > maxIntensity, values will be swapped");
        m_minIntensity = std::clamp(minIntensity, 0.0f, 1.0f);
        m_maxIntensity = std::clamp(maxIntensity, 0.0f, 1.0f);
        if (m_minIntensity > m_maxIntensity)
        {
            std::swap(m_minIntensity, m_maxIntensity);
        }
    }

    void AIDirector::SamplePlayerMetrics(World& world)
    {
        // Read player health from ECS
        auto entities = world.GetEntitiesWith<HealthComponent, TagComponent>();
        for (auto entity : entities)
        {
            auto* tag = world.GetComponent<TagComponent>(entity);
            if (tag && tag->HasTag("Player"))
            {
                auto* health = world.GetComponent<HealthComponent>(entity);
                if (health)
                {
                    m_playerMetrics.healthPercent = health->health / health->maxHealth;
                }
                break;
            }
        }

        // Update time since last combat
        m_playerMetrics.timeSinceLastCombat += m_metricsSampleInterval;

        // Estimate player skill from accumulated metrics
        m_playerSkillEstimate = CalculatePlayerSkillLevel();
    }

    void AIDirector::UpdateIntensity(float deltaTime)
    {
        SPARK_GUARDED_UPDATE("AI:PhaseFSM", "AI", { m_phaseFSM.Tick(deltaTime); });

        // Adjust target intensity based on player skill
        float skillAdjustment = (m_playerSkillEstimate - 0.5f) * 0.2f;
        m_targetIntensity = std::clamp(m_targetIntensity + skillAdjustment, 0.0f, 1.0f);

        // Smooth interpolation towards target
        float lerpSpeed = 0.5f * deltaTime;
        m_currentIntensity += (m_targetIntensity - m_currentIntensity) * lerpSpeed;
        m_currentIntensity = std::clamp(m_currentIntensity, 0.0f, 1.0f);
    }

    void AIDirector::ApplyDifficulty()
    {
        float intensity = m_currentIntensity;

        // Scale spawn rate: low intensity = fewer enemies, high = more
        m_difficultyParams.enemySpawnRate = m_baseDifficultyParams.enemySpawnRate * (0.5f + intensity * 1.5f);

        // Scale enemy health: slight increase with intensity
        m_difficultyParams.enemyHealthMultiplier =
            m_baseDifficultyParams.enemyHealthMultiplier * (0.8f + intensity * 0.4f);

        // Scale enemy damage: moderate increase with intensity
        m_difficultyParams.enemyDamageMultiplier =
            m_baseDifficultyParams.enemyDamageMultiplier * (0.7f + intensity * 0.6f);

        // Scale enemy accuracy: moderate increase with intensity
        m_difficultyParams.enemyAccuracyMultiplier =
            m_baseDifficultyParams.enemyAccuracyMultiplier * (0.6f + intensity * 0.8f);

        // Resource availability: inverse of intensity (more resources during relax)
        m_difficultyParams.resourceSpawnRate = m_baseDifficultyParams.resourceSpawnRate * (1.5f - intensity * 1.0f);

        // Max concurrent enemies scales with intensity
        m_difficultyParams.maxConcurrentEnemies =
            static_cast<int>(m_baseDifficultyParams.maxConcurrentEnemies * (0.5f + intensity * 1.0f));

        // Special enemy chance increases at high intensity
        m_difficultyParams.specialEnemyChance = m_baseDifficultyParams.specialEnemyChance * (0.2f + intensity * 1.6f);
    }

    float AIDirector::CalculatePlayerSkillLevel() const
    {
        float skill = 0.5f; // Baseline

        // Better accuracy → higher skill
        skill += (m_playerMetrics.accuracy - 0.5f) * 0.3f;

        // High health → player doing well
        skill += (m_playerMetrics.healthPercent - 0.5f) * 0.2f;

        // More kills relative to deaths → higher skill
        float kdRatio = (m_playerMetrics.deathsRecent > 0) ? static_cast<float>(m_playerMetrics.killsRecent) /
                                                                 static_cast<float>(m_playerMetrics.deathsRecent)
                                                           : static_cast<float>(m_playerMetrics.killsRecent);
        skill += std::clamp((kdRatio - 1.0f) * 0.1f, -0.2f, 0.2f);

        return std::clamp(skill, 0.0f, 1.0f);
    }

    std::string AIDirector::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== AI Director Status ===\n";
        oss << "Enabled: " << (m_enabled ? "YES" : "NO") << "\n";

        const char* phaseNames[] = {"BuildUp", "Peak", "Relax", "Transition"};
        oss << "Phase: " << phaseNames[static_cast<int>(m_currentPhase)] << "\n";
        oss << "Phase timer: " << m_phaseTimer << "s\n";
        oss << "Intensity: " << m_currentIntensity << " (target: " << m_targetIntensity << ")\n";
        oss << "Player skill estimate: " << m_playerSkillEstimate << "\n";
        oss << "\n--- Difficulty Parameters ---\n";
        oss << "Spawn rate: " << m_difficultyParams.enemySpawnRate << "x\n";
        oss << "Enemy health: " << m_difficultyParams.enemyHealthMultiplier << "x\n";
        oss << "Enemy damage: " << m_difficultyParams.enemyDamageMultiplier << "x\n";
        oss << "Enemy accuracy: " << m_difficultyParams.enemyAccuracyMultiplier << "x\n";
        oss << "Resource spawn: " << m_difficultyParams.resourceSpawnRate << "x\n";
        oss << "Max enemies: " << m_difficultyParams.maxConcurrentEnemies << "\n";
        oss << "Special chance: " << m_difficultyParams.specialEnemyChance << "\n";
        oss << "\n--- Player Metrics ---\n";
        oss << "Health: " << static_cast<int>(m_playerMetrics.healthPercent * 100) << "%\n";
        oss << "Ammo: " << static_cast<int>(m_playerMetrics.ammoPercent * 100) << "%\n";
        oss << "Accuracy: " << static_cast<int>(m_playerMetrics.accuracy * 100) << "%\n";
        oss << "Time since combat: " << m_playerMetrics.timeSinceLastCombat << "s\n";
        return oss.str();
    }

    void AIDirector::Console_ForcePhase(DirectorPhase phase)
    {
        m_phaseFSM.TransitionTo(phase);
    }

    void AIDirector::Console_SetIntensity(float intensity)
    {
        float clamped = std::clamp(intensity, 0.0f, 1.0f);
        SPARK_LOG_INFO(Spark::LogCategory::AI, "AIDirector: intensity forced to %.2f (was %.2f)", clamped,
                       m_currentIntensity);
        m_currentIntensity = clamped;
        m_targetIntensity = m_currentIntensity;
    }

} // namespace Spark::AI
