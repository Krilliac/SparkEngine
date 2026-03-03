/**
 * @file ConsolePanel.cpp
 * @brief Implementation of the advanced console panel with integrated logging
 * @author Spark Engine Team
 * @date 2025
 */

#include "ConsolePanel.h"
#include "../Core/EditorLogger.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <iomanip>
#include <thread>

namespace SparkEditor {

ConsolePanel::ConsolePanel() : EditorPanel("Engine Console", "EngineConsole") {
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

ConsolePanel::~ConsolePanel() {
    Shutdown();
}

bool ConsolePanel::Initialize() {
    if (m_isInitialized) {
        return true;
    }
    m_logger = &EditorLogger::GetInstance();
    RegisterBuiltInCommands();
    if (m_logger) {
        m_isLoggerIntegrated = true;
    }
    m_isInitialized = true;
    if (m_logger) {
        m_logger->Log(LogLevel::INFO, LogCategory::UI, "Console Panel initialized successfully");
    }
    return true;
}

void ConsolePanel::Update(float deltaTime) {
    if (!m_isInitialized) return;
    ProcessPendingLogEntries();
    if (m_filterChanged) {
        UpdateFilteredEntries();
        m_filterChanged = false;
    }
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - m_lastStatsUpdate).count() >= static_cast<int>(STATS_UPDATE_INTERVAL)) {
        UpdateStats();
        m_lastStatsUpdate = now;
    }
    if (m_filter.autoScroll && !m_logEntries.empty()) {
        m_scrollToBottom = true;
    }
}

void ConsolePanel::Render() {
    if (!BeginPanel()) {
        EndPanel();
        return;
    }
    RenderCommandInput();
    RenderFilterControls();
    RenderLogDisplay();
    // RenderStats(); // If implemented
    EndPanel();
}

void ConsolePanel::Shutdown() {
    EditorPanel::Shutdown();
    m_logEntries.clear();
    m_commands.clear();
    m_commandHistory.clear();
}

bool ConsolePanel::HandleEvent(const std::string& eventType, void* eventData) {
    if (eventType == "LogEntry" && eventData) {
        LogEntry* entry = static_cast<LogEntry*>(eventData);
        AddLogEntry(*entry);
        return true;
    }
    return EditorPanel::HandleEvent(eventType, eventData);
}

void ConsolePanel::RegisterCommand(const ConsoleCommand& command) {
    m_commands[command.name] = command;
}

void ConsolePanel::UnregisterCommand(const std::string& commandName) {
    m_commands.erase(commandName);
}

std::string ConsolePanel::ExecuteCommand(const std::string& commandLine) {
    if (commandLine.empty()) {
        return "Empty command";
    }
    ConsoleHistoryEntry historyEntry;
    historyEntry.command = commandLine;
    historyEntry.timestamp = std::chrono::system_clock::now();
    std::string result = commandLine;
    if (result.find("clear") == 0) {
        m_commandHistory.clear();
        return "Console cleared.";
    }
    if (result.find("help") == 0) {
        return "Available commands: clear, help, echo <text>, list";
    }
    if (result.find("echo ") == 0) {
        return result.substr(5);
    }
    if (result.find("list") == 0) {
        return "Command history: " + std::to_string(m_commandHistory.size()) + " entries";
    }
    return "Unknown command: " + commandLine;
}

void ConsolePanel::AddLogEntry(const LogEntry& entry) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_logEntries.push_back(entry);
    if (m_logEntries.size() > m_maxLogEntries) {
        size_t toRemove = m_logEntries.size() - m_maxLogEntries;
        m_logEntries.erase(m_logEntries.begin(), m_logEntries.begin() + toRemove);
    }
    m_filterChanged = true;
    m_stats.totalLogEntries = m_logEntries.size();
    m_stats.entriesByLevel[entry.level]++;
    m_stats.entriesByCategory[entry.category]++;
    m_stats.lastActivity = std::chrono::system_clock::now();
    if (m_filter.autoScroll) {
        m_scrollToBottom = true;
    }
}

void ConsolePanel::Clear() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_logEntries.clear();
    m_filteredIndices.clear();
    m_stats.totalLogEntries = 0;
    m_stats.visibleLogEntries = 0;
    m_stats.entriesByLevel.clear();
    m_stats.entriesByCategory.clear();
    
    if (m_logger) {
        m_logger->Log(LogLevel::INFO, LogCategory::UI, "Console log cleared");
    }
}

