/**
 * @file PlatformerCameraSystem.h
 * @brief Third-person follow camera for 3D platformer
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a platformer-appropriate camera system:
 * - Third-person view behind and above the player
 * - Smooth follow with adjustable lag
 * - Look-ahead in the movement direction
 * - Vertical tracking for jumps with dead zone
 * - Camera collision with level geometry (zoom in when blocked)
 * - Special camera zones (fixed angle, cinematic triggers)
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Player/PlatformerPlayerController.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Platformer
{

    /// @brief Camera zone override (fixed-angle areas, cinematic triggers)
    struct CameraZone
    {
        uint32_t id = 0;
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float width = 10.0f;
        float height = 10.0f;
        float depth = 10.0f;

        // Override camera parameters while player is in this zone
        float overrideOffsetX = 0.0f;
        float overrideOffsetY = 8.0f;
        float overrideOffsetZ = -12.0f;
        float overrideLookAheadX = 0.0f;
        bool isFixedAngle = false;
    };

    /**
     * @brief Third-person follow camera with smooth tracking and collision
     *
     * The camera follows the player with configurable offset, smoothing,
     * and look-ahead. A vertical dead zone prevents jitter during small
     * jumps. Camera collision prevents clipping through level geometry
     * by zooming closer when obstructed.
     */
    class PlatformerCameraSystem
    {
      public:
        PlatformerCameraSystem() = default;
        ~PlatformerCameraSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime, PlayerPosition targetPosition);
        void Shutdown();

        void RenderDebugUI();

        /// @brief Get current camera world position
        PlayerPosition GetCameraPosition() const { return m_cameraPosition; }

        /// @brief Get the point the camera is looking at
        PlayerPosition GetLookTarget() const { return m_lookTarget; }

      private:
        void SmoothFollow(float deltaTime, const PlayerPosition& target);
        void ApplyLookAhead(float deltaTime, const PlayerPosition& target);
        void ApplyVerticalTracking(float deltaTime, float targetY);
        void CheckCollision();
        void CheckCameraZones(float playerX, float playerY, float playerZ);
        void BuildDemoCameraZones();

        Spark::IEngineContext* m_context{nullptr};

        // Camera state
        PlayerPosition m_cameraPosition{0.0f, 8.0f, -12.0f};
        PlayerPosition m_lookTarget{0.0f, 2.0f, 0.0f};

        // Follow offset (relative to player)
        float m_offsetX{0.0f};
        float m_offsetY{8.0f};
        float m_offsetZ{-12.0f};

        // Smooth follow parameters
        float m_followSmoothX{5.0f}; ///< Horizontal follow speed
        float m_followSmoothY{3.0f}; ///< Vertical follow speed
        float m_followSmoothZ{5.0f}; ///< Depth follow speed

        // Look-ahead
        float m_lookAheadDistance{4.0f};
        float m_lookAheadSmooth{3.0f};
        float m_currentLookAheadX{0.0f};
        float m_previousTargetX{0.0f};

        // Vertical dead zone (don't track small vertical movements)
        float m_verticalDeadZoneMin{-2.0f};
        float m_verticalDeadZoneMax{4.0f};
        float m_currentVerticalOffset{0.0f};

        // Collision
        float m_minDistance{3.0f};  ///< Minimum distance when collision detected
        float m_maxDistance{12.0f}; ///< Normal distance (magnitude of offset)
        float m_currentDistance{12.0f};
        float m_collisionSmooth{8.0f};

        // Camera zones
        std::vector<CameraZone> m_cameraZones;
        const CameraZone* m_activeZone{nullptr};

        bool m_initialized{false};
    };

} // namespace Platformer
