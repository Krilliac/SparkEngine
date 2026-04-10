/**
 * @file EditorAutomation.h
 * @brief Editor automation, scripted tools, and batch operation framework
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides an extensible automation system for editor operations. Supports
 * registering named commands, executing scripts of commands in sequence,
 * and building multi-step wizards for common workflows.
 *
 * Designed to be the bridge point for scripting language integration —
 * AngelScript, Lua, or Python can register commands and execute automation
 * scripts through this system.
 *
 * ## Usage
 * @code
 *   auto& automation = SparkEditor::EditorAutomation::GetInstance();
 *   automation.Initialize();
 *
 *   // Register a command
 *   automation.RegisterCommand("select_all_lights", "Select all lights in scene",
 *       [](const auto& args) -> EditorCommandResult {
 *           // Select all light entities...
 *           return {true, "Selected 12 lights"};
 *       });
 *
 *   // Execute a command
 *   auto result = automation.ExecuteCommand("select_all_lights", {});
 *
 *   // Run a script (sequence of commands)
 *   automation.RunScript(R"(
 *       select_all_lights
 *       set_property intensity 2.5
 *       save_scene
 *   )");
 *
 *   // Create a wizard
 *   EditorWizard wizard("Setup Lighting");
 *   wizard.AddStep("Select lights", "select_all_lights");
 *   wizard.AddStep("Set intensity", "set_property intensity 1.0");
 *   automation.RegisterWizard(wizard);
 * @endcode
 *
 * ## Scripting Language Integration
 *
 * To expose editor automation to script runtimes:
 *
 * ### AngelScript Integration
 * ```cpp
 * // In AngelScriptEngine::BindEditorAPI():
 * engine->RegisterGlobalFunction(
 *     "bool EditorExec(const string &in cmd)",
 *     asFUNCTION(+[](const std::string& cmd) -> bool {
 *         auto result = EditorAutomation::GetInstance().ExecuteCommand(cmd, {});
 *         return result.success;
 *     }), asCALL_CDECL);
 * ```
 *
 * ### Python Integration (via pybind11)
 * ```cpp
 * // In a future Python plugin:
 * py::module m = py::module::create_extension_module("spark_editor", ...);
 * m.def("exec_command", [](const std::string& cmd) {
 *     return EditorAutomation::GetInstance().ExecuteCommand(cmd, {}).success;
 * });
 * ```
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

namespace SparkEditor
{

    /** @brief Result of an editor command execution */
    struct EditorCommandResult
    {
        bool success = false;    ///< Whether the command succeeded
        std::string message;     ///< Human-readable result or error message
        std::string returnValue; ///< Optional structured return value
    };

    /** @brief A registered editor command */
    struct EditorCommandInfo
    {
        std::string name;        ///< Command name (used in scripts)
        std::string description; ///< Human-readable description
        std::string usage;       ///< Usage string (e.g. "set_property <path> <value>")
        std::string category;    ///< Category for grouping (e.g. "Scene", "Selection", "Assets")
    };

    /** @brief Callback type for editor commands */
    using EditorCommandHandler = std::function<EditorCommandResult(const std::vector<std::string>& args)>;

    /** @brief A step in an editor wizard */
    struct WizardStep
    {
        std::string name;        ///< Step display name
        std::string description; ///< Step description
        std::string command;     ///< Command string to execute
        bool optional = false;   ///< Whether this step can be skipped
    };

    /** @brief A multi-step editor wizard */
    struct EditorWizard
    {
        std::string name;              ///< Wizard display name
        std::string description;       ///< Wizard description
        std::vector<WizardStep> steps; ///< Ordered steps
        uint32_t currentStep = 0;      ///< Current step index (during execution)
        bool isRunning = false;        ///< Whether the wizard is currently executing

        EditorWizard() = default;
        explicit EditorWizard(const std::string& wizardName) : name(wizardName) {}

        /** @brief Add a step to the wizard */
        void AddStep(const std::string& stepName, const std::string& command, const std::string& desc = "",
                     bool isOptional = false)
        {
            steps.push_back({stepName, desc, command, isOptional});
        }
    };

    /** @brief Record of an executed command for history/undo */
    struct CommandHistoryEntry
    {
        std::string command;                             ///< Full command string
        EditorCommandResult result;                      ///< Execution result
        std::chrono::steady_clock::time_point timestamp; ///< When it was executed
    };

    /** @brief Script execution result */
    struct ScriptResult
    {
        bool success = false;            ///< Whether all commands succeeded
        uint32_t commandsExecuted = 0;   ///< Number of commands that ran
        uint32_t commandsFailed = 0;     ///< Number of commands that failed
        std::vector<std::string> errors; ///< Error messages from failed commands
    };

    /**
     * @brief Editor automation and scripted tool framework
     *
     * Central registry for editor commands that can be invoked programmatically,
     * from scripts, or from wizard UIs. All editor operations that modify the
     * scene should be registered here for automation support.
     *
     * Thread safety: Main thread only.
     */
    class EditorAutomation
    {
      public:
        static EditorAutomation& GetInstance()
        {
            static EditorAutomation instance;
            return instance;
        }

        /** @brief Initialize the automation system with built-in commands */
        void Initialize()
        {
            m_commands.clear();
            m_handlers.clear();
            m_wizards.clear();
            m_history.clear();
            m_maxHistory = 1000;
            m_initialized = true;

            RegisterBuiltInCommands();
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorAutomation initialized with %zu built-in commands",
                           m_commands.size());
        }

        /** @brief Shut down */
        void Shutdown()
        {
            m_commands.clear();
            m_handlers.clear();
            m_wizards.clear();
            m_history.clear();
            m_initialized = false;
        }

        /**
         * @brief Register an editor command
         * @param name Command name (lowercase, underscored)
         * @param description Human-readable description
         * @param handler Callback to execute
         * @param usage Usage string
         * @param category Command category
         * @return true if registered successfully
         */
        bool RegisterCommand(const std::string& name, const std::string& description, EditorCommandHandler handler,
                             const std::string& usage = "", const std::string& category = "General")
        {
            if (name.empty() || !handler)
                return false;

            EditorCommandInfo info;
            info.name = name;
            info.description = description;
            info.usage = usage;
            info.category = category;
            m_commands[name] = std::move(info);
            m_handlers[name] = std::move(handler);
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "EditorAutomation: RegisterCommand '%s'", name.c_str());
            return true;
        }

        /** @brief Unregister a command */
        void UnregisterCommand(const std::string& name)
        {
            m_commands.erase(name);
            m_handlers.erase(name);
        }

        /**
         * @brief Execute a command by name with arguments
         * @param name Command name
         * @param args Command arguments
         * @return Execution result
         */
        EditorCommandResult ExecuteCommand(const std::string& name, const std::vector<std::string>& args)
        {
            auto it = m_handlers.find(name);
            if (it == m_handlers.end())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "EditorAutomation: Unknown command '%s'", name.c_str());
                return {false, "Unknown command: " + name, ""};
            }

            SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorAutomation: ExecuteCommand '%s'", name.c_str());
            auto result = it->second(args);

            // Record in history
            std::string fullCmd = name;
            for (const auto& arg : args)
                fullCmd += " " + arg;
            m_history.push_back({fullCmd, result, std::chrono::steady_clock::now()});
            if (m_history.size() > m_maxHistory)
                m_history.erase(m_history.begin());

            return result;
        }

        /**
         * @brief Execute a command from a single string (parsed into name + args)
         * @param commandLine Full command string (e.g. "set_property intensity 2.5")
         * @return Execution result
         */
        EditorCommandResult ExecuteCommandLine(const std::string& commandLine)
        {
            auto tokens = Tokenize(commandLine);
            if (tokens.empty())
                return {false, "Empty command", ""};

            std::string name = tokens[0];
            std::vector<std::string> args(tokens.begin() + 1, tokens.end());
            return ExecuteCommand(name, args);
        }

        /**
         * @brief Run a script (multiple commands, one per line)
         * @param script Multi-line script text
         * @return Script execution result
         */
        ScriptResult RunScript(const std::string& script)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorAutomation: RunScript started");
            ScriptResult result;
            std::istringstream stream(script);
            std::string line;

            while (std::getline(stream, line))
            {
                // Trim whitespace
                auto start = line.find_first_not_of(" \t");
                if (start == std::string::npos)
                    continue;
                line = line.substr(start);

                // Skip comments and empty lines
                if (line.empty() || line[0] == '#' || line[0] == '/')
                    continue;

                auto cmdResult = ExecuteCommandLine(line);
                result.commandsExecuted++;

                if (!cmdResult.success)
                {
                    result.commandsFailed++;
                    result.errors.push_back("Line: '" + line + "' — " + cmdResult.message);
                }
            }

            result.success = (result.commandsFailed == 0);
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorAutomation: RunScript completed — %u executed, %u failed",
                           result.commandsExecuted, result.commandsFailed);
            return result;
        }

        // ====================================================================
        // Wizards
        // ====================================================================

        /** @brief Register a wizard */
        void RegisterWizard(const EditorWizard& wizard) { m_wizards[wizard.name] = wizard; }

        /** @brief Get a wizard by name */
        const EditorWizard* GetWizard(const std::string& name) const
        {
            auto it = m_wizards.find(name);
            return (it != m_wizards.end()) ? &it->second : nullptr;
        }

        /** @brief Execute all steps of a wizard */
        ScriptResult RunWizard(const std::string& name)
        {
            auto it = m_wizards.find(name);
            if (it == m_wizards.end())
                return {false, 0, 1, {"Wizard not found: " + name}};

            ScriptResult result;
            for (const auto& step : it->second.steps)
            {
                auto cmdResult = ExecuteCommandLine(step.command);
                result.commandsExecuted++;
                if (!cmdResult.success)
                {
                    result.commandsFailed++;
                    result.errors.push_back("Step '" + step.name + "': " + cmdResult.message);
                    if (!step.optional)
                        break; // Stop on non-optional failure
                }
            }
            result.success = (result.commandsFailed == 0);
            return result;
        }

        /** @brief Get all wizard names */
        std::vector<std::string> GetWizardNames() const
        {
            std::vector<std::string> names;
            for (const auto& [name, wiz] : m_wizards)
                names.push_back(name);
            return names;
        }

        // ====================================================================
        // Query
        // ====================================================================

        /** @brief Get all registered command names */
        std::vector<std::string> GetCommandNames() const
        {
            std::vector<std::string> names;
            for (const auto& [name, info] : m_commands)
                names.push_back(name);
            std::sort(names.begin(), names.end());
            return names;
        }

        /** @brief Get commands by category */
        std::vector<std::string> GetCommandsByCategory(const std::string& category) const
        {
            std::vector<std::string> names;
            for (const auto& [name, info] : m_commands)
            {
                if (info.category == category)
                    names.push_back(name);
            }
            return names;
        }

        /** @brief Get command info */
        const EditorCommandInfo* GetCommandInfo(const std::string& name) const
        {
            auto it = m_commands.find(name);
            return (it != m_commands.end()) ? &it->second : nullptr;
        }

        /** @brief Get command history */
        const std::vector<CommandHistoryEntry>& GetHistory() const { return m_history; }

        /** @brief Get registered command count */
        size_t GetCommandCount() const { return m_commands.size(); }

        /** @brief Check if a command exists */
        bool HasCommand(const std::string& name) const { return m_commands.count(name) > 0; }

        /** @brief Get console-friendly status */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[Automation] Not initialized";
            return "[Automation] Commands: " + std::to_string(m_commands.size()) +
                   " | Wizards: " + std::to_string(m_wizards.size()) +
                   " | History: " + std::to_string(m_history.size());
        }

      private:
        EditorAutomation() = default;

        void RegisterBuiltInCommands()
        {
            RegisterCommand(
                "echo", "Print a message",
                [](const std::vector<std::string>& args) -> EditorCommandResult
                {
                    std::string msg;
                    for (const auto& a : args)
                        msg += a + " ";
                    return {true, msg, msg};
                },
                "echo <message>", "General");

            RegisterCommand(
                "help", "List all commands",
                [this](const std::vector<std::string>&) -> EditorCommandResult
                {
                    std::string list;
                    for (const auto& [name, info] : m_commands)
                        list += name + " — " + info.description + "\n";
                    return {true, list, ""};
                },
                "help", "General");

            RegisterCommand(
                "wait", "Placeholder for delayed operations", [](const std::vector<std::string>&) -> EditorCommandResult
                { return {true, "Wait complete", ""}; }, "wait", "General");

            RegisterCommand(
                "noop", "Do nothing (useful for wizard placeholders)",
                [](const std::vector<std::string>&) -> EditorCommandResult { return {true, "", ""}; }, "noop",
                "General");
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
        size_t m_maxHistory = 1000;
        std::unordered_map<std::string, EditorCommandInfo> m_commands;
        std::unordered_map<std::string, EditorCommandHandler> m_handlers;
        std::unordered_map<std::string, EditorWizard> m_wizards;
        std::vector<CommandHistoryEntry> m_history;
    };

} // namespace SparkEditor
