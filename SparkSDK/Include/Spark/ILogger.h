/**
 * @file ILogger.h
 * @brief Logging interface game modules reach through IEngineContext::GetLogger()
 *
 * The host implements ILogger (EngineSdkLogger) and routes every message to its
 * log sinks: the engine log file, stderr, and the console. A module logs through
 * the context it received in OnLoad instead of including the engine-private
 * Utils/Logger.h, Utils/LogMacros.h or Utils/SparkConsole.h. Those are DLL-local
 * copies inside a module, so the host never sees what they record.
 *
 * ## Usage
 * @code
 *   #include <Spark/ModuleLog.h>
 *
 *   void MyModule::OnLoad(Spark::IEngineContext* context)
 *   {
 *       Spark::ModuleLog::Info(context, "Player spawned at wave {}", wave);
 *       Spark::ModuleLog::Warn(context, "Low ammo: {}", ammo);
 *       Spark::ModuleLog::Error(context, "Asset not found: {}", path);
 *   }
 * @endcode
 *
 * The helpers format with std::format and do nothing when the context or its
 * logger is null. Call ILogger directly only for preformatted text.
 */

#pragma once

namespace Spark
{

    /**
     * @brief Abstract logging interface for game modules
     *
     * Implemented by the host; obtained from IEngineContext::GetLogger(). Every
     * method takes a null-terminated message the implementation copies before
     * returning, and must be safe to call from any thread.
     */
    class ILogger
    {
      public:
        virtual ~ILogger() = default;

        /** @brief Log an informational message */
        virtual void Info(const char* message) = 0;

        /** @brief Log a warning message */
        virtual void Warn(const char* message) = 0;

        /** @brief Log an error message */
        virtual void Error(const char* message) = 0;

        /** @brief Log a debug message */
        virtual void Debug(const char* message) = 0;
    };

} // namespace Spark
