/**
 * @file EngineSdkLogger.h
 * @brief The engine's implementation of the public Spark::ILogger SDK interface.
 *
 * Game modules reach it through IEngineContext::GetLogger() (usually via the
 * Spark::ModuleLog helpers in <Spark/ModuleLog.h>) instead of including the
 * private Utils/SparkConsole.h or Utils/LogMacros.h. A module DLL links its own
 * copy of SparkEngineLib, so its Logger/SimpleConsole singletons are DLL-local;
 * calling through the host context's logger reaches the host's sinks.
 *
 * Contract:
 * - Thread affinity: async-safe; Logger is thread-safe.
 * - Ownership: a member of EngineContext; stateless, lives as long as it.
 * - Allocation: one std::string per message, made by the Logger.
 * - Routing: every message goes to Spark::Logger with LogCategory::Game, which
 *   the engine's installed sinks send to the log file, stderr and (through
 *   ConsoleSink) SimpleConsole / SparkConsole.exe.
 */

#pragma once

#include <Spark/ILogger.h>

class EngineSdkLogger final : public Spark::ILogger
{
  public:
    void Info(const char* message) override;
    void Warn(const char* message) override;
    void Error(const char* message) override;
    void Debug(const char* message) override;
};