void ConsolePanel::SetFilter(const ConsoleFilter& filter) {
    m_filter = filter;
    m_filterChanged = true;
}

bool ConsolePanel::ExportLog(const std::string& filePath, const std::string& format) {
    try {
        std::ofstream file(filePath);
        if (!file.is_open()) {
            if (m_logger) {
                m_logger->Log(LogLevel::ERROR_, LogCategory::UI, "Failed to open export file: " + filePath);
            }
            return false;
        }
        
        if (format == "csv") {
            // CSV format
            file << "Timestamp,Level,Category,Message,File,Line,Thread\n";
            for (const auto& entry : m_logEntries) {
                file << FormatTimestamp(entry.timestamp) << ","
                     << LogLevelToString(entry.level) << ","
                     << LogCategoryToString(entry.category) << ","
                     << "\"" << entry.message << "\","
                     << entry.file << ","
                     << entry.line << ","
                     << std::hex << entry.threadId << "\n";
            }
        } else {
            // Plain text format
            file << "# Spark Engine Editor Console Log Export\n";
            file << "# Exported: " << FormatTimestamp(std::chrono::system_clock::now()) << "\n";
            file << "# Total Entries: " << m_logEntries.size() << "\n\n";
            
            for (const auto& entry : m_logEntries) {
                file << "[" << FormatTimestamp(entry.timestamp) << "] "
                     << LogLevelToString(entry.level) << " "
                     << LogCategoryToString(entry.category) << ": "
                     << entry.message;
                
                if (entry.level >= LogLevel::WARNING) {
                    file << " (" << entry.file << ":" << entry.line << ")";
                }
                
                file << "\n";
            }
        }
        
        if (m_logger) {
            m_logger->Log(LogLevel::INFO, LogCategory::UI, 
                         "Console log exported to: " + filePath + " (" + format + " format)");
        }
        
        return true;
        
    } catch (const std::exception& e) {
        if (m_logger) {
            m_logger->Log(LogLevel::ERROR_, LogCategory::UI, 
                         "Export failed: " + std::string(e.what()));
        }
        return false;
    }
}

ConsolePanel::ConsoleStats ConsolePanel::GetStats() const {
    return m_stats;
}

void ConsolePanel::RenderLogDisplay() {
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    contentSize.y -= 30; // Reserve space for command input
    
    if (ImGui::BeginChild("LogDisplay", contentSize, true, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
        // Check if window size changed (affects text wrapping)
        ImVec2 currentSize = ImGui::GetWindowSize();
        if (currentSize.x != m_lastWindowSize.x || currentSize.y != m_lastWindowSize.y) {
            m_lastWindowSize = currentSize;
        }
        
        // Render filtered log entries
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_filteredIndices.size()));
        
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                if (i < 0 || i >= static_cast<int>(m_filteredIndices.size())) {
                    continue;
                }
                
                size_t entryIndex = m_filteredIndices[i];
                if (entryIndex >= m_logEntries.size()) {
                    continue;
                }
                
                RenderLogEntry(m_logEntries[entryIndex], i);
            }
        }
        
        clipper.End();
        
        // Auto-scroll to bottom
        if (m_scrollToBottom) {
            ImGui::SetScrollHereY(1.0f);
            m_scrollToBottom = false;
        }
        
        // Context menu
        if (ImGui::IsWindowHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            m_showContextMenu = true;
        }
    }
    ImGui::EndChild();
}

