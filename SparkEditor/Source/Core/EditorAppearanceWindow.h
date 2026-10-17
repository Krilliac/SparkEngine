/**
 * @file EditorAppearanceWindow.h
 * @brief View > Appearance: palette, appearance, density, UI scale and accessibility settings.
 *
 * The window edits an EditorPreferences value owned by EditorUI and reports every change, so the caller can
 * apply it (ApplyPreferences) and persist it. It never reads or writes the preferences file itself, which keeps
 * it usable from the test binary and keeps test-mode launches away from the user's settings.
 *
 * Contract: game thread only; acts on the current ImGui context and the EditorTheme statics. EditorUI owns the
 * single instance by value. ThemeCustomizer::ShowThemeEditor() routes here through RequestOpen(), a process-wide
 * flag the instance consumes on its next Draw(). The contrast audit allocates only when the theme revision
 * changes; drawing allocates nothing.
 */

#pragma once

#include "EditorPreferences.h"

#include "SparkUI/Tokens.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace SparkEditor
{

    /**
     * @brief The Appearance settings window.
     */
    class EditorAppearanceWindow
    {
      public:
        /// Palette families the window offers; the legacy light/high-contrast palettes are appearances.
        static constexpr std::string_view kPaletteFamilies[] = {"Ember Studio", "Cobalt Forge", "Graphite Signal"};

        /**
         * @brief Ask the window to open on the next Draw(), from code that cannot reach EditorUI.
         */
        static void RequestOpen();

        /// @brief Show the window.
        void Open() { m_open = true; }
        /// @brief Hide the window.
        void Close() { m_open = false; }
        /// @brief True while the window is shown.
        bool IsOpen() const { return m_open; }

        /**
         * @brief Draw the window when open (or when RequestOpen() was called).
         * @param prefs  Preferences edited in place.
         * @return true when @p prefs changed this frame; the caller applies and saves them.
         */
        bool Draw(EditorPreferences& prefs);

        /**
         * @brief Push @p prefs onto the live editor: appearance override, density, palette, UI scale, focus
         *        visibility and reduced motion. Scale changes land at the start of the next frame.
         */
        static void ApplyPreferences(const EditorPreferences& prefs);

        /**
         * @brief Appearance override for a stored appearance string; "dark" (and anything unknown) keeps the
         *        palette's own appearance.
         */
        static std::optional<Spark::Ui::Appearance> AppearanceOverrideFor(std::string_view appearance);

        /**
         * @brief Appearance the operating system asks for: high contrast first, then the app light/dark setting.
         *        Non-Windows builds have no portable query and report Dark.
         */
        static Spark::Ui::Appearance SystemAppearance();

        /**
         * @brief Next user scale one step (12.5 %) up or down from @p current, clamped to the preference range.
         * @param current    Stored user scale; 0 (Auto) counts as 100 %.
         * @param direction  Positive to grow, negative to shrink.
         */
        static float StepUserScale(float current, int direction);

        /**
         * @brief Move legacy palette names onto a family plus appearance ("Professional Light" -> Ember Studio,
         *        light; "High Contrast" -> Ember Studio, hc). Other names are left alone.
         */
        static void NormalizePalette(EditorPreferences& prefs);

      private:
        bool DrawPaletteSection(EditorPreferences& prefs);
        bool DrawAppearanceSection(EditorPreferences& prefs);
        bool DrawScaleSection(EditorPreferences& prefs);
        bool DrawAccessibilitySection(EditorPreferences& prefs);
        void DrawAuditBadge();

        bool m_open = false;
        std::uint64_t m_auditRevision = 0;   ///< Theme revision the cached audit belongs to
        bool m_auditValid = false;           ///< False until the first audit ran
        std::size_t m_auditFailureCount = 0; ///< Failures in the current theme's token audit
        std::string m_auditSummary;          ///< Tooltip text naming the first failures
    };

} // namespace SparkEditor
