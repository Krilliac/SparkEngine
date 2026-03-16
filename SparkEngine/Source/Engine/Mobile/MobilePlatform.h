/**
 * @file MobilePlatform.h
 * @brief Mobile platform abstraction for touch input and GPU optimization
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides the framework for mobile (iOS/Android) platform support:
 * - Touch input handling (tap, swipe, pinch, multi-touch)
 * - Mobile GPU quality presets (low, medium, high)
 * - Battery-aware performance scaling
 * - Screen orientation and safe area management
 *
 * Enabled via CMake toggle: `ENABLE_MOBILE=ON`
 */

#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace Spark::Mobile
{

    // =============================================================================
    // TouchEvent
    // =============================================================================

    /**
 * @brief Types of touch events.
 */
    enum class TouchEventType
    {
        Began,
        Moved,
        Ended,
        Cancelled
    };

    /**
 * @brief A single touch point event.
 */
    struct TouchEvent
    {
        uint32_t touchId = 0; ///< Unique touch identifier
        TouchEventType type = TouchEventType::Began;
        float x = 0.0f;         ///< Screen X (0-1 normalized)
        float y = 0.0f;         ///< Screen Y (0-1 normalized)
        float pressure = 1.0f;  ///< Touch pressure (0-1)
        float timestamp = 0.0f; ///< Event timestamp
    };

    // =============================================================================
    // GestureType
    // =============================================================================

    /**
 * @brief Recognized gesture types.
 */
    enum class GestureType
    {
        Tap,
        DoubleTap,
        LongPress,
        Swipe,
        Pinch,
        Rotate
    };

    /**
 * @brief A recognized touch gesture.
 */
    struct Gesture
    {
        GestureType type = GestureType::Tap;
        float x = 0.0f, y = 0.0f;           ///< Gesture center
        float deltaX = 0.0f, deltaY = 0.0f; ///< Movement delta (swipe)
        float scale = 1.0f;                 ///< Scale factor (pinch)
        float rotation = 0.0f;              ///< Rotation angle (rotate gesture)
        int touchCount = 1;                 ///< Number of fingers
    };

    // =============================================================================
    // MobileQualityPreset
    // =============================================================================

    /**
 * @brief GPU quality presets for mobile performance.
 */
    enum class MobileQualityPreset
    {
        Low,    ///< Lowest quality, maximum performance
        Medium, ///< Balanced quality and performance
        High,   ///< Highest quality for flagship devices
        Auto    ///< Auto-detect based on device capabilities
    };

    /**
 * @brief Quality settings derived from preset.
 */
    struct MobileQualitySettings
    {
        float renderScale = 1.0f;   ///< Render resolution scale (0.5 - 1.0)
        int shadowResolution = 512; ///< Shadow map resolution
        bool enablePostProcessing = true;
        bool enableParticles = true;
        int maxDrawCalls = 500;
        int textureQuality = 2; ///< 0=low, 1=medium, 2=high
        bool enableSSAO = false;
        float lodBias = 0.0f; ///< Positive = lower quality LOD sooner
    };

    // =============================================================================
    // ScreenOrientation
    // =============================================================================

    enum class ScreenOrientation
    {
        Portrait,
        PortraitUpsideDown,
        LandscapeLeft,
        LandscapeRight,
        Auto
    };

    // =============================================================================
    // SafeArea
    // =============================================================================

    /**
 * @brief Screen safe area (avoiding notches, home indicators).
 */
    struct SafeArea
    {
        float top = 0.0f;
        float bottom = 0.0f;
        float left = 0.0f;
        float right = 0.0f;
    };

    // =============================================================================
    // MobilePlatform
    // =============================================================================

    /**
 * @class MobilePlatform
 * @brief Manages mobile-specific input, quality, and platform features.
 */
    class MobilePlatform
    {
      public:
        MobilePlatform();
        ~MobilePlatform() = default;

        /** @brief Initialize mobile platform support. */
        bool Initialize();

        /** @brief Update touch input and gestures. */
        void Update(float deltaTime);

        // --- Touch input ---

        /**
     * @brief Process a raw touch event from the OS.
     * @param event The touch event.
     */
        void ProcessTouchEvent(const TouchEvent& event);

        /** @brief Get active touch points. */
        std::vector<TouchEvent> GetActiveTouches() const;

        /** @brief Get recognized gestures since last frame. */
        std::vector<Gesture> GetGestures() const;

        /** @brief Register a gesture callback. */
        void OnGesture(GestureType type, std::function<void(const Gesture&)> callback);

        // --- Quality ---

        /**
     * @brief Set quality preset.
     * @param preset Quality level.
     */
        void SetQualityPreset(MobileQualityPreset preset);

        /** @brief Get current quality settings. */
        const MobileQualitySettings& GetQualitySettings() const { return m_qualitySettings; }

        /** @brief Get current quality preset. */
        MobileQualityPreset GetQualityPreset() const { return m_qualityPreset; }

        // --- Battery ---

        /** @brief Get battery level (0.0 - 1.0, -1 if unknown). */
        float GetBatteryLevel() const { return m_batteryLevel; }

        /** @brief Check if device is charging. */
        bool IsCharging() const { return m_isCharging; }

        /**
     * @brief Enable battery-aware performance scaling.
     *
     * When enabled, quality is automatically reduced as battery drops below
     * configurable thresholds to extend play time.
     */
        void SetBatteryAwareScaling(bool enabled) { m_batteryAwareScaling = enabled; }

        // --- Screen ---

        /** @brief Set preferred screen orientation. */
        void SetOrientation(ScreenOrientation orientation);

        /** @brief Get current screen orientation. */
        ScreenOrientation GetOrientation() const { return m_orientation; }

        /** @brief Get the safe area insets. */
        const SafeArea& GetSafeArea() const { return m_safeArea; }

        // --- Console integration ---

        /** @brief Get mobile platform status (console integration). */
        std::string Console_GetStatus() const;

      private:
        void RecognizeGestures();
        MobileQualitySettings GetSettingsForPreset(MobileQualityPreset preset) const;

        std::vector<TouchEvent> m_activeTouches;
        std::vector<Gesture> m_pendingGestures;
        std::unordered_map<int, std::vector<std::function<void(const Gesture&)>>> m_gestureCallbacks;

        MobileQualityPreset m_qualityPreset = MobileQualityPreset::Auto;
        MobileQualitySettings m_qualitySettings;
        ScreenOrientation m_orientation = ScreenOrientation::Auto;
        SafeArea m_safeArea;

        float m_batteryLevel = -1.0f;
        bool m_isCharging = false;
        bool m_batteryAwareScaling = true;
        std::atomic<bool> m_initialized{false};
    };

} // namespace Spark::Mobile