void ConsolePanel::RenderFilterControls() {
    if (ImGui::BeginChild("FilterControls", ImVec2(0, 120), true)) {
        // Log level filter
        ImGui::Text("Log Level:");
        ImGui::SameLine();
        
        const char* levelNames[] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL", "FATAL"};
        int currentLevel = static_cast<int>(m_filter.minLevel);
        if (ImGui::Combo("##LogLevel", &currentLevel, levelNames, IM_ARRAYSIZE(levelNames))) {
            m_filter.minLevel = static_cast<LogLevel>(currentLevel);
            m_filterChanged = true;
        }
        
        // Category filters
        ImGui::Text("Categories:");
        ImGui::SameLine();
        if (ImGui::Checkbox("All##Categories", &m_filter.enableAllCategories)) {
            m_filterChanged = true;
        }
        
        // Display options
        ImGui::Separator();
        ImGui::Text("Display:");
        
        if (ImGui::Checkbox("Timestamps", &m_filter.showTimestamps)) m_filterChanged = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Categories", &m_filter.showCategories)) m_filterChanged = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Thread IDs", &m_filter.showThreadIds)) m_filterChanged = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("File Info", &m_filter.showFileInfo)) m_filterChanged = true;
        
        if (ImGui::Checkbox("Color Coding", &m_filter.colorCodeLevels)) m_filterChanged = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Auto Scroll", &m_filter.autoScroll)) m_filterChanged = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Word Wrap", &m_filter.wordWrap)) m_filterChanged = true;
    }
    ImGui::EndChild();
}

void ConsolePanel::RenderContextMenu() {
    if (ImGui::BeginPopupContextWindow("ConsoleContextMenu")) {
        if (ImGui::MenuItem("Clear Log")) {
            Clear();
        }
        
        if (ImGui::MenuItem("Copy All")) {
            // TODO: Copy all visible entries to clipboard
        }
        
        if (ImGui::MenuItem("Export...")) {
            // TODO: Show export dialog
            ExportLog("console_export.txt");
        }
        
        ImGui::Separator();
        
        if (ImGui::MenuItem("Reset Filters")) {
            m_filter = ConsoleFilter{};
            m_filterChanged = true;
        }
        
        m_showContextMenu = false;
        ImGui::EndPopup();
    }
}

void ConsolePanel::RenderLogEntry(const LogEntry& entry, size_t index) {
    ImGui::PushID(static_cast<int>(index));
    
    // Color coding based on log level
    ImVec4 textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (m_filter.colorCodeLevels) {
        textColor = GetLogLevelColor(entry.level);
    }
    
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    
    // Build display string
    std::string displayText;
    
    // Timestamp
    if (m_filter.showTimestamps) {
        displayText += "[" + FormatTimestamp(entry.timestamp) + "] ";
    }
    
    // Level with icon
    displayText += LogLevelToString(entry.level);
    displayText += " ";
    
    // Category with icon
    if (m_filter.showCategories) {
        displayText += GetCategoryIcon(entry.category);
        displayText += LogCategoryToString(entry.category);
        displayText += " | ";
    }
    
    // Message
    displayText += entry.message;
    
    // File info for warnings/errors
    if (m_filter.showFileInfo && entry.level >= LogLevel::WARNING) {
        displayText += " (";
        displayText += entry.file;
        displayText += ":";
        displayText += std::to_string(entry.line);
        displayText += ")";
    }
    
    // Thread ID
    if (m_filter.showThreadIds) {
        std::stringstream ss;
        ss << " [Thread:" << std::hex << entry.threadId << "]";
        displayText += ss.str();
    }
    
    // Render the text (with or without wrapping)
    if (m_filter.wordWrap) {
        ImGui::TextWrapped("%s", displayText.c_str());
    } else {
        ImGui::Text("%s", displayText.c_str());
    }
    
    // Tooltip with full entry details
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Full Entry Details:");
        ImGui::Separator();
        ImGui::Text("Time: %s", FormatTimestamp(entry.timestamp).c_str());
        ImGui::Text("Level: %s", LogLevelToString(entry.level));
        ImGui::Text("Category: %s", LogCategoryToString(entry.category));
        ImGui::Text("Function: %s", entry.function.c_str());
        ImGui::Text("File: %s:%d", entry.file.c_str(), entry.line);
        ImGui::Text("Thread: 0x%x", static_cast<uint32_t>(std::hash<std::thread::id>{}(entry.threadId)));
        ImGui::Text("Frame: %llu", entry.frameNumber);
        
        if (!entry.metadata.empty()) {
            ImGui::Separator();
            ImGui::Text("Metadata:");
            for (const auto& [key, value] : entry.metadata) {
                ImGui::Text("  %s: %s", key.c_str(), value.c_str());
            }
        }
        
        ImGui::EndTooltip();
    }
    
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void ConsolePanel::UpdateFilteredEntries() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    
    m_filteredIndices.clear();
    m_filteredIndices.reserve(m_logEntries.size());
    
    for (size_t i = 0; i < m_logEntries.size(); ++i) {
        const auto& entry = m_logEntries[i];
        
        // Level filter
        if (entry.level < m_filter.minLevel) {
            continue;
        }
        
        // Category filter
        if (!m_filter.enableAllCategories) {
            bool categoryEnabled = std::find(m_filter.enabledCategories.begin(),
                                            m_filter.enabledCategories.end(),
                                            entry.category) != m_filter.enabledCategories.end();
            if (!categoryEnabled) {
                continue;
            }
        }
        
        // Search pattern filter
        if (!m_filter.searchPattern.empty()) {
            std::string lowerMessage = entry.message;
            std::string lowerPattern = m_filter.searchPattern;
            std::transform(lowerMessage.begin(), lowerMessage.end(), lowerMessage.begin(), ::tolower);
            std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(), ::tolower);
            
            if (lowerMessage.find(lowerPattern) == std::string::npos) {
                continue;
            }
        }
        
        m_filteredIndices.push_back(i);
    }
    
    // Limit visible entries for performance
    if (m_filteredIndices.size() > MAX_VISIBLE_ENTRIES) {
        m_filteredIndices.erase(m_filteredIndices.begin(), 
                               m_filteredIndices.end() - MAX_VISIBLE_ENTRIES);
    }
    
    m_stats.visibleLogEntries = m_filteredIndices.size();
}

