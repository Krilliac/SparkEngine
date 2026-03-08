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
        std::string name;
        std::string description;
        std::string usage;
        std::function<std::string(const std::vector<std::string>&)> handler;
        bool isEngineCommand = false; // Whether to forward to engine
    };

    /**
 * @brief Console history entry
 */
    struct ConsoleHistoryEntry
    {
        std::string command;
        std::string result;
        std::chrono::system_clock::time_point timestamp;
        bool wasSuccessful = true;
    };

    /**
 * @brief Console filter settings
 */
    struct ConsoleFilter
    {
        LogLevel minLevel = LogLevel::TRACE;
        std::vector<std::string> enabledCategories;
        bool enableAllCategories = true;
        std::string searchPattern;
        bool showTimestamps = true;
        bool showCategories = true;
        bool showThreadIds = false;
        bool showFileInfo = false;
        bool colorCodeLevels = true;
        bool autoScroll = true;
        bool wordWrap = true;
        int maxDisplayEntries = 1000;

        // Additional properties for simplified filter UI
        bool showInfo = true;
        bool showWarning = true;
        bool showError = true;
        std::string searchText;
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
            size_t totalLogEntries = 0;
            size_t visibleLogEntries = 0;
            size_t commandsExecuted = 0;
            size_t engineCommandsExecuted = 0;
            float averageCommandTime = 0.0f;
            std::chrono::system_clock::time_point lastActivity;
            std::unordered_map<LogLevel, size_t> entriesByLevel;
            std::unordered_map<std::string, size_t> entriesByCategory;
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
        std::vector<LogEntry> m_logEntries;
        std::vector<size_t> m_filteredIndices; // Indices of entries that pass filter
        size_t m_maxLogEntries = 10000;
        mutable std::mutex m_logMutex;

        // Commands
        std::unordered_map<std::string, ConsoleCommand> m_commands;
        std::vector<ConsoleHistoryEntry> m_commandHistory;
        std::string m_currentCommand;
        int m_historyIndex = -1;
        std::vector<std::string> m_completionSuggestions;
        int m_completionIndex = -1;

        // Filtering
        ConsoleFilter m_filter;
        std::string m_searchBuffer;
        bool m_filterChanged = true;

        // UI state
        bool m_showFilterControls = false;
        bool m_showContextMenu = false;
        bool m_scrollToBottom = false;
        bool m_commandInputActive = false;
        ImVec2 m_lastWindowSize;

        // Statistics
        mutable ConsoleStats m_stats;
        std::atomic<size_t> m_commandCounter{0};
        std::atomic<size_t> m_engineCommandCounter{0};
        std::chrono::steady_clock::time_point m_lastStatsUpdate;

        // Integration
        EditorLogger* m_logger = nullptr;
        bool m_isLoggerIntegrated = false;
        size_t m_lastPolledLogIndex = 0; ///< Tracks how many logger entries we've already consumed

        // Performance
        static constexpr size_t MAX_VISIBLE_ENTRIES = 1000;
        static constexpr float STATS_UPDATE_INTERVAL = 1.0f;

        // Command input
        static constexpr size_t COMMAND_BUFFER_SIZE = 512;
        char m_commandBuffer[COMMAND_BUFFER_SIZE] = {0};
    };

} // namespace SparkEditor