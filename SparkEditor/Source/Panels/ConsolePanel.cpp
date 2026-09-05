/**
 * @file ConsolePanel.cpp
 * @brief Implementation of the advanced console panel with integrated logging
 * @author Spark Engine Team
 * @date 2025
 */

#include "ConsolePanel.h"

// Standard library headers
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Project and third-party headers
#include "../Core/EditorLogger.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include "Utils/LogMacros.h"
#include "Utils/Logger.h"
#include "Utils/SparkConsole.h"

namespace SparkEditor
{

    // File-scope state for the export dialog
    static bool s_showExportDialog = false;
    static char s_exportPathBuffer[512] = "console_export.txt";
    static int s_exportFormatIndex = 0; // 0 = txt, 1 = csv

    // Map an engine Spark::LogLevel onto the editor's display severity.
    static LogLevel TranslateEngineLogLevel(Spark::LogLevel level)
    {
        switch (level)
        {
        case Spark::LogLevel::Trace:
            return LogLevel::TRACE;
        case Spark::LogLevel::Debug:
            return LogLevel::DEBUG;
        case Spark::LogLevel::Warn:
            return LogLevel::WARNING;
        case Spark::LogLevel::Error:
            return LogLevel::ERROR_;
        case Spark::LogLevel::Fatal:
            return LogLevel::CRITICAL;
        default:
            return LogLevel::INFO;
        }
    }

