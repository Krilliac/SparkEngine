/**
 * @file RacingAIDriver.cpp
 * @brief AI racer behavior with difficulty scaling and rubber-banding
 */

#include "RacingAIDriver.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace Racing
{
    namespace
    {
        constexpr float kSyntheticTrackWaypointCount = 256.0f;
        constexpr float kWaypointAdvancePerSecond = 14.0f;
        constexpr float kMaxRubberBandBoost = 1.15f;
        constexpr float kMaxRubberBandPenalty = 0.90f;

        [[nodiscard]] float GetTrackPhase(const AIDriverState& state)
        {
            return (static_cast<float>(state.targetWaypoint % static_cast<uint32_t>(kSyntheticTrackWaypointCount)) /
                    kSyntheticTrackWaypointCount) *
                   (2.0f * 3.14159265359f);
        }

        [[nodiscard]] float NormalizeSigned(float value, float range)
        {
            if (range <= 0.0f)
                return 0.0f;
            return std::clamp(value / range, -1.0f, 1.0f);
        }
    } // namespace


    bool RacingAIDriver::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing AI driver system initialized");
        console.LogInfo("[Racing AI] AI driver system initialized");
        return true;
    }

    void RacingAIDriver::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        for (size_t i = 0; i < m_drivers.size(); ++i)
        {
            if (i < m_states.size())
                UpdateDriver(m_drivers[i], m_states[i], deltaTime);
        }
    }

    void RacingAIDriver::Shutdown()
    {
        m_drivers.clear();
        m_states.clear();
        m_stateIndexByVehicleId.clear();
        m_initialized = false;
    }

    void RacingAIDriver::AddDriver(const AIDriverConfig& config)
    {
        m_drivers.push_back(config);

        AIDriverState state{};
        state.vehicleId = config.vehicleId;
        state.reactionDelay = config.reactionTime;
        m_states.push_back(state);
        m_stateIndexByVehicleId[state.vehicleId] = m_states.size() - 1;
    }

    void RacingAIDriver::RemoveDriver(uint32_t vehicleId)
    {
        for (size_t i = 0; i < m_drivers.size(); ++i)
        {
            if (m_drivers[i].vehicleId == vehicleId)
            {
                m_drivers.erase(m_drivers.begin() + static_cast<ptrdiff_t>(i));
                m_states.erase(m_states.begin() + static_cast<ptrdiff_t>(i));
                RebuildStateIndex();
                return;
            }
        }
    }

    void RacingAIDriver::SetGlobalDifficulty(AIDifficulty difficulty)
    {
        m_globalDifficulty = difficulty;

        // Recompute stats for all drivers based on new difficulty
        for (auto& driver : m_drivers)
        {
            driver.difficulty = difficulty;
            AIDriverConfig preset = MakeConfigForDifficulty(difficulty);
            driver.speedFactor = preset.speedFactor;
            driver.lineAccuracy = preset.lineAccuracy;
            driver.reactionTime = preset.reactionTime;
            driver.aggressiveness = preset.aggressiveness;
        }

        auto& console = Spark::SimpleConsole::GetInstance();
        const char* names[] = {"Easy", "Medium", "Hard", "Expert"};
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing AI global difficulty set to: %s",
                       names[static_cast<int>(difficulty)]);
        console.LogInfo("[Racing AI] Global difficulty set to: " + std::string(names[static_cast<int>(difficulty)]));
    }

    void RacingAIDriver::UpdateRubberBanding(float playerDistance, float leadDistance, float lastDistance)
    {
        if (m_states.empty())
            return;

        // Rubber-banding model with synthetic race positions:
        // - m_states index 0 is considered race leader, last index is tail.
        // - Distances are normalized to [lastDistance, leadDistance].
        // - Drivers behind the player receive up to +15% throttle.
        // - Drivers ahead of the player receive up to -10% throttle.
        const float minDistance = std::min(lastDistance, leadDistance);
        const float maxDistance = std::max(lastDistance, leadDistance);
        const float spread = std::max(maxDistance - minDistance, 1.0f);
        const float clampedPlayerDistance = std::clamp(playerDistance, minDistance, maxDistance);

        for (size_t i = 0; i < m_states.size(); ++i)
        {
            auto& state = m_states[i];
            const float rankLerp =
                (m_states.size() == 1) ? 0.0f : (static_cast<float>(i) / static_cast<float>(m_states.size() - 1));
            const float aiDistance = maxDistance - (rankLerp * spread);
            const float distanceDelta = clampedPlayerDistance - aiDistance;
            const float normalizedDelta = NormalizeSigned(distanceDelta, spread);

            state.rubberBandFactor = 1.0f;

            if (normalizedDelta < 0.0f)
            {
                // AI is behind the player.
                const float behindAmount = -normalizedDelta;
                state.rubberBandFactor = 1.0f + (kMaxRubberBandBoost - 1.0f) * behindAmount;
            }
            else if (normalizedDelta > 0.0f)
            {
                // AI is ahead of the player.
                const float aheadAmount = normalizedDelta;
                state.rubberBandFactor = 1.0f - (1.0f - kMaxRubberBandPenalty) * aheadAmount;
            }

            state.rubberBandFactor = std::clamp(state.rubberBandFactor, kMaxRubberBandPenalty, kMaxRubberBandBoost);
        }
    }

    const AIDriverState* RacingAIDriver::GetDriverState(uint32_t vehicleId) const
    {
        const auto it = m_stateIndexByVehicleId.find(vehicleId);
        if (it == m_stateIndexByVehicleId.end())
            return nullptr;
        const size_t index = it->second;
        return (index < m_states.size()) ? &m_states[index] : nullptr;
    }

    void RacingAIDriver::RebuildStateIndex()
    {
        m_stateIndexByVehicleId.clear();
        m_stateIndexByVehicleId.reserve(m_states.size());
        for (size_t i = 0; i < m_states.size(); ++i)
            m_stateIndexByVehicleId[m_states[i].vehicleId] = i;
    }

    std::string RacingAIDriver::GetDriverListString() const
    {
        const char* diffNames[] = {"Easy", "Medium", "Hard", "Expert"};

        std::string result = "AI Drivers (" + std::to_string(m_drivers.size()) + "):\n";
        for (size_t i = 0; i < m_drivers.size(); ++i)
        {
            const auto& d = m_drivers[i];
            result += "  [" + std::to_string(d.vehicleId) + "] " + d.name;
            result += " | " + std::string(diffNames[static_cast<int>(d.difficulty)]);
            result += " | Speed: " + std::to_string(static_cast<int>(d.speedFactor * 100.0f)) + "%";
            if (i < m_states.size() && m_states[i].isOvertaking)
                result += " [OVERTAKING]";
            result += "\n";
        }
        return result;
    }

    void RacingAIDriver::UpdateDriver(AIDriverConfig& config, AIDriverState& state, float dt)
    {
        if (dt <= 0.0f)
            return;

        dt = std::min(dt, 0.1f);

        // Reaction delay simulation
        if (state.reactionDelay > 0.0f)
        {
            state.reactionDelay -= dt;
            return;
        }

        ComputeSteering(config, state);
        ComputeThrottle(config, state);
        ComputeOvertaking(config, state, dt);

        // Apply rubber-banding to throttle
        state.throttle *= state.rubberBandFactor;

        // Nitro usage: AI uses nitro on straights when behind
        state.useNitro = (state.rubberBandFactor > 1.05f) && (std::abs(state.steer) < 0.2f);

        // Advance synthetic waypoint target based on intended throttle.
        const float waypointAdvance = dt * kWaypointAdvancePerSecond * std::max(0.15f, state.throttle);
        state.targetWaypoint = (state.targetWaypoint + static_cast<uint32_t>(std::max(1.0f, waypointAdvance))) %
                               static_cast<uint32_t>(kSyntheticTrackWaypointCount);

        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Racing AI driver %u: throttle=%.2f steer=%.2f nitro=%s",
                        state.vehicleId, state.throttle, state.steer, state.useNitro ? "yes" : "no");
    }

    void RacingAIDriver::ComputeSteering(const AIDriverConfig& config, AIDriverState& state)
    {
        // Synthetic racing line (sine-cosine blend) for deterministic behavior
        // when no track-waypoint service is injected yet.
        const float phase = GetTrackPhase(state);
        const float upcomingPhase = phase + (state.lookAheadCount * 0.05f);
        const float targetAngle = std::sin(phase) * 0.55f + std::sin(upcomingPhase * 0.7f) * 0.25f;

        // Line accuracy adds noise: lower accuracy = wider, less precise lines
        const float deterministicSeed = static_cast<float>((state.vehicleId * 1103515245u + 12345u) & 0x3FFu) / 1023.0f;
        const float bias = (deterministicSeed - 0.5f) * 2.0f; // [-1, 1]
        const float noise = (1.0f - std::clamp(config.lineAccuracy, 0.0f, 1.0f)) * 0.2f * bias;

        state.steer = std::clamp(targetAngle + noise, -1.0f, 1.0f);

        // Apply overtake lateral offset
        if (state.isOvertaking)
            state.steer += state.overtakeOffset * 0.5f;

        state.steer = std::clamp(state.steer, -1.0f, 1.0f);
    }

    void RacingAIDriver::ComputeThrottle(const AIDriverConfig& config, AIDriverState& state)
    {
        // Base throttle from speed factor
        state.throttle = config.speedFactor;

        // Brake in sharp turns
        float absSteer = std::abs(state.steer);
        if (absSteer > 0.6f)
        {
            state.brake = absSteer * 0.5f;
            state.throttle *= 0.6f;
        }
        else
        {
            state.brake = 0.0f;
        }

        // Expert AI uses drift in tight corners
        state.useDrift = (config.difficulty == AIDifficulty::Expert) && (absSteer > 0.7f);
    }

    void RacingAIDriver::ComputeOvertaking(const AIDriverConfig& config, AIDriverState& state, float dt)
    {
        if (state.isOvertaking)
        {
            state.overtakeTimer -= dt;
            if (state.overtakeTimer <= 0.0f)
            {
                state.isOvertaking = false;
                state.overtakeOffset = 0.0f;
            }
            return;
        }

        // Synthetic overtake trigger: only attempt on mild steering and when
        // aggressiveness plus rubber-band pressure indicates catch-up intent.
        if (std::abs(state.steer) > 0.35f)
            return;

        const float overtakeIntent = std::clamp(config.aggressiveness * state.rubberBandFactor, 0.0f, 1.5f);
        if (overtakeIntent < 0.35f)
            return;

        const float phase = GetTrackPhase(state);
        const float triggerWave = 0.5f + 0.5f * std::sin(phase * 4.0f + static_cast<float>(state.vehicleId) * 0.17f);
        if (triggerWave > overtakeIntent)
            return;

        state.isOvertaking = true;
        state.overtakeTimer = std::clamp(0.8f + (1.4f - overtakeIntent), 0.5f, 2.0f);
        const float lateralDirection =
            (std::sin(phase + static_cast<float>(state.vehicleId) * 0.13f) >= 0.0f) ? 1.0f : -1.0f;
        state.overtakeOffset = lateralDirection * std::clamp(0.35f + config.aggressiveness * 0.45f, 0.25f, 0.85f);

        // Advance virtual waypoint target slightly so overtake has visible intent
        // on subsequent steering updates.
        const float phaseAdvance =
            std::max(dt, 0.016f) * kWaypointAdvancePerSecond * (1.0f + config.speedFactor * 0.25f);
        state.targetWaypoint = (state.targetWaypoint + static_cast<uint32_t>(phaseAdvance * 2.0f)) %
                               static_cast<uint32_t>(kSyntheticTrackWaypointCount);
    }

    AIDriverConfig RacingAIDriver::MakeConfigForDifficulty(AIDifficulty difficulty)
    {
        AIDriverConfig config{};
        config.difficulty = difficulty;

        switch (difficulty)
        {
        case AIDifficulty::Easy:
            config.speedFactor = 0.70f;
            config.lineAccuracy = 0.50f;
            config.reactionTime = 0.30f;
            config.aggressiveness = 0.2f;
            break;
        case AIDifficulty::Medium:
            config.speedFactor = 0.85f;
            config.lineAccuracy = 0.70f;
            config.reactionTime = 0.15f;
            config.aggressiveness = 0.5f;
            break;
        case AIDifficulty::Hard:
            config.speedFactor = 0.95f;
            config.lineAccuracy = 0.85f;
            config.reactionTime = 0.08f;
            config.aggressiveness = 0.7f;
            break;
        case AIDifficulty::Expert:
            config.speedFactor = 1.00f;
            config.lineAccuracy = 0.95f;
            config.reactionTime = 0.03f;
            config.aggressiveness = 0.9f;
            break;
        default:
            break;
        }

        return config;
    }

    void RacingAIDriver::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Racing AI Drivers"))
            return;

        const char* diffNames[] = {"Easy", "Medium", "Hard", "Expert"};
        ImGui::Text("Global Difficulty: %s", diffNames[static_cast<int>(m_globalDifficulty)]);
        ImGui::Text("Active Drivers: %zu", m_drivers.size());
        ImGui::Separator();

        for (size_t i = 0; i < m_drivers.size(); ++i)
        {
            const auto& driver = m_drivers[i];
            if (i < m_states.size())
            {
                const auto& state = m_states[i];
                if (ImGui::TreeNode(driver.name.c_str()))
                {
                    ImGui::Text("Difficulty: %s", diffNames[static_cast<int>(driver.difficulty)]);
                    ImGui::Text("Speed Factor: %.0f%%", driver.speedFactor * 100.0f);
                    ImGui::Text("Throttle: %.2f | Brake: %.2f | Steer: %.2f", state.throttle, state.brake, state.steer);
                    ImGui::Text("Rubber Band: %.2f", state.rubberBandFactor);
                    ImGui::Text("Overtaking: %s", state.isOvertaking ? "Yes" : "No");
                    ImGui::TreePop();
                }
            }
        }
#endif
    }

} // namespace Racing
