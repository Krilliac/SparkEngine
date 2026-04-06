/**
 * @file InGameConsole.h
 * @brief In-game developer console overlay (Quake-style drop-down console)
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a runtime in-game console that renders as an overlay on the game
 * viewport. Unlike SparkConsole (a separate process), this runs inside the
 * engine and provides immediate command execution, log viewing, and
 * auto-completion without leaving the game.
 *
 * Toggled with the tilde (~) key by default. Renders using the engine's
 * immediate-mode UI or text rendering system.
 *
 * ## Usage
 * @code
 *   auto& console = Spark::InGameConsole::GetInstance();
 *   console.Initialize();
 *
 *   // Register commands
 *   console.RegisterCommand("god", "Toggle god mode", [](const auto& args) {
 *       player.godMode = !player.godMode;
 *       return player.godMode ? "God mode ON" : "God mode OFF";
 *   });
 *
 *   console.RegisterCommand("give", "Give item", [](const auto& args) {
 *       if (args.empty()) return std::string("Usage: give <item_id> [count]");
 *       // ... give item
 *       return std::string("Gave " + args[0]);
 *   }, "give <item_id> [count]");
 *
 *   // Print to console
 *   console.Print("Welcome to SparkEngine!");
 *   console.PrintWarning("Low FPS detected");
 *   console.PrintError("Asset failed to load: missing.png");
 *
 *   // Each frame:
 *   console.Update(deltaTime);
 *   if (console.IsOpen())
 *       console.Render();  // Draw the overlay
 * @endcode
 */

#pragma once

