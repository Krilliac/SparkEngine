/**
 * @file IVRBackend.h
 * @brief Abstract interface for VR/AR runtime backends
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines the interface that VR implementations (OpenXR, etc.) must implement.
 * This abstraction enables runtime-swappable VR backends through EngineContext.
 */

#pragma once

#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <string>
#include <utility>

namespace Spark::VR
{

    struct VRController;
    struct VREye;
    enum class VRTrackingSpace;

    /**
     * @brief Abstract interface for VR system backends.
     *
     * All VR operations go through this interface. Implementations manage
     * their own runtime connection (OpenXR session, etc.) and translate
     * tracking data into the engine's coordinate system.
     */
    class IVRBackend
    {
      public:
        virtual ~IVRBackend() = default;

        // ================================================================
        // Lifecycle
        // ================================================================

        /// @brief Initialize the VR system (connect to VR runtime).
        /// @return true if VR hardware was found and initialized.
        virtual bool Initialize() = 0;

        /// @brief Shutdown and release VR resources.
        virtual void Shutdown() = 0;

        /// @brief Check if VR is available and initialized.
        [[nodiscard]] virtual bool IsAvailable() const = 0;

        // ================================================================
        // Tracking
        // ================================================================

        /// @brief Update tracking data (call once per frame before rendering).
        virtual void UpdateTracking() = 0;

        /// @brief Get head position in world space.
        [[nodiscard]] virtual DirectX::XMFLOAT3 GetHeadPosition() const = 0;

        /// @brief Get head orientation as quaternion.
        [[nodiscard]] virtual DirectX::XMFLOAT4 GetHeadOrientation() const = 0;

        // ================================================================
        // Eye Rendering
        // ================================================================

        /// @brief Get left eye rendering data.
        [[nodiscard]] virtual const VREye& GetLeftEye() const = 0;

        /// @brief Get right eye rendering data.
        [[nodiscard]] virtual const VREye& GetRightEye() const = 0;

        /// @brief Get the recommended render target size per eye.
        [[nodiscard]] virtual std::pair<int, int> GetRecommendedRenderSize() const = 0;

        // ================================================================
        // Controllers
        // ================================================================

        /// @brief Get left controller state.
        [[nodiscard]] virtual const VRController& GetLeftController() const = 0;

        /// @brief Get right controller state.
        [[nodiscard]] virtual const VRController& GetRightController() const = 0;

        /// @brief Trigger haptic feedback on a controller.
        /// @param isLeft true = left, false = right.
        /// @param amplitude Vibration amplitude (0-1).
        /// @param duration Duration in seconds.
        virtual void TriggerHaptic(bool isLeft, float amplitude, float duration) = 0;

        // ================================================================
        // Tracking Space
        // ================================================================

        /// @brief Set the tracking space type.
        virtual void SetTrackingSpace(VRTrackingSpace space) = 0;

        /// @brief Get the current tracking space.
        [[nodiscard]] virtual VRTrackingSpace GetTrackingSpace() const = 0;

        /// @brief Reset the seated position (re-center).
        virtual void RecenterTracking() = 0;
    };

} // namespace Spark::VR
