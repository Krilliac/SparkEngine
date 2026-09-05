/**
 * @file ConsoleSink.cpp
 * @brief Logger-to-SimpleConsole bridge implementation
 * @author Spark Engine Team
 * @date 2026
 */

#include "ConsoleSink.h"
#include "SparkConsole.h"
#include "Validate.h"

#include <cstring>

namespace Spark
{

    ConsoleSink::ConsoleSink(bool includeSourceInfo) : m_includeSourceInfo(includeSourceInfo) {}

    void ConsoleSink::Write(const LogMessage& msg)
    {
        // No logging of any kind inside a sink. Logger::DispatchToSinks holds
        // m_sinkMutex across this call, so a SPARK_LOG_* here (the debug-only
        // SPARK_TRACE_ENTER that used to open this function) re-enters Logger::Log
        // on the same thread and deadlocks on that non-recursive mutex — latent
        // only for as long as nobody actually installed this sink.
        // Build the message: [Category] message (file:line)
        std::string formatted;
        formatted.reserve(msg.message.size() + 64);

        formatted += "[";
        formatted += LogCategoryToString(msg.category);
        formatted += "] ";
        formatted += msg.message;

        if (m_includeSourceInfo && !msg.file.empty())
        {
            // Extract just the filename
            const char* shortFile = msg.file.c_str();
            const char* slash = strrchr(shortFile, '\\');
            if (!slash)
            {
                slash = strrchr(shortFile, '/');
            }
            if (slash)
            {
                shortFile = slash + 1;
            }

            formatted += "  (";
            formatted += shortFile;
            formatted += ":";
            formatted += std::to_string(msg.line);
            formatted += ")";
        }

        const char* type = LogLevelToConsoleType(msg.level);
        SimpleConsole::GetInstance().Log(formatted, type);
    }

    void ConsoleSink::Flush()
    {
        // SimpleConsole writes immediately; nothing to flush
    }

    const char* ConsoleSink::LogLevelToConsoleType(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARNING";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "CRITICAL";
        default:
            return "INFO";
        }
    }

} // namespace Spark
