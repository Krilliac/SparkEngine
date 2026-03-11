/**
 * @file VRSystem.h
 * @brief VR/AR integration system (OpenXR-ready framework)
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides the framework for VR and AR integration. Currently a framework
 * stub — actual OpenXR initialization requires the OpenXR SDK and a VR
 * runtime (SteamVR, Oculus, WMR). The system provides:
 *
 * - Head tracking (position + orientation)
 * - Stereoscopic rendering (left/right eye viewports)
 * - Motion controller input (position, orientation, buttons, triggers)
 * - Haptic feedback
 * - Room-scale tracking space configuration
 *
 * Enabled via CMake toggle: `ENABLE_VR=ON`
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace Spark::VR
{

    // =============================================================================
    // VRController
    // =============================================================================

    /**
 * @brief State of a single VR motion controller.
 */
    struct VRController
    {
        bool connected = false;
        DirectX::XMFLOAT3 position{0, 0, 0};
        DirectX::XMFLOAT4 orientation{0, 0, 0, 1};
        DirectX::XMFLOAT3 velocity{0, 0, 0};
        DirectX::XMFLOAT3 angularVelocity{0, 0, 0};

        float triggerValue = 0.0f;          ///< Trigger axis (0-1)
        float gripValue = 0.0f;             ///< Grip axis (0-1)
        DirectX::XMFLOAT2 thumbstick{0, 0}; ///< Thumbstick x,y (-1 to 1)
        uint32_t buttonMask = 0;            ///< Bitmask of pressed buttons
    };

    // =============================================================================
    // VREye
    // =============================================================================

    /**
 * @brief Per-eye rendering data.
 */
    struct VREye
    {
        DirectX::XMFLOAT4X4 viewMatrix;       ///< Eye view matrix
        DirectX::XMFLOAT4X4 projectionMatrix; ///< Eye projection matrix
        DirectX::XMFLOAT3 position{0, 0, 0};  ///< Eye world position
        int viewportWidth = 0;                ///< Render target width per eye
        int viewportHeight = 0;               ///< Render target height per eye
    };

    // =============================================================================
    // VRTrackingSpace
    // =============================================================================

    /**
 * @brief Tracking space configuration.
 */
    enum class VRTrackingSpace
    {
        Seated,   ///< Seated/standing (small area)
        RoomScale ///< Room-scale (walk around)
    };

    // =============================================================================
    // VRSystem
    // =============================================================================

    /**
 * @class VRSystem
 * @brief Manages VR hardware initialization, tracking, and rendering.
 */
    class VRSystem
    {
      public:
        VRSystem();
        ~VRSystem();

        /**
     * @brief Initialize the VR system (connect to VR runtime).
     * @return true if VR hardware was found and initialized.
     */
        bool Initialize();

        /** @brief Shutdown and release VR resources. */
        void Shutdown();

        /** @brief Check if VR is available and initialized. */
        bool IsAvailable() const { return m_initialized; }

        /**
     * @brief Update tracking data (call once per frame before rendering).
     */
        void UpdateTracking();

        // --- Head tracking ---

        /** @brief Get head position in world space. */
        DirectX::XMFLOAT3 GetHeadPosition() const { return m_headPosition; }

        /** @brief Get head orientation as quaternion. */
        DirectX::XMFLOAT4 GetHeadOrientation() const { return m_headOrientation; }

        // --- Eye rendering ---

        /** @brief Get per-eye rendering data. */
        const VREye& GetLeftEye() const { return m_leftEye; }
        const VREye& GetRightEye() const { return m_rightEye; }

        /** @brief Get the recommended render target size per eye. */
        void GetRecommendedRenderSize(int& width, int& height) const;

        // --- Controllers ---

        /** @brief Get left controller state. */
        const VRController& GetLeftController() const { return m_leftController; }

        /** @brief Get right controller state. */
        const VRController& GetRightController() const { return m_rightController; }

        /**
     * @brief Trigger haptic feedback on a controller.
     * @param isLeft    true = left, false = right.
     * @param amplitude Vibration amplitude (0-1).
     * @param duration  Duration in seconds.
     */
        void TriggerHaptic(bool isLeft, float amplitude, float duration);

        // --- Tracking space ---

        /** @brief Set the tracking space type. */
        void SetTrackingSpace(VRTrackingSpace space) { m_trackingSpace = space; }

        /** @brief Get the current tracking space. */
        VRTrackingSpace GetTrackingSpace() const { return m_trackingSpace; }

        /** @brief Reset the seated position (re-center). */
        void RecenterTracking();

        // --- Console integration ---

        /** @brief Get VR system status (console integration). */
        std::string Console_GetStatus() const;

      private:
        bool m_initialized = false;
        DirectX::XMFLOAT3 m_headPosition{0, 0, 0};
        DirectX::XMFLOAT4 m_headOrientation{0, 0, 0, 1};
        VREye m_leftEye;
        VREye m_rightEye;
        VRController m_leftController;
        VRController m_rightController;
        VRTrackingSpace m_trackingSpace = VRTrackingSpace::RoomScale;
        int m_recommendedWidth = 1440;
        int m_recommendedHeight = 1600;
    };

} // namespace Spark::VR
