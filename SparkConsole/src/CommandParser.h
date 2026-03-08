#pragma once

#include "CommandRegistry.h"
#include <string>
#include <vector>

class CommandParser
{
  public:
    // Parse a command line into command name and arguments
    static bool ParseCommandLine(const std::string& commandLine, std::string& commandName, CommandArgs& args);

    // Tokenize a command line respecting quotes
    static std::vector<std::string> Tokenize(const std::string& commandLine);
};