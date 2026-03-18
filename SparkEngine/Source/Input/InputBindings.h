/**
 * @file InputBindings.h
 * @brief Runtime input rebinding, presets, and accessibility features
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Extends the InputManager with runtime key rebinding, input presets
 * (e.g. "Default", "Left-Handed", "Accessibility"), hold-to-toggle mode,
 * and colorblind assist settings. Bindings can be serialized to/from JSON
 * for persistence between sessions.
 *
 * ## Usage
 * @code
 *   InputBindingManager bindings;
 *   bindings.SetBinding("MoveForward", InputBinding('W'));
 *   bindings.SetBinding("Jump", InputBinding(VK_SPACE));
 *   bindings.SaveToFile("Data/Config/keybinds.json");
 *
 *   bindings.SetPreset("Left-Handed");
 *   bindings.SetAccessibility(AccessibilityFlags::HoldToToggle |
 *                             AccessibilityFlags::ColorblindDeuteranopia);
 * @endcode
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cstdint>
#include <utility>

namespace Spark
{

    // =============================================================================
    // InputBinding
    // =============================================================================

    /**
 * @brief Represents a single input binding — a key or button mapped to an action.
 */
    struct InputBinding
    {
        int primaryKey = 0;        ///< Primary key code
        int alternateKey = 0;      ///< Alternate key code (0 = none)
        int gamepadButton = -1;    ///< Gamepad button index (-1 = none)
        float gamepadAxis = 0.0f;  ///< Gamepad axis index for analog actions
        bool holdToToggle = false; ///< When true, tap toggles on/off instead of hold
    };

    // =============================================================================
    // AccessibilityFlags
    // =============================================================================

    /**
 * @brief Bitmask flags for accessibility features.
 */
    enum class AccessibilityFlags : uint32_t
    {
        None = 0,
        HoldToToggle = 1 << 0,           ///< Convert all hold actions to toggle
        LargeSubtitles = 1 << 1,         ///< Increase subtitle font size
        HighContrastUI = 1 << 2,         ///< High contrast UI mode
        ColorblindProtanopia = 1 << 3,   ///< Red-green colorblind (protan)
        ColorblindDeuteranopia = 1 << 4, ///< Red-green colorblind (deutan)
        ColorblindTritanopia = 1 << 5,   ///< Blue-yellow colorblind
        ReducedMotion = 1 << 6,          ///< Reduce screen shake and motion effects
        ScreenNarrator = 1 << 7,         ///< Enable screen reader/narrator support
        AutoAim = 1 << 8,                ///< Aim assist for accessibility
        OneTouchMode = 1 << 9            ///< Simplify input to single-button actions
    };

    inline AccessibilityFlags operator|(AccessibilityFlags a, AccessibilityFlags b)
    {
        return static_cast<AccessibilityFlags>(std::to_underlying(a) | std::to_underlying(b));
    }

    inline AccessibilityFlags operator&(AccessibilityFlags a, AccessibilityFlags b)
    {
        return static_cast<AccessibilityFlags>(std::to_underlying(a) & std::to_underlying(b));
    }

    inline bool HasFlag(AccessibilityFlags flags, AccessibilityFlags flag)
    {
        return (std::to_underlying(flags) & std::to_underlying(flag)) != 0;
    }

    // =============================================================================
    // InputPreset
    // =============================================================================

    /**
 * @brief A named preset containing a full set of input bindings.
 */
    struct InputPreset
    {
        std::string name;                                       ///< Preset display name
        std::string description;                                ///< Human-readable description
        std::unordered_map<std::string, InputBinding> bindings; ///< Action → binding map
    };

    // =============================================================================
    // InputBindingManager
    // =============================================================================

    /**
 * @class InputBindingManager
 * @brief Manages input bindings, presets, and accessibility settings.
 *
 * Wraps around InputManager to provide runtime rebinding, preset management,
 * and accessibility configuration. Bindings are serialized to JSON for
 * persistence between sessions.
 */
    class InputBindingManager
    {
      public:
        InputBindingManager();
        ~InputBindingManager() = default;

        // --- Binding management ---

        /**
     * @brief Set a binding for an action.
     * @param action  Action name (e.g. "MoveForward", "Jump", "Fire").
     * @param binding The input binding configuration.
     */
        void SetBinding(const std::string& action, const InputBinding& binding);

        /**
     * @brief Get the binding for an action.
     * @param action Action name.
     * @return The current binding, or a default binding if not found.
     */
        InputBinding GetBinding(const std::string& action) const;

        /**
     * @brief Remove a binding for an action.
     * @param action Action name to unbind.
     */
        void RemoveBinding(const std::string& action);

        /**
     * @brief Get all current bindings.
     * @return Map of action name → InputBinding.
     */
        const std::unordered_map<std::string, InputBinding>& GetAllBindings() const { return m_bindings; }

        /**
     * @brief Check if an action has a conflicting binding with another.
     * @param action Action name to check.
     * @return Name of the conflicting action, or empty string if none.
     */
        std::string FindConflict(const std::string& action) const;

        // --- Presets ---

        /**
     * @brief Register a binding preset.
     * @param preset The preset to register.
     */
        void RegisterPreset(const InputPreset& preset);

        /**
     * @brief Apply a named preset, replacing all current bindings.
     * @param presetName Name of a registered preset.
     * @return true if the preset was found and applied.
     */
        bool ApplyPreset(const std::string& presetName);

        /**
     * @brief Get a list of all registered preset names.
     */
        std::vector<std::string> GetPresetNames() const;

        /**
     * @brief Create default FPS presets (Default, Left-Handed, Accessibility).
     */
        void CreateDefaultPresets();

        // --- Accessibility ---

        /**
     * @brief Set accessibility flags.
     * @param flags Bitmask of AccessibilityFlags.
     */
        void SetAccessibility(AccessibilityFlags flags) { m_accessibilityFlags = flags; }

        /** @brief Get current accessibility flags. */
        AccessibilityFlags GetAccessibility() const { return m_accessibilityFlags; }

        /** @brief Check if a specific accessibility feature is enabled. */
        bool IsAccessibilityEnabled(AccessibilityFlags flag) const { return HasFlag(m_accessibilityFlags, flag); }

        // --- Serialization ---

        /**
     * @brief Save current bindings and settings to a JSON file.
     * @param filePath Output file path.
     * @return true if saving succeeded.
     */
        bool SaveToFile(const std::string& filePath) const;

        /**
     * @brief Load bindings and settings from a JSON file.
     * @param filePath Input file path.
     * @return true if loading succeeded.
     */
        bool LoadFromFile(const std::string& filePath);

        // --- Rebinding callbacks ---

        /**
     * @brief Register a callback invoked when any binding changes.
     * @param callback Function called with action name and new binding.
     */
        void OnBindingChanged(std::function<void(const std::string&, const InputBinding&)> callback);

        // --- Console integration ---

        /** @brief List all bindings (console integration). */
        std::string Console_ListBindings() const;

        /** @brief Get accessibility status (console integration). */
        std::string Console_GetAccessibilityStatus() const;

      private:
        std::unordered_map<std::string, InputBinding> m_bindings;
        std::vector<InputPreset> m_presets;
        AccessibilityFlags m_accessibilityFlags = AccessibilityFlags::None;
        std::vector<std::function<void(const std::string&, const InputBinding&)>> m_bindingChangedCallbacks;
    };

} // namespace Spark
