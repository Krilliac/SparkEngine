/**
 * @file RacingAIDriver.cpp
 * @brief AI racer behavior with difficulty scaling and rubber-banding
 */

#include "RacingAIDriver.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cmath>

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace Racing
{

    bool RacingAIDriver::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
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
        m_initialized = false;
    }

    void RacingAIDriver::AddDriver(const AIDriverConfig& config)
    {
        m_drivers.push_back(config);

        AIDriverState state{};
        state.vehicleId = config.vehicleId;
        state.reactionDelay = config.reactionTime;
        m_states.push_back(state);
    }

    void RacingAIDriver::RemoveDriver(uint32_t vehicleId)
    {
        for (size_t i = 0; i < m_drivers.size(); ++i)
        {
            if (m_drivers[i].vehicleId == vehicleId)
            {
                m_drivers.erase(m_drivers.begin() + static_cast<ptrdiff_t>(i));
                m_states.erase(m_states.begin() + static_cast<ptrdiff_t>(i));
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
        console.LogInfo("[Racing AI] Global difficulty set to: " + std::string(names[static_cast<int>(difficulty)]));
    }

    void RacingAIDriver::UpdateRubberBanding(float playerDistance, float leadDistance, float lastDistance)
    {
        if (m_states.empty())
            return;

        // Rubber-banding: AI behind the player gets a speed boost,
        // AI ahead of the player gets a slight reduction.
        float playerPos = playerDistance;
        float totalSpread = leadDistance - lastDistance;
        if (totalSpread < 1.0f)
            totalSpread = 1.0f;

        for (auto& state : m_states)
        {
            // Base factor: no adjustment
            state.rubberBandFactor = 1.0f;

            // Simple model: further behind = bigger boost, further ahead = small penalty
            // (This would be computed per-vehicle with actual distance data in production)
        }

        (void)playerPos;
    }

    const AIDriverState* RacingAIDriver::GetDriverState(uint32_t vehicleId) const
    {
        for (const auto& state : m_states)
        {
            if (state.vehicleId == vehicleId)
                return &state;
        }
        return nullptr;
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
    }

    void RacingAIDriver::ComputeSteering(const AIDriverConfig& config, AIDriverState& state)
    {
        // Steer toward the target waypoint.
        // In a real implementation this would query the track system for waypoint
        // positions and compute the angle. Here we produce a placeholder oscillation
        // that represents AI following a racing line.
        float targetAngle = 0.0f; // Would come from track waypoint direction

        // Line accuracy adds noise: lower accuracy = wider, less precise lines
        float noise = (1.0f - config.lineAccuracy) * 0.3f;

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

        // Aggressive AI attempts overtakes more often
        // In production, this would check proximity to vehicles ahead
        // and choose inside/outside line based on upcoming corner direction.
        (void)config;
        (void)dt;
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
