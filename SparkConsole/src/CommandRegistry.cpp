#include "CommandRegistry.h"
#include <sstream>
#include <vector>
#include <string>

CommandRegistry::CommandRegistry() {}

CommandRegistry::~CommandRegistry() {}

void CommandRegistry::RegisterCommand(const std::string& name, const std::string& description, const std::string& usage,
                                      CommandHandler handler)
{
    CommandInfo info;
    info.name = name;
    info.description = description;
    info.usage = usage;
    info.handler = handler;

    m_commands[name] = info;
}

std::string CommandRegistry::ExecuteCommand(const std::string& name, const CommandArgs& args)
{
    auto it = m_commands.find(name);
    if (it == m_commands.end())
    {
        return "Unknown command: " + name + ". Type 'help' for available commands.";
    }

    try
    {
        return it->second.handler(args);
    }
    catch (const std::exception& e)
    {
        return "Error executing command '" + name + "': " + e.what();
    }
    catch (...)
    {
        return "Unknown error executing command '" + name + "'";
    }
}

bool CommandRegistry::HasCommand(const std::string& name) const
{
    return m_commands.find(name) != m_commands.end();
}

std::vector<CommandRegistry::CommandInfo> CommandRegistry::GetAllCommands() const
{
    std::vector<CommandInfo> result;
    result.reserve(m_commands.size());

    for (const auto& pair : m_commands)
    {
        result.push_back(pair.second);
    }

    return result;
}

std::string CommandRegistry::GetCommandHelp(const std::string& name) const
{
    auto it = m_commands.find(name);
    if (it == m_commands.end())
    {
        return "Unknown command: " + name;
    }

    std::stringstream ss;
    ss << "Command: " << it->second.name << "\n";

    if (!it->second.description.empty())
    {
        ss << "Description: " << it->second.description << "\n";
    }

    if (!it->second.usage.empty())
    {
        ss << "Usage: " << it->second.usage << "\n";
    }

    return ss.str();
}