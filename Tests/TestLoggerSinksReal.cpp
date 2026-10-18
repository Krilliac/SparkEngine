// TestLoggerSinksReal.cpp - Logger::InstallDefaultSinks against the real Logger,
// FileSink and ConsoleSink. Exercises production types only; no reimplementation.

#include "TestFramework.h"
#include "ScopedLoggerBaseline.h"
#include "Utils/Logger.h"
#include "Utils/ConsoleSink.h"
#include "Utils/SparkConsole.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    std::filesystem::path MakeUniqueLogDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() / ("spark_logger_sinks_" + std::to_string(stamp));
    }

    /// FileSink::Config concatenates directory + prefix, so the directory needs
    /// its own trailing separator.
    std::string AsSinkDirectory(const std::filesystem::path& directory)
    {
        return directory.string() + "/";
    }
} // namespace

// =============================================================================
// utils-01 — engine log output must be persisted to a file
// =============================================================================

TEST(LoggerSinks_InstallDefaultSinksWritesEngineLogToFile)
{
    ScopedLoggerBaseline loggerBaseline;
    namespace fs = std::filesystem;
    const fs::path logDirectory = MakeUniqueLogDirectory();

    auto& logger = Spark::Logger::Get();
    logger.ClearSinks();
    logger.Initialize(false); // sync mode: the message is on disk when Log() returns
    logger.SetGlobalLevel(Spark::LogLevel::Trace);

    Spark::Logger::SinkSetup setup;
    setup.file.directory = AsSinkDirectory(logDirectory);
    setup.file.prefix = "SinkRoundTrip";
    const std::string logFilePath = logger.InstallDefaultSinks(setup);

    ASSERT_FALSE(logFilePath.empty());
    EXPECT_TRUE(fs::exists(logFilePath));

    logger.Log(Spark::LogLevel::Info, Spark::LogCategory::Core, __FILE__, __LINE__, __FUNCTION__,
               "sink-round-trip-marker");
    logger.FlushAll();

    std::ifstream logFile(logFilePath);
    ASSERT_TRUE(logFile.is_open());
    std::ostringstream contents;
    contents << logFile.rdbuf();
    logFile.close();
    EXPECT_STR_CONTAINS(contents.str(), "sink-round-trip-marker");

    logger.ClearSinks();
    logger.Shutdown();

    std::error_code error;
    fs::remove_all(logDirectory, error);
}

TEST(LoggerSinks_FileSinkCanBeDeclined)
{
    ScopedLoggerBaseline loggerBaseline;
    auto& logger = Spark::Logger::Get();
    logger.ClearSinks();
    logger.Initialize(false);

    Spark::Logger::SinkSetup setup;
    setup.enableFile = false;
    // No file requested: the caller must be told so, not handed a path that
    // nothing is writing to.
    EXPECT_TRUE(logger.InstallDefaultSinks(setup).empty());

    logger.ClearSinks();
    logger.Shutdown();
}

TEST(LoggerSinks_FileSinkFailureIsAnnouncedNotSilentlyEmpty)
{
    ScopedLoggerBaseline loggerBaseline;
    namespace fs = std::filesystem;

    // A regular file where the sink wants a directory: create_directories() and
    // the ofstream both fail, so a file sink was requested and could not be made.
    const fs::path blocker = MakeUniqueLogDirectory();
    {
        std::ofstream blockerFile(blocker);
        ASSERT_TRUE(blockerFile.is_open());
        blockerFile << "not a directory\n";
    }

    auto& console = Spark::SimpleConsole::GetInstance();
    const bool consoleWasInitialized = console.IsInitialized();
    if (!consoleWasInitialized)
    {
        ASSERT_TRUE(console.Initialize());
    }

    auto& logger = Spark::Logger::Get();
    logger.ClearSinks();
    logger.Initialize(false);
    logger.SetGlobalLevel(Spark::LogLevel::Trace);

    Spark::Logger::SinkSetup setup;
    setup.enableStderr = false;
    setup.enableFile = true;
    setup.file.directory = AsSinkDirectory(blocker / "Logs");
    setup.file.prefix = "SinkFailureProbe";

    // Same empty string as LoggerSinks_FileSinkCanBeDeclined returns — which is
    // exactly why the failure has to be announced through the sinks instead.
    EXPECT_TRUE(logger.InstallDefaultSinks(setup, std::make_unique<Spark::ConsoleSink>()).empty());

    bool announced = false;
    for (const auto& entry : console.GetLogHistory())
    {
        if (entry.message.find("file sink requested") != std::string::npos)
        {
            announced = true;
        }
    }
    EXPECT_TRUE(announced);

    logger.ClearSinks();
    logger.Shutdown();
    if (!consoleWasInitialized)
    {
        console.Shutdown();
    }

    std::error_code error;
    fs::remove(blocker, error);
}

// =============================================================================
// utils-03 — SPARK_LOG_* must reach SimpleConsole (and through it the pipe)
// =============================================================================

