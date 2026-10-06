/**
 * @file ConsoleHistoryPolicy.h
 * @brief What SparkConsole's command history may keep of a typed line.
 *
 * The engine decides which commands carry credentials (mmo_login, tf_login,
 * tf_register, ... are registered with SimpleConsole::RegisterSensitiveCommand)
 * and redacts them in its own history. That metadata never crosses the pipe, so
 * SparkConsole cannot tell a credential command from any other engine command.
 * It therefore fails closed, the way SimpleConsole::RedactSensitiveArguments
 * treats a command it does not recognise: arguments are kept only for commands
 * this console runs itself; every other line keeps just the name the user typed.
 *
 * Thread affinity: none (pure functions). Allocation: the returned string.
 */

#pragma once

#include "CommandParser.h"

#include <string>

namespace ConsoleHistoryPolicy
{

    /// Replaces the arguments of a history entry whose arguments are not trusted.
    inline constexpr const char* kRedactedArguments = "<arguments-redacted>";

    /**
     * @brief The form of @p typedLine that command history may store.
     *
     * @param typedLine        The line exactly as the user typed it.
     * @param argumentsTrusted True only when the alias-resolved command is a
     *                         console-local command whose arguments cannot hold
     *                         a secret (never an engine command, never `alias`,
     *                         whose arguments are a whole command line).
     * @return @p typedLine when trusted; otherwise the typed command name, plus
     *         kRedactedArguments when the line had arguments. Never an argument.
     */
    [[nodiscard]] inline std::string EntryFor(const std::string& typedLine, bool argumentsTrusted)
    {
        if (argumentsTrusted)
            return typedLine;

        std::string name;
        CommandArgs args;
        if (!CommandParser::ParseCommandLine(typedLine, name, args))
            return {};
        // Keep only the name the user typed: an alias may itself embed a credential.
        return args.empty() ? name : name + " " + kRedactedArguments;
    }

} // namespace ConsoleHistoryPolicy
