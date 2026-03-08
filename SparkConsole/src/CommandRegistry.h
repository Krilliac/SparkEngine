#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory>

// Simplified command arguments - use vector of strings for consistency with engine
using CommandArgs = std::vector<std::string>;
using CommandHandler = std::function<std::string(const CommandArgs&)>;

class CommandRegistry
{
  public:
    CommandRegistry();
    ~CommandRegistry();

    // Command metadata
    struct CommandInfo
    {
        std::string name;
        std::string description;
        std::string usage;
        CommandHandler handler;
    };

    // Register a command
    void RegisterCommand(const std::string& name, const std::string& description, const std::string& usage,
                         CommandHandler handler);

    // Execute a command with arguments
    std::string ExecuteCommand(const std::string& name, const CommandArgs& args);

    // Get command information
    bool HasCommand(const std::string& name) const;
    std::vector<CommandInfo> GetAllCommands() const;
    std::string GetCommandHelp(const std::string& name) const;

  private:
    std::unordered_map<std::string, CommandInfo> m_commands;
};