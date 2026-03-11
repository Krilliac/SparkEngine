/**
 * @file CommandPalette.h
 * @brief Command palette overlay for quick action execution
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a Ctrl+P activated command palette overlay for quickly
 * accessing editor commands, toggling panels, and navigating to
 * entities and assets.
 */

#pragma once

#include <string>
#include <vector>
#include <functional>

namespace SparkEditor
{

    /**
     * @brief A registered action in the command palette
     */
    struct PaletteAction
    {
        std::string name;               ///< Display name
        std::string category;           ///< Category (e.g., "Panel", "Command", "Entity")
        std::string shortcut;           ///< Keyboard shortcut hint
        std::function<void()> callback; ///< Action to execute
    };

    /**
     * @brief Modal command palette overlay
     *
     * Renders as a centered floating input at the top of the screen.
     * Activated with Ctrl+P, supports fuzzy matching and category
     * prefix filters (e.g., ">" for commands, "@" for entities).
     */
    class CommandPalette
    {
      public:
        CommandPalette();
        ~CommandPalette() = default;

        /**
         * @brief Register an action in the palette
         * @param name Display name
         * @param category Category string
         * @param callback Action to execute
         * @param shortcut Optional keyboard shortcut hint
         */
        void RegisterAction(const std::string& name, const std::string& category, std::function<void()> callback,
                            const std::string& shortcut = "");

        /**
         * @brief Remove all registered actions
         */
        void ClearActions();

        /**
         * @brief Open the command palette
         */
        void Open();

        /**
         * @brief Close the command palette
         */
        void Close();

        /**
         * @brief Toggle the command palette open/closed
         */
        void Toggle();

        /**
         * @brief Check if the command palette is open
         * @return true if open
         */
        bool IsOpen() const { return m_isOpen; }

        /**
         * @brief Render the command palette overlay
         *
         * Should be called every frame from EditorUI::Render().
         * Only renders when the palette is open.
         */
        void Render();

      private:
        void FilterActions();
        void ExecuteSelected();
        float CalculateMatchScore(const std::string& text, const std::string& query) const;

        bool m_isOpen = false;
        bool m_focusInput = false;
        char m_inputBuffer[256] = {};
        std::string m_currentFilter;

        std::vector<PaletteAction> m_allActions;
        std::vector<size_t> m_filteredIndices; // Indices into m_allActions
        int m_selectedIndex = 0;

        // Category prefix filters
        static constexpr char PREFIX_COMMAND = '>';
        static constexpr char PREFIX_ENTITY = '@';
        static constexpr char PREFIX_FILE = '/';
    };

} // namespace SparkEditor
