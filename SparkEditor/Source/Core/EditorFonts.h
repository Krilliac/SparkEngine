/**
 * @file EditorFonts.h
 * @brief Editor font faces, loaded through the Spark UI font pipeline (Spark::Ui::LoadFontSet).
 * @author Spark Engine Team
 * @date 2025
 *
 * Thread affinity: main (ImGui) thread only. The returned ImFont pointers are owned by the ImGui
 * font atlas and stay valid until the context that loaded them is destroyed.
 */

#pragma once

#include <imgui.h>

namespace SparkEditor
{

    /**
     * @brief Editor access to the Spark faces loaded into the current ImGui font atlas.
     *
     * - Default: IBM Plex Sans Regular (Roboto fallback) with Font Awesome merged
     * - Bold: IBM Plex Sans SemiBold, for headers and emphasis
     * - Monospace: JetBrains Mono, for console, code and logs
     * - Large: IBM Plex Sans Bold, for titles; push it at Spark::Ui::FontSize(FontRole::H1) or larger
     *
     * ImGui 1.92 fonts are size-independent, so every face is loaded once and sized at push time.
     */
    class EditorFonts
    {
      public:
        /**
         * @brief Load all editor fonts into the current context's atlas. Call before the ImGui
         *        renderer backend initialises.
         * @param baseFontSize Body size in logical pixels at 100 % scale.
         * @return True when the faces came from font files, false when the embedded default font
         *         was substituted.
         */
        static bool LoadFonts(float baseFontSize = 15.0f);

        /** @brief Body face with Font Awesome glyphs merged. */
        static ImFont* GetDefault() { return s_defaultFont; }

        /** @brief Semibold face for headers and panel titles. */
        static ImFont* GetBold() { return s_boldFont ? s_boldFont : s_defaultFont; }

        /** @brief Monospace face for console, code and logs. */
        static ImFont* GetMonospace() { return s_monoFont ? s_monoFont : s_defaultFont; }

        /** @brief Bold face for section headers and titles. */
        static ImFont* GetLarge() { return s_largeFont ? s_largeFont : s_defaultFont; }

        /** @brief True when the faces came from font files rather than the embedded default. */
        static bool AreCustomFontsLoaded() { return s_customFontsLoaded; }

      private:
        static ImFont* s_defaultFont;
        static ImFont* s_boldFont;
        static ImFont* s_monoFont;
        static ImFont* s_largeFont;
        static bool s_customFontsLoaded;
    };

} // namespace SparkEditor
