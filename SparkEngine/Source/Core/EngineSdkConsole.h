/**
 * @file EngineSdkConsole.h
 * @brief The engine's implementation of the public Spark::IConsole SDK interface.
 *
 * Game modules reach it through IEngineContext::GetConsole() instead of
 * including the private Utils/SparkConsole.h. The calls run in the host image,
 * so they reach the host's SimpleConsole, the registry SparkConsole.exe and the
 * in-game console dispatch into.
 *
 * Contract:
 * - Thread affinity: game thread (module OnLoad/OnUnload and command handlers);
 *   SimpleConsole itself is mutex-protected.
 * - Ownership: a member of EngineContext; stateless, lives as long as it.
 * - Allocation: copies the name, help, category and usage into std::strings
 *   for the registry on each registration, and a printed line's text and type
 *   into the console history.
 * - Print forwards to SimpleConsole::Log, which also mirrors the line to the
 *   SparkConsole.exe window; it is safe from any thread.
 * - Registration owner: the call carries no owner token, so SimpleConsole
 *   attributes it to the module whose lifecycle callback is running
 *   (ModuleManager's ScopedRegistrationOwner), exactly like a module calling
 *   SimpleConsole directly.
 */

#pragma once

#include <Spark/IConsole.h>

class EngineSdkConsole final : public Spark::IConsole
{
  public:
    bool RegisterCommand(std::string_view name, CommandHandler handler, std::string_view help,
                         std::string_view category, std::string_view usage) override;
    void UnregisterCommand(std::string_view name) override;
    void Print(std::string_view message, std::string_view type) override;
};
