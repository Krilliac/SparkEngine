/**
 * @file EditorStartPage.h
 * @brief The dockable "Start" tab: brand hero, scene and project actions, recent scenes and a shortcut sheet.
 *
 * The page replaces the old modal welcome screen. It docks into the centre node beside the Scene view, opens on
 * first run or while EditorPreferences::showStartPage is set, and never opens in test mode (EditorUI decides).
 * Draw() reports what the user chose instead of calling back into EditorUI, so the page owns no editor state.
 *
 * Contract: game thread only; draws into the current ImGui context. EditorUI owns the single instance by value.
 * Drawing allocates only the recent-scene labels shortened to fit (at most ten small strings per frame, from
 * EditorPreferences' cap) and the returned recent-scene path when one is chosen. The hero banner's paint and
 * text roles come from Spark::Ui::ChooseHeroBannerStyle, so High Contrast gets a flat outlined banner and the
 * version line keeps measured contrast in every appearance.
 */

#pragma once

#include "EditorPreferences.h"

#include <cstdint>
#include <string>

namespace SparkEditor
{

    /**
     * @brief What the user picked on the Start page this frame.
     */
    enum class StartPageAction : std::uint8_t
    {
        None,
        OpenScene,         ///< "Open Scene..." (file dialog)
        NewScene,          ///< "New Scene"
        OpenProjectFolder, ///< "Open Project Folder" (project browser)
        ResetLayout,       ///< "Reset workspace layout"
        OpenRecentScene    ///< A recent-scene entry; the path is in StartPageResult::recentScene
    };

    /**
     * @brief Outcome of one EditorStartPage::Draw() call.
     */
    struct StartPageResult
    {
        StartPageAction action = StartPageAction::None;
        std::string recentScene;         ///< Set for StartPageAction::OpenRecentScene
        bool preferencesChanged = false; ///< "Show at startup" was toggled; the caller saves preferences
    };

    /**
     * @brief The Start tab.
     */
    class EditorStartPage
    {
      public:
        /// ImGui window name; the visible tab title is "Start".
        static constexpr const char* kWindowName = "Start###SparkStartPage";

        /// @brief Show the page and bring its tab to the front on the next Draw().
        void Open()
        {
            m_open = true;
            m_focusRequested = true;
        }
        /// @brief Hide the page.
        void Close() { m_open = false; }
        /// @brief True while the page is shown.
        bool IsOpen() const { return m_open; }

        /**
         * @brief Draw the page when open. Esc while the page has focus closes it.
         * @param prefs         Reads recentScenes; the "Show at startup" checkbox edits showStartPage.
         * @param centreDockId  Dock node the page joins the first time it appears (0 = leave it where ImGui
         *                      remembers it, or floating).
         */
        StartPageResult Draw(EditorPreferences& prefs, unsigned int centreDockId = 0);

      private:
        void DrawHero();
        StartPageAction DrawActions();
        bool DrawRecentScenes(const EditorPreferences& prefs, std::string& chosen);
        void DrawShortcutSheet();

        bool m_open = false;
        bool m_focusRequested = false;
    };

} // namespace SparkEditor
