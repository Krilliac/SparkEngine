/**
 * @file EngineSdkLogger.cpp
 * @brief Routes SDK ILogger calls from game modules to the engine Logger.
 */

#include "EngineSdkLogger.h"

#include "Utils/Logger.h"

namespace
{
    void Forward(Spark::LogLevel level, const char* message)
    {
        auto& logger = Spark::Logger::Get();
        if (!logger.ShouldLog(level, Spark::LogCategory::Game))
        {
            return;
        }
        // Modules have no engine source location to report; the category says who logged.
        logger.Log(level, Spark::LogCategory::Game, "", 0, "", message ? message : "");
    }
} // namespace

void EngineSdkLogger::Info(const char* message)
{
    Forward(Spark::LogLevel::Info, message);
}

void EngineSdkLogger::Warn(const char* message)
{
    Forward(Spark::LogLevel::Warn, message);
}

void EngineSdkLogger::Error(const char* message)
{
    Forward(Spark::LogLevel::Error, message);
}

void EngineSdkLogger::Debug(const char* message)
{
    Forward(Spark::LogLevel::Debug, message);
}
