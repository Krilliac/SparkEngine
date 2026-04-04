/**
 * @file AccessibilitySystem.h
 * @brief Engine-wide accessibility framework with colorblind filters, subtitles, and adaptive UI
 * @author Spark Engine Team
 * @date 2026
 *
 * @details
 * Provides runtime accessibility features including:
 * - Colorblind simulation/correction (daltonization matrices for 5 modes)
 * - Subtitle rendering with speaker tags and timed display
 * - High-contrast UI, large text, screen reader hooks
 * - Reduced motion, one-handed input mode
 * - Feature toggle system for extensibility
 *
 * @code
 *   auto& access = Spark::Accessibility::AccessibilitySystem::GetInstance();
 *   access.Initialize();
 *   AccessibilitySettings s = access.GetSettings();
 *   s.colorblindMode = ColorblindMode::Deuteranopia;
 *   access.SetSettings(s);
 * @endcode
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Spark::Accessibility
{

    // ============================================================================
    // Enums
    // ============================================================================

    /**
     * @brief Colorblind simulation/correction mode.
     */
    enum class ColorblindMode : uint8_t
    {
        None = 0,     ///< No correction applied
        Protanopia,   ///< Red-blind (L-cone deficiency)
        Deuteranopia, ///< Green-blind (M-cone deficiency)
        Tritanopia,   ///< Blue-blind (S-cone deficiency)
        Achromatopsia ///< Total color blindness (monochromacy)
    };

    /**
     * @brief One-handed input mode selection.
     */
    enum class OneHandedMode : uint8_t
    {
        None = 0,
        LeftHanded,
        RightHanded
    };

    // ============================================================================
    // Settings
    // ============================================================================

    /**
     * @brief All user-facing accessibility preferences.
     */
    struct AccessibilitySettings
    {
        ColorblindMode colorblindMode = ColorblindMode::None;
        float colorblindStrength = 1.0f;        ///< Correction intensity (0 = off, 1 = full)
        bool highContrastUI = false;            ///< High-contrast editor/game UI
        bool largeText = false;                 ///< Force larger default font size
        float textScaleFactor = 1.0f;           ///< Global text scale multiplier
        bool subtitlesEnabled = false;          ///< Show subtitles for dialogue/audio
        float subtitleBackgroundOpacity = 0.7f; ///< Subtitle background alpha (0-1)
        bool screenReaderEnabled = false;       ///< Emit text descriptions for UI elements
        bool reducedMotion = false;             ///< Disable screen shake, reduce particles
        bool inputRemappingEnabled = false;     ///< Allow runtime key remapping
        OneHandedMode oneHandedMode = OneHandedMode::None;
    };

    // ============================================================================
    // SubtitleEntry
    // ============================================================================

    /**
     * @brief A single subtitle to display on screen.
     */
    struct SubtitleEntry
    {
        std::string text;            ///< The subtitle text content
        std::string speaker;         ///< Speaker name/tag (empty if narration)
        float duration = 3.0f;       ///< Display duration in seconds
        float fontSize = 18.0f;      ///< Font size in points
        uint32_t color = 0xFFFFFFFF; ///< RGBA packed color
        float remainingTime = 0.0f;  ///< Internal: time left before removal
    };

    // ============================================================================
    // AccessibilitySystem
    // ============================================================================

    /**
     * @class AccessibilitySystem
     * @brief Singleton managing all engine accessibility features.
     */
    class AccessibilitySystem
    {
      public:
        /** @brief Get the singleton instance. */
        static AccessibilitySystem& GetInstance()
        {
            static AccessibilitySystem instance;
            return instance;
        }

        AccessibilitySystem(const AccessibilitySystem&) = delete;
        AccessibilitySystem& operator=(const AccessibilitySystem&) = delete;

        // --- Lifecycle ---

        /** @brief Initialize the accessibility system. */
        void Initialize()
        {
            m_settings = AccessibilitySettings{};
            m_activeSubtitles.clear();
            m_featureFlags.clear();
            BuildColorMatrices();
            m_initialized = true;
        }

        /** @brief Shut down and reset state. */
        void Shutdown()
        {
            m_activeSubtitles.clear();
            m_featureFlags.clear();
            m_initialized = false;
        }

        /**
         * @brief Per-frame update (expires subtitles).
         * @param dt Delta time in seconds.
         */
        void Update(float dt)
        {
            if (!m_initialized)
                return;

            // Tick subtitle timers and remove expired entries
            for (auto& sub : m_activeSubtitles)
            {
                sub.remainingTime -= dt;
            }
            std::erase_if(m_activeSubtitles, [](const SubtitleEntry& s) { return s.remainingTime <= 0.0f; });
        }

        // --- Settings ---

        /** @brief Get current accessibility settings (read-only). */
        const AccessibilitySettings& GetSettings() const { return m_settings; }

        /** @brief Apply new accessibility settings. */
        void SetSettings(const AccessibilitySettings& settings) { m_settings = settings; }

        // --- Colorblind correction ---

        /**
         * @brief Apply the current colorblind daltonization filter to an RGB triplet.
         * @param rgbOut Output corrected RGB (3 floats, 0-1 range).
         * @param rgbIn  Input RGB (3 floats, 0-1 range).
         */
        void ApplyColorblindFilter(float* rgbOut, const float* rgbIn) const
        {
            if (m_settings.colorblindMode == ColorblindMode::None || m_settings.colorblindStrength <= 0.0f)
            {
                rgbOut[0] = rgbIn[0];
                rgbOut[1] = rgbIn[1];
                rgbOut[2] = rgbIn[2];
                return;
            }

            auto mat = GetColorblindCorrectionMatrix(m_settings.colorblindMode);
            float strength = m_settings.colorblindStrength;

            // Matrix multiply then lerp with original based on strength
            for (int row = 0; row < 3; ++row)
            {
                float corrected = mat[static_cast<size_t>(row * 3 + 0)] * rgbIn[0] +
                                  mat[static_cast<size_t>(row * 3 + 1)] * rgbIn[1] +
                                  mat[static_cast<size_t>(row * 3 + 2)] * rgbIn[2];
                rgbOut[row] = rgbIn[row] + (corrected - rgbIn[row]) * strength;
            }
        }

        /**
         * @brief Get the 3x3 daltonization correction matrix for a given mode.
         * @param mode The colorblind mode.
         * @return Row-major 3x3 matrix (9 floats).
         */
        static std::array<float, 9> GetColorblindCorrectionMatrix(ColorblindMode mode)
        {
            // Identity matrix (no correction)
            constexpr std::array<float, 9> kIdentity = {1, 0, 0, 0, 1, 0, 0, 0, 1};

            switch (mode)
            {
            case ColorblindMode::Protanopia:
                // Daltonization matrix for protanopia (red-blind)
                return {0.567f, 0.433f, 0.000f, 0.558f, 0.442f, 0.000f, 0.000f, 0.242f, 0.758f};
            case ColorblindMode::Deuteranopia:
                // Daltonization matrix for deuteranopia (green-blind)
                return {0.625f, 0.375f, 0.000f, 0.700f, 0.300f, 0.000f, 0.000f, 0.300f, 0.700f};
            case ColorblindMode::Tritanopia:
                // Daltonization matrix for tritanopia (blue-blind)
                return {0.950f, 0.050f, 0.000f, 0.000f, 0.433f, 0.567f, 0.000f, 0.475f, 0.525f};
            case ColorblindMode::Achromatopsia:
                // Luminance-preserving grayscale (Rec. 709)
                return {0.2126f, 0.7152f, 0.0722f, 0.2126f, 0.7152f, 0.0722f, 0.2126f, 0.7152f, 0.0722f};
            default:
                return kIdentity;
            }
        }

        // --- Subtitles ---

        /**
         * @brief Push a new subtitle for display.
         * @param entry The subtitle entry (duration sets how long it shows).
         */
        void PushSubtitle(const SubtitleEntry& entry)
        {
            if (!m_settings.subtitlesEnabled)
                return;

            SubtitleEntry copy = entry;
            copy.remainingTime = entry.duration;
            copy.fontSize *= m_settings.textScaleFactor;
            m_activeSubtitles.push_back(std::move(copy));
        }

        /** @brief Get all currently visible subtitles. */
        const std::vector<SubtitleEntry>& GetActiveSubtitles() const { return m_activeSubtitles; }

        // --- Feature flags ---

        /**
         * @brief Check if a named accessibility feature is enabled.
         * @param feature Feature name (e.g. "screen_magnifier").
         */
        bool IsFeatureEnabled(std::string_view feature) const
        {
            auto it = m_featureFlags.find(std::string(feature));
            return it != m_featureFlags.end() && it->second;
        }

        /**
         * @brief Enable or disable a named accessibility feature.
         * @param feature Feature name.
         * @param enabled Whether to enable.
         */
        void SetFeatureEnabled(std::string_view feature, bool enabled)
        {
            m_featureFlags[std::string(feature)] = enabled;
        }

        // --- Console ---

        /** @brief Get accessibility system status (console integration). */
        std::string Console_GetStatus() const
        {
            std::string status = "AccessibilitySystem: ";
            status += m_initialized ? "initialized" : "not initialized";
            status += " | Colorblind: ";

            switch (m_settings.colorblindMode)
            {
            case ColorblindMode::None:
                status += "None";
                break;
            case ColorblindMode::Protanopia:
                status += "Protanopia";
                break;
            case ColorblindMode::Deuteranopia:
                status += "Deuteranopia";
                break;
            case ColorblindMode::Tritanopia:
                status += "Tritanopia";
                break;
            case ColorblindMode::Achromatopsia:
                status += "Achromatopsia";
                break;
            }

            status += " | Subtitles: ";
            status += m_settings.subtitlesEnabled ? "on" : "off";
            status += " (active: " + std::to_string(m_activeSubtitles.size()) + ")";
            status += " | HighContrast: ";
            status += m_settings.highContrastUI ? "on" : "off";
            status += " | ReducedMotion: ";
            status += m_settings.reducedMotion ? "on" : "off";
            status += " | TextScale: " + std::to_string(m_settings.textScaleFactor);
            return status;
        }

      private:
        AccessibilitySystem() = default;
        ~AccessibilitySystem() = default;

        /** @brief Pre-build correction matrices (called during Initialize). */
        void BuildColorMatrices()
        {
            m_colorMatrices[0] = GetColorblindCorrectionMatrix(ColorblindMode::None);
            m_colorMatrices[1] = GetColorblindCorrectionMatrix(ColorblindMode::Protanopia);
            m_colorMatrices[2] = GetColorblindCorrectionMatrix(ColorblindMode::Deuteranopia);
            m_colorMatrices[3] = GetColorblindCorrectionMatrix(ColorblindMode::Tritanopia);
            m_colorMatrices[4] = GetColorblindCorrectionMatrix(ColorblindMode::Achromatopsia);
        }

        bool m_initialized = false;
        AccessibilitySettings m_settings;
        std::vector<SubtitleEntry> m_activeSubtitles;
        std::unordered_map<std::string, bool> m_featureFlags;
        std::array<std::array<float, 9>, 5> m_colorMatrices; ///< Cached correction matrices per mode
    };

} // namespace Spark::Accessibility
