/**
 * @file TestPrototypeModuleKitReal.cpp
 * @brief MOD-295: prototype modules log through the public SDK logger.
 *
 * IEngineContext::GetLogger() hands a module the host's Spark::ILogger
 * (EngineSdkLogger on the real EngineContext), and the Spark::ModuleLog helpers
 * in <Spark/ModuleLog.h> format through it. SparkGameRTS and SparkGamePlatformer
 * log their engine-system wiring this way instead of through the private
 * Utils/SparkConsole.h and Utils/LogMacros.h.
 */

#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Utils/Logger.h"

#include <Spark/ModuleLog.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef SPARK_TEST_HAS_IMGUI
#include "../GameModules/SparkGameRTS/Source/Core/RTSEngineSystems.h"
#endif

namespace
{
    struct CapturedLine
    {
        Spark::LogLevel level;
        Spark::LogCategory category;
        std::string message;
    };

    /// Records what reaches the engine Logger.
    class CaptureSink final : public Spark::ILogSink
    {
      public:
        explicit CaptureSink(std::shared_ptr<std::vector<CapturedLine>> lines) : m_lines(std::move(lines)) {}
        void Write(const Spark::LogMessage& message) override
        {
            m_lines->push_back({message.level, message.category, message.message});
        }
        void Flush() override {}

      private:
        std::shared_ptr<std::vector<CapturedLine>> m_lines;
    };

    /// The engine Logger in synchronous mode with only the capture sink, restored on scope exit.
    class ScopedLoggerCapture
    {
      public:
        ScopedLoggerCapture() : m_previousLevel(Spark::Logger::Get().GetGlobalLevel())
        {
            auto& logger = Spark::Logger::Get();
            logger.ClearSinks();
            logger.Initialize(false); // synchronous: a message is in the sink when Log() returns
            logger.SetGlobalLevel(Spark::LogLevel::Trace);
            logger.AddSink(std::make_unique<CaptureSink>(m_lines));
        }

        ~ScopedLoggerCapture()
        {
            auto& logger = Spark::Logger::Get();
            logger.ClearSinks();
            logger.Shutdown();
            logger.SetGlobalLevel(m_previousLevel);
        }

        ScopedLoggerCapture(const ScopedLoggerCapture&) = delete;
        ScopedLoggerCapture& operator=(const ScopedLoggerCapture&) = delete;

        const std::vector<CapturedLine>& Lines() const { return *m_lines; }

      private:
        std::shared_ptr<std::vector<CapturedLine>> m_lines = std::make_shared<std::vector<CapturedLine>>();
        Spark::LogLevel m_previousLevel;
    };

    /// An ILogger that records every call, for a module driven outside the engine.
    class RecordingLogger final : public Spark::ILogger
    {
      public:
        void Info(const char* message) override { Record("info", message); }
        void Warn(const char* message) override { Record("warn", message); }
        void Error(const char* message) override { Record("error", message); }
        void Debug(const char* message) override { Record("debug", message); }

        bool Saw(const std::string& level, const std::string& message) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return std::find(m_lines.begin(), m_lines.end(), std::make_pair(level, message)) != m_lines.end();
        }

        size_t Count() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_lines.size();
        }

      private:
        void Record(const char* level, const char* message)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lines.emplace_back(level, message ? message : "");
        }

        mutable std::mutex m_mutex;
        std::vector<std::pair<std::string, std::string>> m_lines;
    };

    /// A module-facing context with only the required services (all absent) and an optional logger.
    class LoggerOnlyContext final : public Spark::IEngineContext
    {
      public:
        explicit LoggerOnlyContext(Spark::ILogger* logger) : m_logger(logger) {}

        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        ::AudioEngine* GetAudio() override { return nullptr; }
        const ::AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        uint32_t GetEngineVersion() const override { return SPARK_ENGINE_VERSION_PACKED; }
        uint32_t GetSDKVersion() const override { return SPARK_SDK_VERSION; }
        Spark::ILogger* GetLogger() override { return m_logger; }

      private:
        Spark::ILogger* m_logger;
    };
} // namespace

