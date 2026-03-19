/**
 * @file ConsolePanel.h
 * @brief Advanced console panel for Spark Engine Editor with integrated logging
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

// Standard library headers first (MSVC /Zc:preprocessor compatibility)
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>
#include <algorithm>

// Third-party headers
#include <imgui.h> // For ImVec4, ImVec2

// Project headers
#include "../Core/EditorPanel.h"
#include "../Core/EditorLogger.h"

namespace SparkEditor
{

    /**
 * @brief Console command structure
 */
    struct ConsoleCommand
    {
        std::string name;        ///< Command name (e.g. "help", "clear").
        std::string description; ///< One-line help text for listing.
        std::string usage;       ///< Usage pattern string (e.g. "help [command]").
        std::function<std::string(const std::vector<std::string>&)> handler; ///< Callback returning a result string.
        bool isEngineCommand = false;                                        ///< True = forward to engine via pipe.
    };

    /**
 * @brief Console history entry
 */
    struct ConsoleHistoryEntry
    {
        std::string command;                             ///< The command string that was executed.
        std::string result;                              ///< Output/result returned by the handler.
        std::chrono::system_clock::time_point timestamp; ///< When the command was executed.
        bool wasSuccessful = true;                       ///< False if the command failed or was unknown.
    };

    /**
 * @brief Console filter settings
 */
    struct ConsoleFilter
    {
        LogLevel minLevel = LogLevel::TRACE;        ///< Minimum severity to display.
        std::vector<std::string> enabledCategories; ///< Whitelist of visible log categories.
        bool enableAllCategories = true;            ///< True = ignore enabledCategories and show all.
        std::string searchPattern;                  ///< Regex/substring pattern for filtering log text.
        bool showTimestamps = true;                 ///< Show timestamps in log entries.
        bool showCategories = true;                 ///< Show category tags in log entries.
        bool showThreadIds = false;                 ///< Show thread IDs in log entries.
        bool showFileInfo = false;                  ///< Show source file:line in log entries.
        bool colorCodeLevels = true;                ///< Color-code log lines by severity.
        bool autoScroll = true;                     ///< Auto-scroll to latest log entry.
        bool wordWrap = true;                       ///< Wrap long lines instead of clipping.
        int maxDisplayEntries = 1000;               ///< Max visible entries (performance cap).

        // Simplified filter UI toggles
        bool showInfo = true;    ///< Show INFO-level entries.
        bool showWarning = true; ///< Show WARNING-level entries.
        bool showError = true;   ///< Show ERROR-level entries.
        std::string searchText;  ///< Quick-search text for the filter bar.
    };

    /**
 * @brief Advanced console panel with logging integration
 * 
 * Provides a comprehensive console interface with:
 * - Real-time log display with filtering
 * - Command execution (editor and engine)
 * - Search and filtering capabilities
 * - Export/import functionality
 * - Performance monitoring
 */
    class ConsolePanel : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        ConsolePanel();

        /**
     * @brief Destructor
     */
        ~ConsolePanel();

        // EditorPanel overrides
        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Register a console command
     * @param command Command configuration
     */
        void RegisterCommand(const ConsoleCommand& command);

        /**
     * @brief Unregister a console command
     * @param commandName Name of the command to remove
     */
        void UnregisterCommand(const std::string& commandName);

        /**
     * @brief Execute a command
     * @param commandLine Command line to execute
     * @return Execution result
     */
        std::string ExecuteCommand(const std::string& commandLine);

        /**
     * @brief Add log entry to console display
     * @param entry Log entry to add
     */
        void AddLogEntry(const LogEntry& entry);

        /**
     * @brief Clear console display
     */
        void Clear();

        /**
     * @brief Set console filter
     * @param filter Filter configuration
     */
        void SetFilter(const ConsoleFilter& filter);

        /**
     * @brief Get console filter
     * @return Current filter configuration
     */
        const ConsoleFilter& GetFilter() const { return m_filter; }

        /**
     * @brief Export console log to file
     * @param filePath File path to export to
     * @param format Export format ("txt", "csv", "json")
     * @return true if export succeeded
     */
        bool ExportLog(const std::string& filePath, const std::string& format = "txt");

        /**
     * @brief Get command history
     * @return Vector of command history entries
     */
        const std::vector<ConsoleHistoryEntry>& GetCommandHistory() const { return m_commandHistory; }

        /**
     * @brief Set maximum number of log entries to keep
     * @param maxEntries Maximum entries (0 = unlimited)
     */
        void SetMaxLogEntries(size_t maxEntries) { m_maxLogEntries = maxEntries; }

        /**
     * @brief Get console statistics
     */
        struct ConsoleStats
        {
            size_t totalLogEntries = 0;                                ///< Total log entries received since startup.
            size_t visibleLogEntries = 0;                              ///< Entries currently passing the active filter.
            size_t commandsExecuted = 0;                               ///< Total commands executed (editor + engine).
            size_t engineCommandsExecuted = 0;                         ///< Commands forwarded to the engine process.
            float averageCommandTime = 0.0f;                           ///< Mean command execution time (ms).
            std::chrono::system_clock::time_point lastActivity;        ///< Time of last command or log entry.
            std::unordered_map<LogLevel, size_t> entriesByLevel;       ///< Entry count per severity level.
            std::unordered_map<std::string, size_t> entriesByCategory; ///< Entry count per category.
        };
        ConsoleStats GetStats() const;

      private:
        /**
     * @brief Render log display area
     */
        void RenderLogDisplay();

        /**
     * @brief Render command input area
     */
        void RenderCommandInput();

        /**
     * @brief Render filter controls
     */
        void RenderFilterControls();

        /**
     * @brief Render context menu
     */
        void RenderContextMenu();

        /**
     * @brief Render log entry
     * @param entry Log entry to render
     * @param index Entry index for unique IDs
     */
        void RenderLogEntry(const LogEntry& entry, size_t index);

        /**
     * @brief Apply current filter to log entries
     */
        void UpdateFilteredEntries();

        /**
     * @brief Parse command line into command and arguments
     * @param commandLine Command line to parse
     * @return Pair of command name and arguments
     */
        std::pair<std::string, std::vector<std::string>> ParseCommandLine(const std::string& commandLine);

        /**
     * @brief Register built-in commands
     */
        void RegisterBuiltInCommands();

        /**
     * @brief Handle command auto-completion
     * @param input Current input text
     * @return Vector of completion suggestions
     */
        std::vector<std::string> GetCompletionSuggestions(const std::string& input);

        /**
     * @brief Get log level color
     * @param level Log level
     * @return ImVec4 color
     */
        ImVec4 GetLogLevelColor(LogLevel level) const;

        /**
     * @brief Get category icon
     * @param category Log category
     * @return Icon string
     */
        const char* GetCategoryIcon(const std::string& category) const;

        /**
     * @brief Format timestamp
     * @param timestamp Timestamp to format
     * @return Formatted string
     */
        std::string FormatTimestamp(const std::chrono::system_clock::time_point& timestamp) const;

        /**
     * @brief Update statistics
     */
        void UpdateStats();

        /**
     * @brief Process pending log entries from logger
     */
        void ProcessPendingLogEntries();

      private:
        // Log display
        std::vector<LogEntry> m_logEntries;    ///< All received log entries.
        std::vector<size_t> m_filteredIndices; ///< Indices of entries that pass the active filter.
        size_t m_maxLogEntries = 10000;        ///< Max entries before oldest are discarded.
        mutable std::mutex m_logMutex;         ///< Guards m_logEntries (logger writes from any thread).

        // Commands
        std::unordered_map<std::string, ConsoleCommand> m_commands; ///< Registered command lookup table.
        std::vector<ConsoleHistoryEntry> m_commandHistory;          ///< Ordered list of executed commands.
        std::string m_currentCommand;                               ///< In-progress command text.
        int m_historyIndex = -1;                                    ///< Arrow-key position in command history.
        std::vector<std::string> m_completionSuggestions;           ///< Tab-completion candidates.
        int m_completionIndex = -1;                                 ///< Current tab-completion selection.

        // Filtering
        ConsoleFilter m_filter;      ///< Active filter configuration.
        std::string m_searchBuffer;  ///< Text in the search input field.
        bool m_filterChanged = true; ///< True when filter needs reapplication.

        // UI state
        bool m_showFilterControls = false; ///< Whether the filter panel is expanded.
        bool m_showContextMenu = false;    ///< Whether the right-click context menu is open.
        bool m_scrollToBottom = false;     ///< Request to scroll to the newest entry next frame.
        bool m_commandInputActive = false; ///< Whether the command input field has focus.
        ImVec2 m_lastWindowSize;           ///< Cached window size for layout calculations.

        // Statistics
        mutable ConsoleStats m_stats;                            ///< Cached statistics snapshot.
        std::atomic<size_t> m_commandCounter{0};                 ///< Thread-safe total command count.
        std::atomic<size_t> m_engineCommandCounter{0};           ///< Thread-safe engine command count.
        std::chrono::steady_clock::time_point m_lastStatsUpdate; ///< Last time stats were recomputed.

        // Integration
        EditorLogger* m_logger = nullptr;  ///< Non-owning pointer to the editor logger singleton.
        bool m_isLoggerIntegrated = false; ///< True once the logger callback is registered.
        size_t m_lastPolledLogIndex = 0;   ///< Tracks how many logger entries we've already consumed

        // Performance
        static constexpr size_t MAX_VISIBLE_ENTRIES = 1000;
        static constexpr float STATS_UPDATE_INTERVAL = 1.0f;

        // Command input
        static constexpr size_t COMMAND_BUFFER_SIZE = 512;
        char m_commandBuffer[COMMAND_BUFFER_SIZE] = {0};
    };

} // namespace SparkEditor