/**
 * @file RacingCameraSystem.h
 * @brief Multiple camera modes for the racing showcase
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides chase, cockpit, hood, orbit, and cinematic camera modes
 * with smooth transitions and vehicle-following behavior.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/RacingEnums.h"
#include <cstdint>
#include <string>

namespace Racing
{

    /**
     * @brief Camera state for smooth interpolation
     */
    struct CameraState
    {
        // Current position and orientation
        float posX = 0.0f;
        float posY = 5.0f;
        float posZ = -10.0f;
        float lookX = 0.0f;
        float lookY = 0.0f;
        float lookZ = 0.0f;
        float fov = 75.0f;

        // Target position (for smooth follow)
        float targetX = 0.0f;
        float targetY = 0.0f;
        float targetZ = 0.0f;
        float targetHeading = 0.0f;
    };

    /**
     * @brief Configuration per camera mode
     */
    struct CameraModeConfig
    {
        CameraMode mode = CameraMode::Chase;
        float distance = 8.0f;       ///< Distance behind vehicle (chase)
        float height = 3.0f;         ///< Height above vehicle
        float lookAhead = 5.0f;      ///< How far ahead of vehicle to look
        float smoothSpeed = 5.0f;    ///< Interpolation speed
        float fov = 75.0f;           ///< Field of view in degrees
        float shakeIntensity = 0.0f; ///< Speed-dependent camera shake
    };

    /**
     * @brief Manages camera modes with smooth transitions
     *
     * Each frame, the camera system reads the followed vehicle's position
     * and heading, computes the desired camera position for the active mode,
     * and smoothly interpolates toward it.
     */
    class RacingCameraSystem
    {
      public:
        RacingCameraSystem() = default;
        ~RacingCameraSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();

        void RenderDebugUI();

        /// Set which camera mode to use
        void SetMode(CameraMode mode);

        /// Cycle to the next camera mode
        void CycleMode();

        /// Set the vehicle to follow
        void SetTarget(float x, float y, float z, float heading, float speed);

        /// Set orbit camera angles (for mouse look)
        void SetOrbitAngles(float yaw, float pitch);

        CameraMode GetMode() const { return m_activeMode; }
        const CameraState& GetState() const { return m_state; }
        float GetCurrentFOV() const { return m_state.fov; }

        static const char* ModeToString(CameraMode mode);

      private:
        void UpdateChase(float dt);
        void UpdateCockpit(float dt);
        void UpdateHood(float dt);
        void UpdateOrbit(float dt);
        void UpdateCinematic(float dt);
        void ApplySpeedShake(float speed, float dt);
        void SmoothFollow(float targetX, float targetY, float targetZ, float speed, float dt);

        static CameraModeConfig GetDefaultConfig(CameraMode mode);

        Spark::IEngineContext* m_context{nullptr};
        CameraState m_state;
        CameraMode m_activeMode = CameraMode::Chase;
        CameraModeConfig m_configs[static_cast<size_t>(CameraMode::Count)];
        float m_transitionProgress = 1.0f; ///< 0-1 blend between old and new mode
        float m_orbitYaw = 0.0f;
        float m_orbitPitch = 20.0f;
        float m_vehicleSpeed = 0.0f;
        bool m_initialized{false};
    };

} // namespace Racing
