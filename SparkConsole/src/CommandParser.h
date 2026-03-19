/**
 * @file CommandParser.h
 * @brief Splits raw command-line strings into a command name and argument list.
 *
 * Used by ConsoleApp to parse user input before dispatching to the
 * CommandRegistry. Handles quoted arguments (e.g. `say "hello world"`).
 */

#pragma once

#include "CommandRegistry.h"
#include <string>
#include <vector>

class CommandParser
{
  public:
    /**
     * @brief Parse a command line into a command name and argument vector.
     * @param commandLine  Raw input string (e.g. "teleport 10 20 30").
     * @param commandName  [out] Receives the first token (the command name).
     * @param args         [out] Receives the remaining tokens (arguments).
     * @return True if at least a command name was extracted.
     */
    static bool ParseCommandLine(const std::string& commandLine, std::string& commandName, CommandArgs& args);

    /**
     * @brief Split a command line into tokens, respecting quoted strings.
     *
     * Tokens separated by whitespace are returned individually. Text within
     * double quotes is treated as a single token (quotes are stripped).
     *
     * @param commandLine  Raw input string.
     * @return Vector of string tokens.
     */
    static std::vector<std::string> Tokenize(const std::string& commandLine);
};