TEST(LoggerSinks_InstallDefaultSinksBridgesToSimpleConsole)
{
    ScopedLoggerBaseline loggerBaseline;
    auto& console = Spark::SimpleConsole::GetInstance();
    const bool consoleWasInitialized = console.IsInitialized();
    if (!consoleWasInitialized)
    {
        ASSERT_TRUE(console.Initialize());
    }
    const uint64_t logsBefore = console.GetStats().totalLogsWritten;

    auto& logger = Spark::Logger::Get();
    logger.ClearSinks();
    logger.Initialize(false);
    logger.SetGlobalLevel(Spark::LogLevel::Trace);

    Spark::Logger::SinkSetup setup;
    setup.enableStderr = false;
    setup.enableFile = false;
    // The console bridge is caller-supplied: Logger is a leaf that standalone
    // tools compile without SparkConsole.cpp, so it cannot construct it.
    EXPECT_TRUE(logger.InstallDefaultSinks(setup, std::make_unique<Spark::ConsoleSink>()).empty());

    logger.Log(Spark::LogLevel::Warn, Spark::LogCategory::Graphics, __FILE__, __LINE__, __FUNCTION__,
               "console-bridge-marker");

    EXPECT_GT(console.GetStats().totalLogsWritten, logsBefore);

    bool bridged = false;
    std::string bridgedType;
    for (const auto& entry : console.GetLogHistory())
    {
        if (entry.message.find("console-bridge-marker") != std::string::npos)
        {
            bridged = true;
            bridgedType = entry.type;
            // The category prefix is what makes engine traffic readable in the
            // external console window.
            EXPECT_STR_CONTAINS(entry.message, "[Graphics]");
        }
    }
    EXPECT_TRUE(bridged);
    EXPECT_EQ(bridgedType, std::string("WARNING"));

    logger.ClearSinks();
    logger.Shutdown();
    if (!consoleWasInitialized)
    {
        console.Shutdown();
    }
}

// =============================================================================
// Flush policy — the log tail must reach the OS without Shutdown()
// =============================================================================
//
// Each test reads the log through a second, independent file handle while the
// sink still has the file open and Shutdown() has not run. Bytes visible there
// have left the process's buffer and sit in the OS file cache, which is exactly
// what survives TerminateProcess/_exit or a crash; bytes still in the sink's
// buffer are invisible to the second handle and would be lost. (Observed
// 2026-10-15: force-killed SparkEngine runs lost the last ~4-8 KB of the log.)

namespace
{
    struct FlushPolicyLog
    {
        std::filesystem::path directory = MakeUniqueLogDirectory();
        std::string path;

        explicit FlushPolicyLog(bool asyncLogger)
        {
            auto& logger = Spark::Logger::Get();
            logger.ClearSinks();
            logger.Shutdown();
            logger.Initialize(asyncLogger);
            logger.SetGlobalLevel(Spark::LogLevel::Trace);

            Spark::Logger::SinkSetup setup;
            setup.enableStderr = false;
            setup.file.directory = AsSinkDirectory(directory);
            setup.file.prefix = "FlushPolicy";
            path = logger.InstallDefaultSinks(setup);
        }

        ~FlushPolicyLog()
        {
            Spark::Logger::Get().ClearSinks();
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }

        /// What another process (or a post-mortem reader) would see right now.
        std::string ReadFromOtherHandle() const
        {
            std::ifstream file(path, std::ios::binary);
            std::ostringstream contents;
            contents << file.rdbuf();
            return contents.str();
        }

        bool AppearsWithin(const std::string& marker, std::chrono::milliseconds timeout) const
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (ReadFromOtherHandle().find(marker) != std::string::npos)
                {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return ReadFromOtherHandle().find(marker) != std::string::npos;
        }
    };
} // namespace

TEST(LoggerFlush_WarnIsOnDiskWhenLogReturns)
{
    ScopedLoggerBaseline loggerBaseline;
    FlushPolicyLog log(/*asyncLogger=*/false);
    ASSERT_FALSE(log.path.empty());

    Spark::Logger::Get().Log(Spark::LogLevel::Warn, Spark::LogCategory::Core, __FILE__, __LINE__, __FUNCTION__,
                             "flush-policy-warn-marker");

    // No FlushAll(), no Shutdown(), no waiting: Warn must already be out.
    EXPECT_STR_CONTAINS(log.ReadFromOtherHandle(), "flush-policy-warn-marker");
}

TEST(LoggerFlush_InfoReachesDiskWithinFlushIntervalWithoutShutdown)
{
    ScopedLoggerBaseline loggerBaseline;
    FlushPolicyLog log(/*asyncLogger=*/false);
    ASSERT_FALSE(log.path.empty());

    Spark::Logger::Get().Log(Spark::LogLevel::Info, Spark::LogCategory::Core, __FILE__, __LINE__, __FUNCTION__,
                             "flush-policy-info-marker");

    // The process then goes quiet, as a hung or about-to-be-killed one does: no
    // later line pushes the buffer out. Allow 20x the interval for a loaded runner.
    EXPECT_TRUE(log.AppearsWithin("flush-policy-info-marker", Spark::Logger::kPeriodicFlushInterval * 20));
}

TEST(LoggerFlush_FlushForCrashWritesBufferedAndQueuedLines)
{
    ScopedLoggerBaseline loggerBaseline;
    for (const bool asyncLogger : {false, true})
    {
        FlushPolicyLog log(asyncLogger);
        ASSERT_FALSE(log.path.empty());

        const std::string marker = std::string("flush-for-crash-marker-") + (asyncLogger ? "async" : "sync");
        Spark::Logger::Get().Log(Spark::LogLevel::Debug, Spark::LogCategory::Core, __FILE__, __LINE__, __FUNCTION__,
                                 marker);
        Spark::Logger::Get().FlushForCrash();

        // Immediately, well inside one periodic interval.
        EXPECT_STR_CONTAINS(log.ReadFromOtherHandle(), marker);
    }
}