std::pair<std::string, std::vector<std::string>> ConsolePanel::ParseCommandLine(const std::string& commandLine) {
    std::vector<std::string> tokens;
    std::istringstream iss(commandLine);
    std::string token;
    
    // Simple tokenization (can be enhanced for quoted strings, etc.)
    while (iss >> token) {
        tokens.push_back(token);
    }
    
    if (tokens.empty()) {
        return {"", {}};
    }
    
    std::string command = tokens[0];
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());
    
    return {command, args};
}

void ConsolePanel::RegisterBuiltInCommands() {
    // Help command
    RegisterCommand({
        "help",
        "Show available commands",
        "help [command]",
        [this](const std::vector<std::string>& args) -> std::string {
            if (!args.empty()) {
                // Show help for specific command
                auto it = m_commands.find(args[0]);
                if (it != m_commands.end()) {
                    std::string help = "Command: " + it->second.name + "\n";
                    help += "Description: " + it->second.description + "\n";
                    help += "Usage: " + it->second.usage;
                    return help;
                } else {
                    return "Unknown command: " + args[0];
                }
            } else {
                // Show all commands
                std::string help = "Available commands:\n";
                for (const auto& [name, cmd] : m_commands) {
                    help += "  " + name + " - " + cmd.description + "\n";
                }
                return help;
            }
        },
        false
    });
    
    // Clear command
    RegisterCommand({
        "clear",
        "Clear the console log",
        "clear",
        [this](const std::vector<std::string>& args) -> std::string {
            Clear();
            return "Console cleared";
        },
        false
    });
    
    // Export command
    RegisterCommand({
        "export",
        "Export console log to file",
        "export <filename> [format]",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.empty()) {
                return "Usage: export <filename> [format]\nFormats: txt, csv";
            }
            
            std::string format = args.size() > 1 ? args[1] : "txt";
            if (ExportLog(args[0], format)) {
                return "Log exported to " + args[0];
            } else {
                return "Export failed";
            }
        },
        false
    });
    
    // Stats command
    RegisterCommand({
        "stats",
        "Show console statistics",
        "stats",
        [this](const std::vector<std::string>& args) -> std::string {
            auto stats = GetStats();
            std::stringstream ss;
            ss << "Console Statistics:\n";
            ss << "  Total Log Entries: " << stats.totalLogEntries << "\n";
            ss << "  Visible Entries: " << stats.visibleLogEntries << "\n";
            ss << "  Commands Executed: " << stats.commandsExecuted << "\n";
            ss << "  Engine Commands: " << stats.engineCommandsExecuted << "\n";
            ss << "  Average Command Time: " << std::fixed << std::setprecision(2) 
               << stats.averageCommandTime << "ms\n";
            
            ss << "  Entries by Level:\n";
            for (const auto& [level, count] : stats.entriesByLevel) {
                ss << "    " << LogLevelToString(level) << ": " << count << "\n";
            }
            
            return ss.str();
        },
        false
    });
    
    // Echo command (for testing)
    RegisterCommand({
        "echo",
        "Echo the provided arguments",
        "echo <text>",
        [](const std::vector<std::string>& args) -> std::string {
            std::string result;
            for (size_t i = 0; i < args.size(); ++i) {
                if (i > 0) result += " ";
                result += args[i];
            }
            return result.empty() ? "echo: no arguments provided" : result;
        },
        false
    });
}