TEST(PrototypeModuleKit_LoggerRoutesThroughEngineContext)
{
    ScopedLoggerCapture capture;
    EngineContext context;
    Spark::IEngineContext* moduleView = &context;
    ASSERT_TRUE(moduleView->GetLogger() != nullptr);

    Spark::ModuleLog::Info(moduleView, "[Kit] wave {} of {}", 3, 10);
    Spark::ModuleLog::Warn(moduleView, "[Kit] low ammo: {}", 2);
    Spark::ModuleLog::Error(moduleView, "[Kit] missing asset {}", std::string("crate.obj"));
    Spark::ModuleLog::Debug(moduleView, "[Kit] tick {}", 42u);

    const std::vector<CapturedLine>& lines = capture.Lines();
    ASSERT_EQ(lines.size(), static_cast<size_t>(4));
    const std::vector<std::pair<Spark::LogLevel, std::string>> expected = {
        {Spark::LogLevel::Info, "[Kit] wave 3 of 10"},
        {Spark::LogLevel::Warn, "[Kit] low ammo: 2"},
        {Spark::LogLevel::Error, "[Kit] missing asset crate.obj"},
        {Spark::LogLevel::Debug, "[Kit] tick 42"},
    };
    for (size_t index = 0; index < expected.size(); ++index)
    {
        EXPECT_TRUE(lines[index].level == expected[index].first);
        EXPECT_TRUE(lines[index].category == Spark::LogCategory::Game);
        EXPECT_EQ(lines[index].message, expected[index].second);
    }
}

TEST(PrototypeModuleKit_NullContextIsSafe)
{
    ScopedLoggerCapture capture;

    // No context, and a context whose host offers no logger: both are silent no-ops.
    Spark::ModuleLog::Info(nullptr, "[Kit] dropped {}", 1);
    Spark::ModuleLog::Error(nullptr, "[Kit] dropped {}", 2);
    LoggerOnlyContext noLogger(nullptr);
    Spark::ModuleLog::Warn(&noLogger, "[Kit] dropped {}", 3);
    Spark::ModuleLog::Debug(&noLogger, "[Kit] dropped {}", 4);
    EXPECT_TRUE(capture.Lines().empty());

    // The helpers call whatever logger the context hands out, not a global one.
    RecordingLogger recorder;
    LoggerOnlyContext withLogger(&recorder);
    Spark::ModuleLog::Warn(&withLogger, "[Kit] routed {}", 5);
    EXPECT_TRUE(recorder.Saw("warn", "[Kit] routed 5"));
    EXPECT_EQ(recorder.Count(), static_cast<size_t>(1));
    EXPECT_TRUE(capture.Lines().empty());
}

#ifdef SPARK_TEST_HAS_IMGUI
TEST(PrototypeModuleKit_RtsSetupLogsThroughSdk)
{
    RecordingLogger recorder;
    LoggerOnlyContext context(&recorder);

    RTS::RTSEngineSystems systems;
    ASSERT_TRUE(systems.Initialize(&context));
    EXPECT_TRUE(recorder.Saw("info", "[RTS] Initializing engine system integrations..."));
    EXPECT_TRUE(recorder.Saw("info", "[RTS] Coroutines: build/research/train timers configured"));
    EXPECT_TRUE(recorder.Saw("info", "[RTS] Engine system integrations initialized (6 subsystems wired)"));

    // Console commands report through the same logger; this context has no SaveSystem.
    EXPECT_FALSE(systems.SaveMatch("slot_a"));
    EXPECT_TRUE(recorder.Saw("error", "[RTS] SaveSystem not available"));
    EXPECT_FALSE(systems.SaveMatch("../escape"));
    EXPECT_TRUE(recorder.Saw("error", "[RTS] Invalid save slot: ../escape"));

    systems.Shutdown();
    EXPECT_TRUE(recorder.Saw("info", "[RTS] Engine system integrations shut down"));
}
#endif
