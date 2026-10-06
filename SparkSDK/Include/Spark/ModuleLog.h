/**
 * @file ModuleLog.h
 * @brief std::format logging helpers for game modules, routed through IEngineContext::GetLogger().
 *
 * A module logs through the host's ILogger, so its messages reach the host's
 * log file and stderr, without including the engine-private
 * Utils/SparkConsole.h or Utils/LogMacros.h:
 * @code
 *   #include <Spark/ModuleLog.h>
 *   Spark::ModuleLog::Info(context, "[RTS] Match saved to slot: {}", slot);
 *   Spark::ModuleLog::Warn(context, "[RTS] Autosave failed");
 * @endcode
 *
 * A module whose gameplay code has no context at hand binds the one it received
 * in OnLoad once, and unbinds it at the end of OnUnload; the context-free
 * overloads and Print then use it:
 * @code
 *   bool MyModule::OnLoad(Spark::IEngineContext* context)
 *   {
 *       Spark::ModuleLog::Bind(context);
 *       Spark::ModuleLog::Info("[My] loaded wave {}", 1);          // host engine log
 *       Spark::ModuleLog::Print("Arena ready", "SUCCESS");          // host in-game console
 *       return true;
 *   }
 *   void MyModule::OnUnload() { Spark::ModuleLog::Bind(nullptr); }
 * @endcode
 *
 * Every helper does nothing when the context (passed or bound), its logger or
 * its console is null (a host without them, or a module after its context was
 * released).
 * Thread affinity: as the host's ILogger and IConsole::Print (the engine's are
 * both async-safe); the bound context is an atomic pointer, so it can be read
 * from any thread, but Bind belongs in OnLoad/OnUnload.
 * Ownership: the bound pointer is borrowed; the host keeps the context alive
 * until after the module's OnUnload returns. Stop module worker callbacks before
 * unbinding: an atomic pointer does not extend the borrowed context's lifetime.
 * Allocation: one formatted std::string per call.
 * Scope: the bound context is stored behind a hidden inline accessor. ELF
 * visibility is explicitly hidden so the dynamic linker cannot interpose one
 * module image's slot into another image.
 */

#pragma once

#include "Spark/IConsole.h"
#include "Spark/IEngineContext.h"
#include "Spark/ILogger.h"

#include <atomic>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
#pragma GCC visibility push(hidden)
#endif

namespace Spark::ModuleLog
{
    namespace Detail
    {
        /** @brief Return the context slot owned by this module image. */
        [[nodiscard]] inline std::atomic<IEngineContext*>& BoundContextSlot() noexcept
        {
            static std::atomic<IEngineContext*> boundContext{nullptr};
            return boundContext;
        }
    } // namespace Detail

    /** @brief Log an informational message through the host logger. */
    template <typename... Args> void Info(IEngineContext* context, std::format_string<Args...> format, Args&&... args)
    {
        if (ILogger* logger = context ? context->GetLogger() : nullptr)
        {
            logger->Info(std::format(format, std::forward<Args>(args)...).c_str());
        }
    }

    /** @brief Log a warning through the host logger. */
    template <typename... Args> void Warn(IEngineContext* context, std::format_string<Args...> format, Args&&... args)
    {
        if (ILogger* logger = context ? context->GetLogger() : nullptr)
        {
            logger->Warn(std::format(format, std::forward<Args>(args)...).c_str());
        }
    }

    /** @brief Log an error through the host logger. */
    template <typename... Args> void Error(IEngineContext* context, std::format_string<Args...> format, Args&&... args)
    {
        if (ILogger* logger = context ? context->GetLogger() : nullptr)
        {
            logger->Error(std::format(format, std::forward<Args>(args)...).c_str());
        }
    }

    /** @brief Log a debug message through the host logger. */
    template <typename... Args> void Debug(IEngineContext* context, std::format_string<Args...> format, Args&&... args)
    {
        if (ILogger* logger = context ? context->GetLogger() : nullptr)
        {
            logger->Debug(std::format(format, std::forward<Args>(args)...).c_str());
        }
    }

    /** @brief Make @p context the one the context-free helpers use; pass nullptr to unbind. */
    inline void Bind(IEngineContext* context) noexcept
    {
        Detail::BoundContextSlot().store(context, std::memory_order_release);
    }

    /** @brief The context the last Bind() stored, or null. */
    [[nodiscard]] inline IEngineContext* BoundContext() noexcept
    {
        return Detail::BoundContextSlot().load(std::memory_order_acquire);
    }

    /** @brief Log an informational message through the bound context's logger. */
    template <typename... Args> void Info(std::format_string<Args...> format, Args&&... args)
    {
        Info(BoundContext(), format, std::forward<Args>(args)...);
    }

    /** @brief Log a warning through the bound context's logger. */
    template <typename... Args> void Warn(std::format_string<Args...> format, Args&&... args)
    {
        Warn(BoundContext(), format, std::forward<Args>(args)...);
    }

    /** @brief Log an error through the bound context's logger. */
    template <typename... Args> void Error(std::format_string<Args...> format, Args&&... args)
    {
        Error(BoundContext(), format, std::forward<Args>(args)...);
    }

    /** @brief Log a debug message through the bound context's logger. */
    template <typename... Args> void Debug(std::format_string<Args...> format, Args&&... args)
    {
        Debug(BoundContext(), format, std::forward<Args>(args)...);
    }

    /**
     * @brief Print a line to the host's in-game console through the bound context
     * @param message The text to show
     * @param type The console's severity tag, such as "INFO", "SUCCESS", "WARNING" or "ERROR"
     */
    inline void Print(std::string_view message, std::string_view type)
    {
        IEngineContext* context = BoundContext();
        if (IConsole* console = context ? context->GetConsole() : nullptr)
        {
            console->Print(message, type);
        }
    }
} // namespace Spark::ModuleLog

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
#pragma GCC visibility pop
#endif