std::vector<std::string> ConsolePanel::GetCompletionSuggestions(const std::string& input) {
    std::vector<std::string> suggestions;
    
    if (input.empty()) {
        // Return all commands
        for (const auto& [name, cmd] : m_commands) {
            suggestions.push_back(name);
        }
    } else {
        // Return commands that start with input
        for (const auto& [name, cmd] : m_commands) {
            if (name.substr(0, input.length()) == input) {
                suggestions.push_back(name);
            }
        }
    }
    
    return suggestions;
}

ImVec4 ConsolePanel::GetLogLevelColor(LogLevel level) const {
    switch (level) {
        case LogLevel::TRACE:    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);  // Gray
        case LogLevel::DEBUG:    return ImVec4(0.6f, 0.8f, 1.0f, 1.0f);  // Light Blue
        case LogLevel::INFO:     return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White
        case LogLevel::WARNING:  return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // Yellow
        case LogLevel::ERROR_:   return ImVec4(1.0f, 0.4f, 0.4f, 1.0f);  // Red
        case LogLevel::CRITICAL: return ImVec4(1.0f, 0.2f, 0.2f, 1.0f);  // Dark Red
        case LogLevel::FATAL:    return ImVec4(1.0f, 0.0f, 1.0f, 1.0f);  // Magenta
        default:                 return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White
    }
}

const char* ConsolePanel::GetCategoryIcon(LogCategory category) const {
    switch (category) {
        case LogCategory::GENERAL:     return "?? ";
        case LogCategory::ASSET:       return "?? ";
        case LogCategory::RENDERING:   return "?? ";
        case LogCategory::ENGINE:      return "? ";
        case LogCategory::UI:          return "??? ";
        case LogCategory::SCRIPTING:   return "?? ";
        case LogCategory::PHYSICS:     return "?? ";
        case LogCategory::AUDIO:       return "?? ";
        case LogCategory::NETWORKING:  return "?? ";
        case LogCategory::PROFILING:   return "?? ";
        case LogCategory::CRASH:       return "?? ";
        default:                       return "?? ";
    }
}

std::string ConsolePanel::FormatTimestamp(const std::chrono::system_clock::time_point& timestamp) const {
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::Milliseconds>(
        timestamp.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    
    return ss.str();
}

void ConsolePanel::UpdateStats() {
    auto now = std::chrono::system_clock::now();
    m_stats.lastActivity = now;
    m_stats.totalLogEntries = m_logEntries.size();
    m_stats.visibleLogEntries = m_filteredIndices.size();
    m_stats.commandsExecuted = m_commandCounter.load();
    m_stats.engineCommandsExecuted = m_engineCommandCounter.load();
    
    // Calculate average command time (simplified)
    if (!m_commandHistory.empty()) {
        float totalTime = 0.0f;
        int validTimes = 0;
        for (const auto& entry : m_commandHistory) {
            if (entry.wasSuccessful) {
                // Simplified time calculation
                totalTime += 1.0f; // Placeholder
                validTimes++;
            }
        }
        if (validTimes > 0) {
            m_stats.averageCommandTime = totalTime / validTimes;
        }
    }
}

void ConsolePanel::ProcessPendingLogEntries() {
    // Get new log entries from logger's memory buffer
    if (m_logger && m_logger->GetMemoryBuffer()) {
        auto newEntries = m_logger->GetMemoryBuffer()->GetEntries(LogLevel::TRACE, LogCategory::GENERAL, 100);
        
        // Add only new entries (simplified check)
        for (const auto& entry : newEntries) {
            // In a real implementation, you'd want to track which entries are new
            // For now, we'll just add them (this might cause duplicates)
            if (m_logEntries.empty() || 
                entry.timestamp > m_logEntries.back().timestamp) {
                AddLogEntry(entry);
            }
        }
    }
}

} // namespace SparkEditor