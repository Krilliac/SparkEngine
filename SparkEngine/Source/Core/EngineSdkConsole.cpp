/**
 * @file EngineSdkConsole.cpp
 * @brief Routes SDK IConsole calls from game modules to the host SimpleConsole.
 */

#include "EngineSdkConsole.h"

#include "Utils/SparkConsole.h"

#include <string>
#include <utility>

bool EngineSdkConsole::RegisterCommand(std::string_view name, CommandHandler handler, std::string_view help,
                                       std::string_view category, std::string_view usage)
{
    // An empty name cannot be typed and an empty handler would throw on dispatch; refuse both up front.
    if (name.empty() || !handler)
        return false;
    return Spark::SimpleConsole::GetInstance().RegisterCommand(std::string(name), std::move(handler), std::string(help),
                                                               std::string(category), std::string(usage));
}

void EngineSdkConsole::UnregisterCommand(std::string_view name)
{
    Spark::SimpleConsole::GetInstance().UnregisterCommand(std::string(name));
}
