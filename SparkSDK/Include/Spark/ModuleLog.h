/**
 * @file ModuleLog.h
 * @brief std::format logging helpers for game modules, routed through IEngineContext::GetLogger().
 *
 * A module logs through the host's ILogger, so its messages reach the host's
 * log file, stderr and console, without including the engine-private
 * Utils/SparkConsole.h or Utils/LogMacros.h:
 * @code
 *   #include <Spark/ModuleLog.h>
 *   Spark::ModuleLog::Info(context, "[RTS] Match saved to slot: {}", slot);
 *   Spark::ModuleLog::Warn(context, "[RTS] Autosave failed");
 * @endcode
 *
 * Every helper does nothing when the context or its logger is null (a host
 * without a logger, or a module after its context was released).
 * Thread affinity: as the host's ILogger (the engine's is async-safe).
 * Allocation: one formatted std::string per call.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Spark/ILogger.h"

#include <format>
#include <string>
#include <utility>

namespace Spark::ModuleLog
{
    /** @brief Log an informational message through the host logger. */
    template <typename... Args> void Info(IEngineContext* context, std::format_string<Args...> format, Args&&... args)
    {
        if (ILogger* logger = context ? context->GetLogger() : nullptr)
            logger->Info(std::format(format, std::forward<Args>(args)...).c_str());
    }

    /** @brief Log a warning through the host logger. */
    template <typename... Args> void Warn(IEngineContext* context, std::format_string<Args...> format, Args&&... args)
    {
        if (ILogger* logger = context ? context->GetLogger() : nullptr)
            logger->Warn(std::format(format, std::forward<Args>(args)...).c_str());
    }

    /** @brief Log an error through the host logger. */
    template <typename... Args> void Error(IEngineContext* context, std::format_string<Args...> format, Args&&... args)
    {
        if (ILogger* logger = context ? context->GetLogger() : nullptr)
            logger->Error(std::format(format, std::forward<Args>(args)...).c_str());
    }

    /** @brief Log a debug message through the host logger. */
    template <typename... Args> void Debug(IEngineContext* context, std::format_string<Args...> format, Args&&... args)
    {
        if (ILogger* logger = context ? context->GetLogger() : nullptr)
            logger->Debug(std::format(format, std::forward<Args>(args)...).c_str());
    }
} // namespace Spark::ModuleLog
