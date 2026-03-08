/**
 * @file EditorFonts.h
 * @brief Font management for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <imgui.h>
#include <string>

namespace SparkEditor
{

    /**
 * @brief Centralized font manager for the editor
 *
 * Loads and provides access to themed fonts including:
 * - Default: Roboto Regular + merged FontAwesome icons
 * - Bold: Roboto Bold (for headers and emphasis)
 * - Monospace: JetBrains Mono (for console and code)
 * - Large: Roboto at larger size (for titles)
 */
    class EditorFonts
    {
      public:
        /**
     * @brief Load all editor fonts. Must be called before ImGui backends initialize fonts.
     * @param baseFontSize Base font size in pixels (default 15.0f)
     */
        static void LoadFonts(float baseFontSize = 15.0f);

        /** @brief Get the default font (Roboto Regular + FontAwesome icons merged) */
        static ImFont* GetDefault() { return s_defaultFont; }

        /** @brief Get the bold font (for headers and panel titles) */
        static ImFont* GetBold() { return s_boldFont ? s_boldFont : s_defaultFont; }

        /** @brief Get the monospace font (for console, code, and logs) */
        static ImFont* GetMonospace() { return s_monoFont ? s_monoFont : s_defaultFont; }

        /** @brief Get the large font (for section headers and titles) */
        static ImFont* GetLarge() { return s_largeFont ? s_largeFont : s_defaultFont; }

        /** @brief Check if custom fonts were loaded successfully */
        static bool AreCustomFontsLoaded() { return s_customFontsLoaded; }

      private:
        static ImFont* s_defaultFont;
        static ImFont* s_boldFont;
        static ImFont* s_monoFont;
        static ImFont* s_largeFont;
        static bool s_customFontsLoaded;
    };

} // namespace SparkEditor
