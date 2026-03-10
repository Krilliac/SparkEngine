/**
 * @file ConsoleSink.h
 * @brief Logger sink that bridges the unified Logger to SimpleConsole
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a Logger sink implementation that forwards all logged messages
 * to the SimpleConsole, unifying the two logging systems. Messages logged
 * through the Logger (with categories, source locations, etc.) automatically
 * appear in the console with appropriate color coding.
 *
 * @code
 *   auto& logger = Spark::Logger::Get();
 *   logger.AddSink(std::make_unique<Spark::ConsoleSink>());
 *   // Now SPARK_LOG_INFO(Core, "Hello") appears in SimpleConsole too
 * @endcode
 */

#pragma once

#include "Logger.h"
#include <string>

namespace Spark
{

    class SimpleConsole; // Forward declaration

    /**
     * @brief Logger sink that writes formatted messages to SimpleConsole
     *
     * Maps Logger LogLevel to SimpleConsole log types:
     *   Trace -> "TRACE", Debug -> "DEBUG", Info -> "INFO",
     *   Warn -> "WARNING", Error -> "ERROR", Fatal -> "CRITICAL"
     *
     * Messages include the log category prefix for context.
     */
    class ConsoleSink : public ILogSink
    {
      public:
        /**
         * @brief Construct a ConsoleSink
         * @param includeSourceInfo If true, appends (file:line) to messages
         */
        explicit ConsoleSink(bool includeSourceInfo = false);

        void Write(const LogMessage& msg) override;
        void Flush() override;

      private:
        static const char* LogLevelToConsoleType(LogLevel level);
        bool m_includeSourceInfo;
    };

} // namespace Spark
