/**
 * @file ILogger.h
 * @brief Logging interface for game modules
 *
 * Provides a lightweight logging API that game modules can use without
 * depending on the engine's internal Logger or SimpleConsole classes.
 * The engine's Logger.h defines the concrete LogLevel and LogCategory
 * enums; this SDK header provides an abstract ILogger interface that
 * modules use to emit log messages.
 *
 * ## Usage
 * @code
 *   // Include the engine's Logger.h for LogLevel/LogCategory enums
 *   #include "Utils/Logger.h"
 *
 *   // Or use SimpleConsole for simpler logging:
 *   #include "Utils/SparkConsole.h"
 *   auto& console = Spark::SimpleConsole::GetInstance();
 *   console.LogInfo("Player spawned");
 *   console.LogWarning("Low ammo");
 *   console.LogError("Asset not found");
 * @endcode
 *
 * ## Log levels (defined in Utils/Logger.h)
 * Trace, Debug, Info, Warn, Error, Fatal
 *
 * ## Log categories (defined in Utils/Logger.h)
 * Core, Graphics, Physics, Audio, AI, Animation, ECS, Network,
 * Input, Scripting, Scene, Save, Cinematic, Procedural, Editor, Game
 *
 * ## Structured logging with SPARK_LOG macros
 * @code
 *   #include "Utils/Logger.h"
 *   SPARK_LOG_INFO(Spark::LogCategory::Game, "Player {} scored {} points", name, points);
 *   SPARK_LOG_WARN(Spark::LogCategory::Game, "Low health: {}", health);
 * @endcode
 *
 * The engine routes messages to all active sinks (console, file, editor).
 */

#pragma once

namespace Spark
{

    /**
     * @brief Abstract logging interface for game modules
     *
     * Provides a minimal logging surface that can be implemented by
     * the engine's concrete Logger. Game modules that want richer
     * logging should include Utils/Logger.h directly.
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
