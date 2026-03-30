/**
 * @file RacingCameraSystem.cpp
 * @brief Multiple camera modes with smooth transitions
 */

#include "RacingCameraSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace Racing
{

    bool RacingCameraSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        // Initialize default configs for all camera modes
        for (size_t i = 0; i < static_cast<size_t>(CameraMode::Count); ++i)
            m_configs[i] = GetDefaultConfig(static_cast<CameraMode>(i));

        m_activeMode = CameraMode::Chase;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing camera system initialized (5 modes)");
        console.LogInfo("[Racing Camera] Camera system initialized (5 modes)");
        return true;
    }

    void RacingCameraSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        switch (m_activeMode)
        {
        case CameraMode::Chase:
            UpdateChase(deltaTime);
            break;
        case CameraMode::Cockpit:
            UpdateCockpit(deltaTime);
            break;
        case CameraMode::Hood:
            UpdateHood(deltaTime);
            break;
        case CameraMode::Orbit:
            UpdateOrbit(deltaTime);
            break;
        case CameraMode::Cinematic:
            UpdateCinematic(deltaTime);
            break;
        default:
            break;
        }

        // Speed-dependent camera effects
        ApplySpeedShake(m_vehicleSpeed, deltaTime);

        // Transition blending
        if (m_transitionProgress < 1.0f)
            m_transitionProgress = std::min(1.0f, m_transitionProgress + deltaTime * 3.0f);
    }

    void RacingCameraSystem::Shutdown()
    {
        m_initialized = false;
    }

    void RacingCameraSystem::SetMode(CameraMode mode)
    {
        if (mode == m_activeMode)
            return;

        m_activeMode = mode;
        m_transitionProgress = 0.0f;
        m_state.fov = m_configs[static_cast<size_t>(mode)].fov;

        auto& console = Spark::SimpleConsole::GetInstance();
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Racing camera mode changed to: %s", ModeToString(mode));
        console.LogInfo("[Racing Camera] Mode: " + std::string(ModeToString(mode)));
    }

    void RacingCameraSystem::CycleMode()
    {
        uint8_t next = (static_cast<uint8_t>(m_activeMode) + 1) % static_cast<uint8_t>(CameraMode::Count);
        SetMode(static_cast<CameraMode>(next));
    }

    void RacingCameraSystem::SetTarget(float x, float y, float z, float heading, float speed)
    {
        m_state.targetX = x;
        m_state.targetY = y;
        m_state.targetZ = z;
        m_state.targetHeading = heading;
        m_vehicleSpeed = speed;
    }

    void RacingCameraSystem::SetOrbitAngles(float yaw, float pitch)
    {
        m_orbitYaw = yaw;
        m_orbitPitch = std::clamp(pitch, -80.0f, 80.0f);
    }

    const char* RacingCameraSystem::ModeToString(CameraMode mode)
    {
        switch (mode)
        {
        case CameraMode::Chase:
            return "Chase";
        case CameraMode::Cockpit:
            return "Cockpit";
        case CameraMode::Hood:
            return "Hood";
        case CameraMode::Orbit:
            return "Orbit";
        case CameraMode::Cinematic:
            return "Cinematic";
        default:
            return "Unknown";
        }
    }

    void RacingCameraSystem::UpdateChase(float dt)
    {
        const auto& cfg = m_configs[static_cast<size_t>(CameraMode::Chase)];

        // Position behind the vehicle
        float behindX = m_state.targetX - std::sin(m_state.targetHeading) * cfg.distance;
        float behindZ = m_state.targetZ - std::cos(m_state.targetHeading) * cfg.distance;
        float aboveY = m_state.targetY + cfg.height;

        SmoothFollow(behindX, aboveY, behindZ, cfg.smoothSpeed, dt);

        // Look at a point ahead of the vehicle
        m_state.lookX = m_state.targetX + std::sin(m_state.targetHeading) * cfg.lookAhead;
        m_state.lookY = m_state.targetY + 1.0f;
        m_state.lookZ = m_state.targetZ + std::cos(m_state.targetHeading) * cfg.lookAhead;

        // Speed-dependent FOV increase
        float speedFraction = std::clamp(m_vehicleSpeed / 300.0f, 0.0f, 1.0f);
        m_state.fov = cfg.fov + speedFraction * 15.0f;
    }

    void RacingCameraSystem::UpdateCockpit(float dt)
    {
        // Camera sits at the driver's head position inside the vehicle
        m_state.posX = m_state.targetX;
        m_state.posY = m_state.targetY + 1.2f; // Approximate head height
        m_state.posZ = m_state.targetZ;

        m_state.lookX = m_state.targetX + std::sin(m_state.targetHeading) * 20.0f;
        m_state.lookY = m_state.targetY + 1.2f;
        m_state.lookZ = m_state.targetZ + std::cos(m_state.targetHeading) * 20.0f;

        m_state.fov = m_configs[static_cast<size_t>(CameraMode::Cockpit)].fov;
        (void)dt;
    }

    void RacingCameraSystem::UpdateHood(float dt)
    {
        // Camera on the hood, looking forward
        m_state.posX = m_state.targetX + std::sin(m_state.targetHeading) * 1.5f;
        m_state.posY = m_state.targetY + 1.0f;
        m_state.posZ = m_state.targetZ + std::cos(m_state.targetHeading) * 1.5f;

        m_state.lookX = m_state.targetX + std::sin(m_state.targetHeading) * 30.0f;
        m_state.lookY = m_state.targetY + 0.8f;
        m_state.lookZ = m_state.targetZ + std::cos(m_state.targetHeading) * 30.0f;

        m_state.fov = m_configs[static_cast<size_t>(CameraMode::Hood)].fov;
        (void)dt;
    }

    void RacingCameraSystem::UpdateOrbit(float dt)
    {
        const auto& cfg = m_configs[static_cast<size_t>(CameraMode::Orbit)];
        constexpr float degToRad = 3.14159265f / 180.0f;

        float yawRad = m_orbitYaw * degToRad;
        float pitchRad = m_orbitPitch * degToRad;

        float orbitX = std::cos(pitchRad) * std::sin(yawRad) * cfg.distance;
        float orbitY = std::sin(pitchRad) * cfg.distance;
        float orbitZ = std::cos(pitchRad) * std::cos(yawRad) * cfg.distance;

        SmoothFollow(m_state.targetX + orbitX, m_state.targetY + orbitY + 2.0f, m_state.targetZ + orbitZ,
                     cfg.smoothSpeed, dt);

        m_state.lookX = m_state.targetX;
        m_state.lookY = m_state.targetY + 1.0f;
        m_state.lookZ = m_state.targetZ;

        m_state.fov = cfg.fov;
    }

    void RacingCameraSystem::UpdateCinematic(float dt)
    {
        // Cinematic: slow-orbiting wide-angle shot
        const auto& cfg = m_configs[static_cast<size_t>(CameraMode::Cinematic)];
        static float cinematicAngle = 0.0f;
        cinematicAngle += dt * 0.3f; // Slow rotation

        float dist = cfg.distance * 1.5f;
        float camX = m_state.targetX + std::sin(cinematicAngle) * dist;
        float camZ = m_state.targetZ + std::cos(cinematicAngle) * dist;

        SmoothFollow(camX, m_state.targetY + cfg.height * 2.0f, camZ, cfg.smoothSpeed * 0.5f, dt);

        m_state.lookX = m_state.targetX;
        m_state.lookY = m_state.targetY;
        m_state.lookZ = m_state.targetZ;

        m_state.fov = cfg.fov;
    }

    void RacingCameraSystem::ApplySpeedShake(float speed, float dt)
    {
        const auto& cfg = m_configs[static_cast<size_t>(m_activeMode)];
        if (cfg.shakeIntensity <= 0.0f)
            return;

        // Speed-proportional camera shake
        float intensity = cfg.shakeIntensity * std::clamp(speed / 250.0f, 0.0f, 1.0f);
        static float shakePhase = 0.0f;
        shakePhase += dt * 30.0f;

        m_state.posX += std::sin(shakePhase * 1.3f) * intensity * 0.05f;
        m_state.posY += std::cos(shakePhase * 1.7f) * intensity * 0.03f;
    }

    void RacingCameraSystem::SmoothFollow(float targetX, float targetY, float targetZ, float speed, float dt)
    {
        float t = std::clamp(speed * dt, 0.0f, 1.0f);
        m_state.posX += (targetX - m_state.posX) * t;
        m_state.posY += (targetY - m_state.posY) * t;
        m_state.posZ += (targetZ - m_state.posZ) * t;
    }

    CameraModeConfig RacingCameraSystem::GetDefaultConfig(CameraMode mode)
    {
        CameraModeConfig cfg{};
        cfg.mode = mode;

        switch (mode)
        {
        case CameraMode::Chase:
            cfg.distance = 8.0f;
            cfg.height = 3.0f;
            cfg.lookAhead = 5.0f;
            cfg.smoothSpeed = 5.0f;
            cfg.fov = 75.0f;
            cfg.shakeIntensity = 0.3f;
            break;
        case CameraMode::Cockpit:
            cfg.distance = 0.0f;
            cfg.height = 1.2f;
            cfg.lookAhead = 20.0f;
            cfg.smoothSpeed = 10.0f;
            cfg.fov = 90.0f;
            cfg.shakeIntensity = 0.5f;
            break;
        case CameraMode::Hood:
            cfg.distance = 1.5f;
            cfg.height = 1.0f;
            cfg.lookAhead = 30.0f;
            cfg.smoothSpeed = 10.0f;
            cfg.fov = 85.0f;
            cfg.shakeIntensity = 0.4f;
            break;
        case CameraMode::Orbit:
            cfg.distance = 12.0f;
            cfg.height = 4.0f;
            cfg.lookAhead = 0.0f;
            cfg.smoothSpeed = 4.0f;
            cfg.fov = 60.0f;
            cfg.shakeIntensity = 0.0f;
            break;
        case CameraMode::Cinematic:
            cfg.distance = 15.0f;
            cfg.height = 5.0f;
            cfg.lookAhead = 0.0f;
            cfg.smoothSpeed = 2.0f;
            cfg.fov = 50.0f;
            cfg.shakeIntensity = 0.0f;
            break;
        default:
            break;
        }

        return cfg;
    }

    void RacingCameraSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Racing Camera"))
            return;

        ImGui::Text("Mode: %s", ModeToString(m_activeMode));
        ImGui::Text("FOV: %.1f", m_state.fov);
        ImGui::Text("Position: (%.1f, %.1f, %.1f)", m_state.posX, m_state.posY, m_state.posZ);
        ImGui::Text("Look At: (%.1f, %.1f, %.1f)", m_state.lookX, m_state.lookY, m_state.lookZ);
        ImGui::Text("Vehicle Speed: %.1f km/h", m_vehicleSpeed);

        ImGui::Separator();

        // Mode buttons
        const char* modeNames[] = {"Chase", "Cockpit", "Hood", "Orbit", "Cinematic"};
        for (int i = 0; i < 5; ++i)
        {
            if (ImGui::Button(modeNames[i]))
                SetMode(static_cast<CameraMode>(i));
            if (i < 4)
                ImGui::SameLine();
        }

        // Orbit controls
        if (m_activeMode == CameraMode::Orbit)
        {
            ImGui::SliderFloat("Orbit Yaw", &m_orbitYaw, -180.0f, 180.0f);
            ImGui::SliderFloat("Orbit Pitch", &m_orbitPitch, -80.0f, 80.0f);
        }
#endif
    }

} // namespace Racing
