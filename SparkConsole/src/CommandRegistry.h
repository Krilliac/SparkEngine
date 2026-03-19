/**
 * @file CommandRegistry.h
 * @brief Console command registration and dispatch for the SparkConsole subprocess.
 *
 * Commands are registered with a name, description, usage string, and handler
 * callback. When the user types a command, ConsoleApp looks it up here and
 * invokes the handler. Commands that are not registered locally are forwarded
 * to the engine process via the stdout pipe.
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory>

/// Ordered list of string arguments passed to a command handler.
using CommandArgs = std::vector<std::string>;

/// Callback invoked when a command is executed; returns a result string for display.
using CommandHandler = std::function<std::string(const CommandArgs&)>;

/**
 * @brief Stores and dispatches named console commands.
 *
 * Each command is identified by a unique name (case-sensitive). The registry
 * owns the handler callbacks and provides help-text lookup for tab completion.
 */
class CommandRegistry
{
  public:
    CommandRegistry();
    ~CommandRegistry();

    /// Metadata for a single registered command.
    struct CommandInfo
    {
        std::string name;        ///< Command name (e.g. "help", "quit", "stats").
        std::string description; ///< One-line help text shown in the command listing.
        std::string usage;       ///< Usage pattern (e.g. "stats [category]").
        CommandHandler handler;  ///< Callback invoked when the command is executed.
    };

    /**
     * @brief Register a new command.
     * @param name        Unique command name.
     * @param description One-line help text.
     * @param usage       Usage pattern string.
     * @param handler     Callback invoked with parsed arguments.
     */
    void RegisterCommand(const std::string& name, const std::string& description, const std::string& usage,
                         CommandHandler handler);

    /**
     * @brief Execute a registered command by name.
     * @param name  Command name to execute.
     * @param args  Arguments passed to the handler.
     * @return The handler's result string, or an error message if not found.
     */
    std::string ExecuteCommand(const std::string& name, const CommandArgs& args);

    /** @brief Check if a command is registered. */
    bool HasCommand(const std::string& name) const;

    /** @brief Get a copy of all registered commands (for help/listing). */
    std::vector<CommandInfo> GetAllCommands() const;

    /**
     * @brief Get formatted help text for a specific command.
     * @param name  Command name.
     * @return Help string including description and usage, or empty if not found.
     */
    std::string GetCommandHelp(const std::string& name) const;

  private:
    std::unordered_map<std::string, CommandInfo> m_commands; ///< Name -> command lookup table.
};