#include "LogMacros.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /** @brief Log level for console messages */
    enum class ConsoleLogLevel
    {
        Info,    ///< Normal message (white)
        Warning, ///< Warning (yellow)
        Error,   ///< Error (red)
        Success, ///< Success (green)
        Command  ///< User command echo (gray)
    };

    /** @brief A single line in the console output */
    struct ConsoleLine
    {
        std::string text;
        ConsoleLogLevel level = ConsoleLogLevel::Info;
        float timestamp = 0.0f; ///< Engine time when printed
    };

    /** @brief Registered console command info */
    struct ConsoleCommandInfo
    {
        std::string name;
        std::string description;
        std::string usage;
    };

    /** @brief Console command handler — receives args, returns output string */
    using ConsoleCommandHandler = std::function<std::string(const std::vector<std::string>&)>;

    /**
     * @brief In-game developer console overlay
     *
     * A Quake-style drop-down console that renders as an overlay on the game.
     * Supports command registration, execution, auto-completion, command history
     * (up/down arrows), scrollable output, and colored log levels.
     *
     * Thread safety: Not thread-safe. Call from main thread only. Use
     * SimpleConsole for thread-safe logging from background threads.
     */
    class InGameConsole
    {
      public:
        static InGameConsole& GetInstance()
        {
            static InGameConsole instance;
            return instance;
        }

        /** @brief Initialize the in-game console */
        void Initialize()
        {
            m_outputLines.clear();
            m_commandHistory.clear();
            m_commands.clear();
            m_handlers.clear();
            m_inputBuffer.clear();
            m_historyIndex = -1;
            m_scrollOffset = 0;
            m_isOpen = false;
            m_toggleKey = '`'; // Tilde/backtick
            m_maxOutputLines = 1000;
            m_maxHistorySize = 100;
            m_openAmount = 0.0f;
            m_animationSpeed = 8.0f;
            m_totalTime = 0.0f;
            m_initialized = true;

            RegisterBuiltInCommands();

            SPARK_LOG_INFO(Spark::LogCategory::Core, "InGameConsole initialized (%zu built-in commands)",
                           m_commands.size());
            Print("SparkEngine In-Game Console initialized. Type 'help' for commands.");
        }

        /** @brief Shut down */
        void Shutdown()
        {
            m_outputLines.clear();
            m_commandHistory.clear();
            m_commands.clear();
            m_handlers.clear();
            m_initialized = false;
        }

        // ====================================================================
        // Command registration
        // ====================================================================

        /**
         * @brief Register a console command
         * @param name Command name
         * @param description Help text
         * @param handler Callback that receives args and returns output
         * @param usage Optional usage string
         */
        void RegisterCommand(const std::string& name, const std::string& description, ConsoleCommandHandler handler,
                             const std::string& usage = "")
        {
            ConsoleCommandInfo info;
            info.name = name;
            info.description = description;
            info.usage = usage.empty() ? name : usage;
            m_commands[name] = std::move(info);
            m_handlers[name] = std::move(handler);
            SPARK_LOG_DEBUG(Spark::LogCategory::Core, "InGameConsole: RegisterCommand '%s'", name.c_str());
        }

        /** @brief Unregister a command */
        void UnregisterCommand(const std::string& name)
        {
            m_commands.erase(name);
            m_handlers.erase(name);
        }

        // ====================================================================
        // Output
        // ====================================================================

        /** @brief Print an info message */
        void Print(const std::string& text) { AddLine(text, ConsoleLogLevel::Info); }

        /** @brief Print a warning message */
        void PrintWarning(const std::string& text) { AddLine(text, ConsoleLogLevel::Warning); }

        /** @brief Print an error message */
        void PrintError(const std::string& text) { AddLine(text, ConsoleLogLevel::Error); }

        /** @brief Print a success message */
        void PrintSuccess(const std::string& text) { AddLine(text, ConsoleLogLevel::Success); }

        // ====================================================================
        // Input handling
        // ====================================================================

        /**
         * @brief Process a character input
         * @param ch Character typed
         */
        void OnCharInput(char ch)
        {
            if (!m_isOpen)
                return;

            if (ch == '\n' || ch == '\r')
            {
                ExecuteInput();
            }
            else if (ch == '\b')
            {
                if (!m_inputBuffer.empty())
                    m_inputBuffer.pop_back();
            }
            else if (ch == '\t')
            {
                AutoComplete();
            }
            else if (ch >= 32 && ch < 127 && ch != m_toggleKey)
            {
                m_inputBuffer += ch;
            }
        }

        /** @brief Navigate command history (up = -1, down = +1) */
        void NavigateHistory(int direction)
        {
            if (m_commandHistory.empty())
                return;

            m_historyIndex += direction;
            m_historyIndex = std::max(-1, std::min(m_historyIndex, static_cast<int>(m_commandHistory.size()) - 1));

            if (m_historyIndex >= 0)
                m_inputBuffer = m_commandHistory[static_cast<size_t>(m_historyIndex)];
            else
                m_inputBuffer.clear();
        }

        /**
         * @brief Toggle console open/closed
         * @param key The key that was pressed
         * @return true if the key was consumed (was the toggle key)
         */
        bool OnKeyPress(char key)
        {
            if (key == m_toggleKey)
            {
                m_isOpen = !m_isOpen;
                return true;
            }
            return false;
        }

        // ====================================================================
        // Update and render
        // ====================================================================

        /**
         * @brief Update console state (animation, timing)
         * @param deltaTime Frame delta time
         */
        void Update(float deltaTime)
        {
            if (!m_initialized)
                return;

            m_totalTime += deltaTime;

            // Animate open/close
            float target = m_isOpen ? 1.0f : 0.0f;
            float speed = m_animationSpeed * deltaTime;
            if (m_openAmount < target)
                m_openAmount = std::min(m_openAmount + speed, target);
            else if (m_openAmount > target)
                m_openAmount = std::max(m_openAmount - speed, target);
        }

        /**
         * @brief Check if the console overlay should be rendered
         * @return true if open or animating
         */
        bool ShouldRender() const { return m_openAmount > 0.01f; }

        /** @brief Check if the console is open */
        bool IsOpen() const { return m_isOpen; }

        /** @brief Get the open amount for animation (0 = closed, 1 = fully open) */
        float GetOpenAmount() const { return m_openAmount; }

        /** @brief Get the output lines for rendering */
        const std::deque<ConsoleLine>& GetOutputLines() const { return m_outputLines; }

        /** @brief Get the current input buffer text */
        const std::string& GetInputBuffer() const { return m_inputBuffer; }

        /** @brief Get the scroll offset for the output */
        int GetScrollOffset() const { return m_scrollOffset; }

        /** @brief Scroll the output */
        void Scroll(int lines) { m_scrollOffset = std::max(0, m_scrollOffset + lines); }

        // ====================================================================
        // Execution
        // ====================================================================

        /**
         * @brief Execute a command string directly
         * @param commandLine Full command string
         * @return Command output
         */
        std::string Execute(const std::string& commandLine)
        {
            auto tokens = Tokenize(commandLine);
            if (tokens.empty())
                return "";

            std::string name = tokens[0];
            std::vector<std::string> args(tokens.begin() + 1, tokens.end());

            auto it = m_handlers.find(name);
            if (it == m_handlers.end())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "InGameConsole: Unknown command '%s'", name.c_str());
                return "Unknown command: " + name + ". Type 'help' for available commands.";
            }

            return it->second(args);
        }

        // ====================================================================
        // Configuration
        // ====================================================================

        /** @brief Set the toggle key (default: '`') */
        void SetToggleKey(char key) { m_toggleKey = key; }

        /** @brief Set max output lines before pruning */
        void SetMaxOutputLines(size_t max) { m_maxOutputLines = max; }

        /** @brief Clear all output */
        void ClearOutput() { m_outputLines.clear(); }

        /** @brief Get registered command count */
        size_t GetCommandCount() const { return m_commands.size(); }

        /** @brief Get all command names for auto-complete */
        std::vector<std::string> GetCommandNames() const
        {
            std::vector<std::string> names;
            for (const auto& [name, info] : m_commands)
                names.push_back(name);
            std::sort(names.begin(), names.end());
            return names;
        }

        /** @brief Get console-friendly status */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[InGameConsole] Not initialized";
            return "[InGameConsole] " + std::string(m_isOpen ? "OPEN" : "CLOSED") +
                   " | Commands: " + std::to_string(m_commands.size()) +
                   " | Lines: " + std::to_string(m_outputLines.size()) +
                   " | History: " + std::to_string(m_commandHistory.size());
        }

      private:
        InGameConsole() = default;

        void AddLine(const std::string& text, ConsoleLogLevel level)
        {
            m_outputLines.push_back({text, level, m_totalTime});
            if (m_outputLines.size() > m_maxOutputLines)
                m_outputLines.pop_front();
            m_scrollOffset = 0; // Auto-scroll to bottom
        }

        void ExecuteInput()
        {
            if (m_inputBuffer.empty())
                return;

            // Echo the command
            AddLine("> " + m_inputBuffer, ConsoleLogLevel::Command);

            // Add to history
            if (m_commandHistory.empty() || m_commandHistory.front() != m_inputBuffer)
            {
                m_commandHistory.insert(m_commandHistory.begin(), m_inputBuffer);
                if (m_commandHistory.size() > m_maxHistorySize)
                    m_commandHistory.pop_back();
            }
            m_historyIndex = -1;

            // Execute
            SPARK_LOG_INFO(Spark::LogCategory::Core, "InGameConsole: ExecuteInput '%s'", m_inputBuffer.c_str());
            std::string output = Execute(m_inputBuffer);
            if (!output.empty())
            {
                // Determine level from output
                ConsoleLogLevel level = ConsoleLogLevel::Info;
                if (output.find("Error") != std::string::npos || output.find("Unknown") != std::string::npos)
                    level = ConsoleLogLevel::Error;
                else if (output.find("Warning") != std::string::npos)
                    level = ConsoleLogLevel::Warning;
                AddLine(output, level);
            }

            m_inputBuffer.clear();
        }

        void AutoComplete()
        {
            if (m_inputBuffer.empty())
                return;

            std::vector<std::string> matches;
            for (const auto& [name, info] : m_commands)
            {
                if (name.substr(0, m_inputBuffer.size()) == m_inputBuffer)
                    matches.push_back(name);
            }

            if (matches.size() == 1)
            {
                m_inputBuffer = matches[0] + " ";
            }
            else if (matches.size() > 1)
            {
                std::string list;
                for (const auto& m : matches)
                    list += m + "  ";
                AddLine(list, ConsoleLogLevel::Info);
            }
        }

        void RegisterBuiltInCommands()
        {
            RegisterCommand("help", "List all commands",
                            [this](const std::vector<std::string>&)
                            {
                                std::string output;
                                for (const auto& [name, info] : m_commands)
                                    output += "  " + name + " — " + info.description + "\n";
                                return output;
                            });

            RegisterCommand("clear", "Clear console output",
                            [this](const std::vector<std::string>&)
                            {
                                ClearOutput();
                                return std::string();
                            });

            RegisterCommand(
                "echo", "Print a message",
                [](const std::vector<std::string>& args)
                {
                    std::string msg;
                    for (const auto& a : args)
                        msg += a + " ";
                    return msg;
                },
                "echo <message>");

            RegisterCommand("quit", "Exit the engine", [](const std::vector<std::string>&)
                            { return std::string("Use Alt+F4 or close the window to exit."); });

            RegisterCommand("history", "Show command history",
                            [this](const std::vector<std::string>&)
                            {
                                std::string output;
                                for (size_t i = 0; i < m_commandHistory.size() && i < 20; ++i)
                                    output += "  " + std::to_string(i) + ": " + m_commandHistory[i] + "\n";
                                return output;
                            });
        }

        static std::vector<std::string> Tokenize(const std::string& line)
        {
            std::vector<std::string> tokens;
            std::istringstream stream(line);
            std::string token;
            while (stream >> token)
                tokens.push_back(token);
            return tokens;
        }

        bool m_initialized = false;
        bool m_isOpen = false;
        float m_openAmount = 0.0f;
        float m_animationSpeed = 8.0f;
        float m_totalTime = 0.0f;
        char m_toggleKey = '`';
        size_t m_maxOutputLines = 1000;
        size_t m_maxHistorySize = 100;
        int m_scrollOffset = 0;
        int m_historyIndex = -1;

        std::string m_inputBuffer;
        std::deque<ConsoleLine> m_outputLines;
        std::vector<std::string> m_commandHistory;

        std::unordered_map<std::string, ConsoleCommandInfo> m_commands;
        std::unordered_map<std::string, ConsoleCommandHandler> m_handlers;
    };

} // namespace Spark