    // Local helper to convert LogLevel to string
    static const char* LogLevelToString(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::TRACE:
            return "TRACE";
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARNING:
            return "WARNING";
        case LogLevel::ERROR_:
            return "ERROR";
        case LogLevel::CRITICAL:
            return "CRITICAL";
        default:
            return "UNKNOWN";
        }
    }

    ConsolePanel::ConsolePanel() : EditorPanel("Engine Console", "EngineConsole")
    {
        SetTitle("Engine Console");
        m_filter.enableAllCategories = true;
        m_filter.minLevel = LogLevel::TRACE;
        m_filter.autoScroll = true;
        m_filter.showTimestamps = true;
        m_filter.showCategories = true;
        m_filter.colorCodeLevels = true;
        m_stats.lastActivity = std::chrono::system_clock::now();
        m_lastStatsUpdate = std::chrono::steady_clock::now();
    }

    ConsolePanel::~ConsolePanel()
    {
        Shutdown();
    }

    bool ConsolePanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (m_isInitialized)
        {
            return true;
        }
        RegisterBuiltInCommands();

        // Subscribe to the engine Logger. Without this the panel titled
        // "Engine Console" only ever echoed commands typed into it.
        // Guarded on the queue, not on m_isInitialized: a Shutdown/Initialize
        // cycle must not install a second sink (the Logger cannot remove one).
        if (!m_pendingEngineLogs)
        {
            m_pendingEngineLogs = std::make_shared<PendingEngineLogs>();
            Spark::Logger::Get().AddSink(std::make_unique<Spark::CallbackSink>(
                [queue = m_pendingEngineLogs](const Spark::LogMessage& message)
                {
                    std::lock_guard<std::mutex> lock(queue->mutex);
                    if (queue->entries.size() >= MAX_PENDING_ENGINE_LOGS)
                        return;
                    LogEntry entry(TranslateEngineLogLevel(message.level),
                                   std::string(Spark::LogCategoryToString(message.category)), message.message);
                    entry.file = message.file;
                    entry.line = message.line;
                    entry.function = message.function;
                    queue->entries.push_back(std::move(entry));
                }));
        }

        m_isInitialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Console panel initialized");
        return true;
    }

    void ConsolePanel::Update(float /*deltaTime*/)
    {
        if (!m_isInitialized)
            return;

        // Pull both feeds before filtering, so entries that arrived this frame
        // are visible this frame.
        ProcessPendingLogEntries();
        DrainEngineLogQueue();

        if (m_filterChanged)
        {
            // Deliberately silent: this panel is now a Logger sink, so logging
            // here would append an entry, set m_filterChanged again, and spin
            // one self-produced log line per frame forever.
            UpdateFilteredEntries();
            m_filterChanged = false;
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - m_lastStatsUpdate).count() >=
            static_cast<int>(STATS_UPDATE_INTERVAL))
        {
            UpdateStats();
            m_lastStatsUpdate = now;
        }
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            if (m_filter.autoScroll && !m_logEntries.empty())
            {
                m_scrollToBottom = true;
            }
        }
    }

    void ConsolePanel::Render()
    {
        // No per-frame trace here: this panel is a Logger sink, so a trace-level
        // entry every frame would fill its own display in Debug builds.
        if (!BeginPanel())
        {
            EndPanel();
            return;
        }
        RenderCommandInput();
        RenderFilterControls();
        RenderLogDisplay();
        // Clear / Copy All / Export... / Reset Filters. Defined but never called
        // before, which made the export dialog below unreachable from the UI.
        RenderContextMenu();

        // Export dialog (modal popup for file save)
        if (s_showExportDialog)
        {
            ImGui::OpenPopup("Export Log");
            s_showExportDialog = false;
        }
        if (ImGui::BeginPopupModal("Export Log", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Export console log to file");
            ImGui::Separator();

            ImGui::InputText("File Path", s_exportPathBuffer, sizeof(s_exportPathBuffer));

            const char* formats[] = {"Text (.txt)", "CSV (.csv)"};
            ImGui::Combo("Format", &s_exportFormatIndex, formats, IM_ARRAYSIZE(formats));

            ImGui::Separator();
            if (ImGui::Button("Export", ImVec2(120, 0)))
            {
                std::string format = (s_exportFormatIndex == 1) ? "csv" : "txt";
                ExportLog(std::string(s_exportPathBuffer), format);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        EndPanel();
    }

    void ConsolePanel::Shutdown()
    {
        EditorPanel::Shutdown();
        m_logEntries.clear();
        m_commands.clear();
        m_commandHistory.clear();
    }

    bool ConsolePanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        if (eventType == "LogEntry" && eventData)
        {
            LogEntry* entry = static_cast<LogEntry*>(eventData);
            AddLogEntry(*entry);
            return true;
        }
        return EditorPanel::HandleEvent(eventType, eventData);
    }

    void ConsolePanel::RegisterCommand(const ConsoleCommand& command)
    {
        m_commands[command.name] = command;
    }

    void ConsolePanel::UnregisterCommand(const std::string& commandName)
    {
        m_commands.erase(commandName);
    }

    std::string ConsolePanel::ExecuteCommand(const std::string& commandLine)
    {
        if (commandLine.empty())
        {
            return "Empty command";
        }

        auto [command, args] = ParseCommandLine(commandLine);

        ConsoleHistoryEntry historyEntry;
        historyEntry.command = commandLine;
        historyEntry.timestamp = std::chrono::system_clock::now();

        auto& engineConsole = Spark::SimpleConsole::GetInstance();
        auto it = m_commands.find(command);
        const bool routeToEngine = (it != m_commands.end() && it->second.isEngineCommand) ||
                                   (it == m_commands.end() && engineConsole.HasCommand(command));

        if (routeToEngine)
        {
            // ConsoleCommand::isEngineCommand used to be inert and
            // m_engineCommandCounter never moved off zero. Dispatch through the
            // engine console; its own output arrives via the Logger sink.
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Engine console command executed: %s", command.c_str());
            historyEntry.wasSuccessful = engineConsole.ExecuteCommand(commandLine);
            historyEntry.result =
                historyEntry.wasSuccessful ? "Dispatched to engine: " + command : "Engine rejected command: " + command;
            m_engineCommandCounter++;
        }
        else if (it != m_commands.end())
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Console command executed: %s", command.c_str());
            historyEntry.result = it->second.handler(args);
            historyEntry.wasSuccessful = true;
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Unknown console command: %s", command.c_str());
            historyEntry.result = "Unknown command: " + command;
            historyEntry.wasSuccessful = false;
        }

        m_commandHistory.push_back(historyEntry);
        m_commandCounter++;
        return historyEntry.result;
    }

    void ConsolePanel::AddLogEntry(const LogEntry& entry)
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_logEntries.push_back(entry);
        if (m_logEntries.size() > m_maxLogEntries)
        {
            size_t toRemove = m_logEntries.size() - m_maxLogEntries;
            m_logEntries.erase(m_logEntries.begin(), m_logEntries.begin() + static_cast<ptrdiff_t>(toRemove));
        }
        m_filterChanged = true;
        m_stats.totalLogEntries = m_logEntries.size();
        m_stats.entriesByLevel[entry.level]++;
        m_stats.entriesByCategory[entry.category]++;
        m_stats.lastActivity = std::chrono::system_clock::now();
        if (m_filter.autoScroll)
        {
            m_scrollToBottom = true;
        }
    }

    void ConsolePanel::Clear()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Console log cleared");
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_logEntries.clear();
        m_filteredIndices.clear();
        m_stats.totalLogEntries = 0;
        m_stats.visibleLogEntries = 0;
        m_stats.entriesByLevel.clear();
        m_stats.entriesByCategory.clear();
    }

    void ConsolePanel::SetFilter(const ConsoleFilter& filter)
    {
        m_filter = filter;
        m_filterChanged = true;
    }

    bool ConsolePanel::ExportLog(const std::string& filePath, const std::string& format)
    {
        try
        {
            std::ofstream file(filePath);
            if (!file.is_open())
            {
                std::cerr << "Failed to open export file: " << filePath << "\n";
                return false;
            }

            if (format == "csv")
            {
                file << "Timestamp,Level,Category,Message,File,Line\n";
                for (const auto& entry : m_logEntries)
                {
                    file << FormatTimestamp(entry.timestamp) << "," << LogLevelToString(entry.level) << ","
                         << entry.category << "," << "\"" << entry.message << "\"," << entry.file << "," << entry.line
                         << "\n";
                }
            }
            else
            {
                file << "# Spark Engine Editor Console Log Export\n";
                file << "# Exported: " << FormatTimestamp(std::chrono::system_clock::now()) << "\n";
                file << "# Total Entries: " << m_logEntries.size() << "\n\n";

                for (const auto& entry : m_logEntries)
                {
                    file << "[" << FormatTimestamp(entry.timestamp) << "] " << LogLevelToString(entry.level) << " "
                         << entry.category << ": " << entry.message;

                    if (entry.level >= LogLevel::WARNING)
                    {
                        file << " (" << entry.file << ":" << entry.line << ")";
                    }

                    file << "\n";
                }
            }

            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Console log exported to: %s (%s format)", filePath.c_str(),
                           format.c_str());
            return true;
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Console log export failed: %s", e.what());
            return false;
        }
    }

    ConsolePanel::ConsoleStats ConsolePanel::GetStats() const
    {
        return m_stats;
    }

    void ConsolePanel::RenderLogDisplay()
    {
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        contentSize.y -= 30; // Reserve space for command input

        // Snapshot filtered entries under lock to avoid data races during rendering
        std::vector<LogEntry> visibleEntries;
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            visibleEntries.reserve(m_filteredIndices.size());
            for (size_t idx : m_filteredIndices)
            {
                if (idx < m_logEntries.size())
                {
                    visibleEntries.push_back(m_logEntries[idx]);
                }
            }
        }

        if (ImGui::BeginChild("LogDisplay", contentSize, true, ImGuiWindowFlags_AlwaysVerticalScrollbar))
        {
            ImVec2 currentSize = ImGui::GetWindowSize();
            if (currentSize.x != m_lastWindowSize.x || currentSize.y != m_lastWindowSize.y)
            {
                m_lastWindowSize = currentSize;
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visibleEntries.size()));

            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                {
                    if (i < 0 || i >= static_cast<int>(visibleEntries.size()))
                        continue;

                    RenderLogEntry(visibleEntries[i], i);
                }
            }

            clipper.End();

            if (m_scrollToBottom)
            {
                ImGui::SetScrollHereY(1.0f);
                m_scrollToBottom = false;
            }
        }
        ImGui::EndChild();
    }

    int ConsolePanel::HandleCommandInputCallback(ImGuiInputTextCallbackData* data)
    {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
        {
            if (m_commandHistory.empty())
                return 0;

            const int lastIndex = static_cast<int>(m_commandHistory.size()) - 1;
            if (data->EventKey == ImGuiKey_UpArrow)
            {
                if (m_historyIndex < 0)
                {
                    m_currentCommand.assign(data->Buf, static_cast<size_t>(data->BufTextLen));
                    m_historyIndex = lastIndex;
                }
                else if (m_historyIndex > 0)
                {
                    --m_historyIndex;
                }
            }
            else if (data->EventKey == ImGuiKey_DownArrow)
            {
                if (m_historyIndex < 0)
                    return 0;
                if (m_historyIndex < lastIndex)
                {
                    ++m_historyIndex;
                }
                else
                {
                    // Past the newest entry: restore what the user was typing.
                    m_historyIndex = -1;
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, m_currentCommand.c_str());
                    return 0;
                }
            }

            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, m_commandHistory[static_cast<size_t>(m_historyIndex)].command.c_str());
            return 0;
        }

        if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
        {
            const std::string prefix(data->Buf, static_cast<size_t>(data->BufTextLen));
            if (m_completionIndex < 0 || m_completionSuggestions.empty() || prefix != m_completionPrefix)
            {
                m_completionPrefix = prefix;
                m_completionSuggestions = GetCompletionSuggestions(prefix);
                std::sort(m_completionSuggestions.begin(), m_completionSuggestions.end());
                m_completionIndex = -1;
            }
            if (m_completionSuggestions.empty())
                return 0;

            m_completionIndex = (m_completionIndex + 1) % static_cast<int>(m_completionSuggestions.size());
            const std::string& suggestion = m_completionSuggestions[static_cast<size_t>(m_completionIndex)];
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, suggestion.c_str());
            return 0;
        }

        return 0;
    }

    void ConsolePanel::RenderCommandInput()
    {
        // Simple command input at top
        ImGui::SetNextItemWidth(-60);
        const ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
                                               ImGuiInputTextFlags_CallbackHistory |
                                               ImGuiInputTextFlags_CallbackCompletion;
        bool submitted = ImGui::InputText(
            "##CmdInput", m_commandBuffer, COMMAND_BUFFER_SIZE, inputFlags, [](ImGuiInputTextCallbackData* data)
            { return static_cast<ConsolePanel*>(data->UserData)->HandleCommandInputCallback(data); }, this);
        ImGui::SameLine();
        if (ImGui::Button("Run") || submitted)
        {
            std::string cmd(m_commandBuffer);
            if (!cmd.empty())
            {
                std::string result = ExecuteCommand(cmd);
                // Add result as log entry
                LogEntry resultEntry(LogLevel::INFO, "Console", result);
                AddLogEntry(resultEntry);
                m_commandBuffer[0] = '\0';
                m_currentCommand.clear();
                m_historyIndex = -1;
                m_completionIndex = -1;
                m_completionPrefix.clear();
                m_completionSuggestions.clear();
            }
        }
    }

    void ConsolePanel::RenderFilterControls()
    {
        if (ImGui::BeginChild("FilterControls", ImVec2(0, 120), true))
        {
            ImGui::Text("Log Level:");
            ImGui::SameLine();

            const char* levelNames[] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
            int currentLevel = static_cast<int>(m_filter.minLevel);
            if (ImGui::Combo("##LogLevel", &currentLevel, levelNames, IM_ARRAYSIZE(levelNames)))
            {
                m_filter.minLevel = static_cast<LogLevel>(currentLevel);
                m_filterChanged = true;
            }

            ImGui::Text("Categories:");
            ImGui::SameLine();
            if (ImGui::Checkbox("All##Categories", &m_filter.enableAllCategories))
            {
                m_filterChanged = true;
            }

            // The search pattern was already honoured by UpdateFilteredEntries;
            // it simply had no way in.
            ImGui::Text("Search:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputTextWithHint("##ConsoleSearch", "filter log text", m_searchBuffer, sizeof(m_searchBuffer)))
            {
                m_filter.searchPattern = m_searchBuffer;
                m_filterChanged = true;
            }

            ImGui::Separator();
            ImGui::Text("Display:");

            if (ImGui::Checkbox("Timestamps", &m_filter.showTimestamps))
                m_filterChanged = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Categories", &m_filter.showCategories))
                m_filterChanged = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("File Info", &m_filter.showFileInfo))
                m_filterChanged = true;

            if (ImGui::Checkbox("Color Coding", &m_filter.colorCodeLevels))
                m_filterChanged = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Auto Scroll", &m_filter.autoScroll))
                m_filterChanged = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Word Wrap", &m_filter.wordWrap))
                m_filterChanged = true;
        }
        ImGui::EndChild();
    }

    void ConsolePanel::RenderContextMenu()
    {
        if (ImGui::BeginPopupContextWindow("ConsoleContextMenu"))
        {
            if (ImGui::MenuItem("Clear Log"))
            {
                Clear();
            }

            if (ImGui::MenuItem("Copy All"))
            {
                std::string allText;
                allText.reserve(m_filteredIndices.size() * 128);
                for (size_t idx : m_filteredIndices)
                {
                    if (idx >= m_logEntries.size())
                        continue;
                    const auto& entry = m_logEntries[idx];
                    allText += "[";
                    allText += FormatTimestamp(entry.timestamp);
                    allText += "] ";
                    allText += LogLevelToString(entry.level);
                    allText += " ";
                    allText += entry.category;
                    allText += ": ";
                    allText += entry.message;
                    allText += "\n";
                }
                ImGui::SetClipboardText(allText.c_str());
            }

            if (ImGui::MenuItem("Export..."))
            {
                s_showExportDialog = true;
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Reset Filters"))
            {
                m_filter = ConsoleFilter{};
                m_filterChanged = true;
            }

            ImGui::EndPopup();
        }
    }

    void ConsolePanel::RenderLogEntry(const LogEntry& entry, size_t index)
    {
        ImGui::PushID(static_cast<int>(index));

        ImVec4 textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (m_filter.colorCodeLevels)
        {
            textColor = GetLogLevelColor(entry.level);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, textColor);

        std::string displayText;

        if (m_filter.showTimestamps)
        {
            displayText += "[" + FormatTimestamp(entry.timestamp) + "] ";
        }

        displayText += LogLevelToString(entry.level);
        displayText += " ";

        if (m_filter.showCategories)
        {
            displayText += GetCategoryIcon(entry.category);
            displayText += entry.category;
            displayText += " | ";
        }

        displayText += entry.message;

        if (m_filter.showFileInfo && entry.level >= LogLevel::WARNING && !entry.file.empty())
        {
            displayText += " (";
            displayText += entry.file;
            displayText += ":";
            displayText += std::to_string(entry.line);
            displayText += ")";
        }

        if (m_filter.wordWrap)
        {
            ImGui::TextWrapped("%s", displayText.c_str());
        }
        else
        {
            ImGui::Text("%s", displayText.c_str());
        }

        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Full Entry Details:");
            ImGui::Separator();
            ImGui::Text("Time: %s", FormatTimestamp(entry.timestamp).c_str());
            ImGui::Text("Level: %s", LogLevelToString(entry.level));
            ImGui::Text("Category: %s", entry.category.c_str());
            ImGui::Text("Function: %s", entry.function.c_str());
            ImGui::Text("File: %s:%d", entry.file.c_str(), entry.line);
            ImGui::Text("Frame: %llu", static_cast<unsigned long long>(entry.frameNumber));
            ImGui::EndTooltip();
        }

        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    void ConsolePanel::UpdateFilteredEntries()
    {
        // Copy entries under lock, then filter without holding the lock to avoid
        // blocking AddLogEntry() callers during expensive string searches.
        std::vector<LogEntry> entriesCopy;
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            entriesCopy = m_logEntries;
        }

        // Pre-compute lowercase search pattern once outside the loop
        std::string lowerPattern;
        if (!m_filter.searchPattern.empty())
        {
            lowerPattern = m_filter.searchPattern;
            std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }

        std::vector<size_t> newFilteredIndices;
        newFilteredIndices.reserve(entriesCopy.size());

        for (size_t i = 0; i < entriesCopy.size(); ++i)
        {
            const auto& entry = entriesCopy[i];

            if (entry.level < m_filter.minLevel)
                continue;

            if (!m_filter.enableAllCategories)
            {
                bool categoryEnabled = std::find(m_filter.enabledCategories.begin(), m_filter.enabledCategories.end(),
                                                 entry.category) != m_filter.enabledCategories.end();
                if (!categoryEnabled)
                    continue;
            }

            if (!lowerPattern.empty())
            {
                std::string lowerMessage = entry.message;
                std::transform(lowerMessage.begin(), lowerMessage.end(), lowerMessage.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                if (!lowerMessage.contains(lowerPattern))
                    continue;
            }

            newFilteredIndices.push_back(i);
        }

        if (newFilteredIndices.size() > MAX_VISIBLE_ENTRIES)
        {
            newFilteredIndices.erase(newFilteredIndices.begin(), newFilteredIndices.end() - MAX_VISIBLE_ENTRIES);
        }

        // Swap results back under lock
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            m_filteredIndices = std::move(newFilteredIndices);
            m_stats.visibleLogEntries = m_filteredIndices.size();
        }
    }

    std::pair<std::string, std::vector<std::string>> ConsolePanel::ParseCommandLine(const std::string& commandLine)
    {
        std::vector<std::string> tokens;
        std::istringstream iss(commandLine);
        std::string token;

        while (iss >> token)
        {
            tokens.push_back(token);
        }

        if (tokens.empty())
        {
            return {"", {}};
        }

        std::string command = tokens[0];
        std::vector<std::string> args(tokens.begin() + 1, tokens.end());

        return {command, args};
    }

    void ConsolePanel::RegisterBuiltInCommands()
    {
        RegisterCommand({"help", "Show available commands", "help [command]",
                         [this](const std::vector<std::string>& args) -> std::string
                         {
                             if (!args.empty())
                             {
                                 auto it = m_commands.find(args[0]);
                                 if (it != m_commands.end())
                                 {
                                     return "Command: " + it->second.name + "\n" +
                                            "Description: " + it->second.description + "\n" +
                                            "Usage: " + it->second.usage;
                                 }
                                 return "Unknown command: " + args[0];
                             }
                             std::string help = "Available commands:\n";
                             for (const auto& [name, cmd] : m_commands)
                             {
                                 help += "  " + name + " - " + cmd.description + "\n";
                             }
                             return help;
                         },
                         false});

        RegisterCommand({"clear", "Clear the console log", "clear",
                         [this](const std::vector<std::string>&) -> std::string
                         {
                             Clear();
                             return "Console cleared";
                         },
                         false});

        RegisterCommand({"export", "Export console log to file", "export <filename> [format]",
                         [this](const std::vector<std::string>& args) -> std::string
                         {
                             if (args.empty())
                                 return "Usage: export <filename> [format]\nFormats: txt, csv";
                             std::string format = args.size() > 1 ? args[1] : "txt";
                             return ExportLog(args[0], format) ? "Log exported to " + args[0] : "Export failed";
                         },
                         false});

        RegisterCommand({"stats", "Show console statistics", "stats",
                         [this](const std::vector<std::string>&) -> std::string
                         {
                             auto stats = GetStats();
                             std::stringstream ss;
                             ss << "Console Statistics:\n";
                             ss << "  Total Log Entries: " << stats.totalLogEntries << "\n";
                             ss << "  Visible Entries: " << stats.visibleLogEntries << "\n";
                             ss << "  Commands Executed: " << stats.commandsExecuted << "\n";
                             return ss.str();
                         },
                         false});

        RegisterCommand({"echo", "Echo the provided arguments", "echo <text>",
                         [](const std::vector<std::string>& args) -> std::string
                         {
                             std::string result;
                             for (size_t i = 0; i < args.size(); ++i)
                             {
                                 if (i > 0)
                                     result += " ";
                                 result += args[i];
                             }
                             return result.empty() ? "echo: no arguments provided" : result;
                         },
                         false});
    }

    std::vector<std::string> ConsolePanel::GetCompletionSuggestions(const std::string& input)
    {
        std::vector<std::string> suggestions;

        for (const auto& [name, cmd] : m_commands)
        {
            if (input.empty() || name.substr(0, input.length()) == input)
            {
                suggestions.push_back(name);
            }
        }

        return suggestions;
    }

    ImVec4 ConsolePanel::GetLogLevelColor(LogLevel level) const
    {
        switch (level)
        {
        case LogLevel::TRACE:
            return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        case LogLevel::DEBUG:
            return ImVec4(0.6f, 0.8f, 1.0f, 1.0f);
        case LogLevel::INFO:
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        case LogLevel::WARNING:
            return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
        case LogLevel::ERROR_:
            return ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        case LogLevel::CRITICAL:
            return ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
        default:
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

    const char* ConsolePanel::GetCategoryIcon(const std::string& category) const
    {
        if (category == "General")
            return "[G] ";
        if (category == "Asset")
            return "[A] ";
        if (category == "Rendering")
            return "[R] ";
        if (category == "Engine")
            return "[E] ";
        if (category == "UI")
            return "[U] ";
        if (category == "Scripting")
            return "[S] ";
        if (category == "Physics")
            return "[P] ";
        if (category == "Audio")
            return "[Au] ";
        if (category == "Networking")
            return "[N] ";
        if (category == "Profiling")
            return "[Pr] ";
        if (category == "Crash")
            return "[!] ";
        if (category == "Console")
            return "[>] ";
        return "[?] ";
    }

    std::string ConsolePanel::FormatTimestamp(const std::chrono::system_clock::time_point& timestamp) const
    {
        auto time_t = std::chrono::system_clock::to_time_t(timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
        ss << "." << std::setfill('0') << std::setw(3) << ms.count();

        return ss.str();
    }

    void ConsolePanel::UpdateStats()
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_stats.lastActivity = std::chrono::system_clock::now();
        m_stats.totalLogEntries = m_logEntries.size();
        m_stats.visibleLogEntries = m_filteredIndices.size();
        m_stats.commandsExecuted = m_commandCounter.load();
        m_stats.engineCommandsExecuted = m_engineCommandCounter.load();
    }

    void ConsolePanel::ProcessPendingLogEntries()
    {
        if (!m_logger)
        {
            m_logger = &EditorLogger::GetInstance();
        }

        const auto& memoryLogs = m_logger->GetMemoryLogs();
        size_t currentCount = memoryLogs.size();

        if (currentCount <= m_lastPolledLogIndex)
        {
            // Logger was cleared or no new entries
            if (currentCount < m_lastPolledLogIndex)
            {
                m_lastPolledLogIndex = 0;
            }
            return;
        }

        // Copy new entries from the logger into the console panel
        for (size_t i = m_lastPolledLogIndex; i < currentCount; ++i)
        {
            AddLogEntry(memoryLogs[i]);
        }
        m_lastPolledLogIndex = currentCount;
    }

    void ConsolePanel::DrainEngineLogQueue()
    {
        if (!m_pendingEngineLogs)
            return;

        std::vector<LogEntry> pending;
        {
            std::lock_guard<std::mutex> lock(m_pendingEngineLogs->mutex);
            if (m_pendingEngineLogs->entries.empty())
                return;
            pending.swap(m_pendingEngineLogs->entries);
        }

        for (const LogEntry& entry : pending)
        {
            AddLogEntry(entry);
        }
    }

} // namespace SparkEditor
