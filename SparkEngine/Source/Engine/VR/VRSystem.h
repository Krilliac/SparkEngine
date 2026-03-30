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
#include "IVRBackend.h"

#include <atomic>

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <utility>

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
        bool connected = false;                     ///< Whether this controller is tracked and active.
        DirectX::XMFLOAT3 position{0, 0, 0};        ///< World-space position (meters, tracking origin).
        DirectX::XMFLOAT4 orientation{0, 0, 0, 1};  ///< Orientation quaternion (x, y, z, w).
        DirectX::XMFLOAT3 velocity{0, 0, 0};        ///< Linear velocity (m/s) — used for throw physics.
        DirectX::XMFLOAT3 angularVelocity{0, 0, 0}; ///< Angular velocity (rad/s) — used for throw spin.

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
    class VRSystem : public IVRBackend
    {
      public:
        VRSystem();
        ~VRSystem();

        /**
     * @brief Initialize the VR system (connect to VR runtime).
     * @return true if VR hardware was found and initialized.
     */
        bool Initialize() override;

        /** @brief Shutdown and release VR resources. */
        void Shutdown() override;

        /** @brief Check if VR is available and initialized. */
        bool IsAvailable() const override { return m_initialized; }

        /**
     * @brief Update tracking data (call once per frame before rendering).
     */
        void UpdateTracking() override;

        // --- Head tracking ---

        /** @brief Get head position in world space. */
        DirectX::XMFLOAT3 GetHeadPosition() const override { return m_headPosition; }

        /** @brief Get head orientation as quaternion. */
        DirectX::XMFLOAT4 GetHeadOrientation() const override { return m_headOrientation; }

        // --- Eye rendering ---

        /** @brief Get per-eye rendering data. */
        const VREye& GetLeftEye() const override { return m_leftEye; }
        const VREye& GetRightEye() const override { return m_rightEye; }

        /**
         * @brief Get the recommended render target size per eye.
         * @return Pair of {width, height}
         */
        std::pair<int, int> GetRecommendedRenderSize() const override;

        // --- Controllers ---

        /** @brief Get left controller state. */
        const VRController& GetLeftController() const override { return m_leftController; }

        /** @brief Get right controller state. */
        const VRController& GetRightController() const override { return m_rightController; }

        /**
     * @brief Trigger haptic feedback on a controller.
     * @param isLeft    true = left, false = right.
     * @param amplitude Vibration amplitude (0-1).
     * @param duration  Duration in seconds.
     */
        void TriggerHaptic(bool isLeft, float amplitude, float duration) override;

        // --- Tracking space ---

        /** @brief Set the tracking space type. */
        void SetTrackingSpace(VRTrackingSpace space) override { m_trackingSpace = space; }

        /** @brief Get the current tracking space. */
        VRTrackingSpace GetTrackingSpace() const override { return m_trackingSpace; }

        /** @brief Reset the seated position (re-center). */
        void RecenterTracking() override;

        // --- Console integration ---

        /** @brief Get VR system status (console integration). */
        std::string Console_GetStatus() const;

      private:
        std::atomic<bool> m_initialized{false};                       ///< Whether VR hardware is connected and ready.
        DirectX::XMFLOAT3 m_headPosition{0, 0, 0};                    ///< HMD position in tracking space (meters).
        DirectX::XMFLOAT4 m_headOrientation{0, 0, 0, 1};              ///< HMD orientation quaternion (x, y, z, w).
        VREye m_leftEye;                                              ///< Left eye view/projection data.
        VREye m_rightEye;                                             ///< Right eye view/projection data.
        VRController m_leftController;                                ///< Left motion controller state.
        VRController m_rightController;                               ///< Right motion controller state.
        VRTrackingSpace m_trackingSpace = VRTrackingSpace::RoomScale; ///< Active tracking space mode.
        int m_recommendedWidth = 1440;                                ///< HMD-recommended render target width per eye.
        int m_recommendedHeight = 1600;                               ///< HMD-recommended render target height per eye.
    };

} // namespace Spark::VR
