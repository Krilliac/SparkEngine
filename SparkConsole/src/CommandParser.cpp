#include "CommandParser.h"
#include <sstream>
#include <vector>
#include <string>
#include <cctype>

bool CommandParser::ParseCommandLine(const std::string& commandLine, std::string& commandName, CommandArgs& args)
{
    auto tokens = Tokenize(commandLine);
    if (tokens.empty())
        return false;

    commandName = tokens[0];
    args.clear();

    // Copy all tokens except the first one (command name) as string arguments
    for (size_t i = 1; i < tokens.size(); ++i)
    {
        args.push_back(tokens[i]);
    }

    return true;
}

std::vector<std::string> CommandParser::Tokenize(const std::string& commandLine)
{
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;

    for (char c : commandLine)
    {
        if (c == '"')
        {
            inQuotes = !inQuotes;
        }
        else if (std::isspace(static_cast<unsigned char>(c)) && !inQuotes)
        {
            if (!current.empty())
            {
                tokens.push_back(current);
                current.clear();
            }
        }
        else
        {
            current += c;
        }
    }

    if (!current.empty())
    {
        tokens.push_back(current);
    }

    return tokens;
}