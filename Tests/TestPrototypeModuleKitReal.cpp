/**
 * @file TestPrototypeModuleKitReal.cpp
 * @brief MOD-295: prototype modules log, register console commands and add state rules through the public SDK.
 *
 * IEngineContext::GetLogger() hands a module the host's Spark::ILogger
 * (EngineSdkLogger on the real EngineContext), and the Spark::ModuleLog helpers
 * in <Spark/ModuleLog.h> format through it. IEngineContext::GetConsole() hands it
 * the host's Spark::IConsole (EngineSdkConsole), which registers into the host
 * SimpleConsole. SparkGameRTS and SparkGamePlatformer use both instead of the
 * private Utils/SparkConsole.h and Utils/LogMacros.h.
 *
 * MOD-310: IConsole::Print (SDK v8) writes the host's in-game console, and
 * Spark::ModuleLog::Bind lets SparkGameFPS's gameplay code log and print through
 * the context its OnLoad received.
 *
 * MOD-295: IEngineContext::GetStateValidation() (SDK v9) hands it the host's
 * Spark::IStateValidation (EngineSdkStateValidation), which adds rules to the host
 * InvalidStateDetector, so the prototype modules no longer include the private
 * Utils/InvalidStateDetector.h.
 */

#include "ScopedLoggerBaseline.h"
#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Engine/ECS/Components.h"
#include "Utils/InvalidStateDetector.h"
#include "Utils/Logger.h"
#include "Utils/SparkConsole.h"

#include <Spark/IConsole.h>
#include <Spark/IStateValidation.h>
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
    ///
    /// The baseline shuts the Logger down and re-initializes it, which is what resets the
    /// per-category levels: a test that ran the engine lifecycle before this one leaves them at the
    /// configured global level (Info), and Initialize() on an already-initialized Logger is a no-op,
    /// so Debug lines were silently dropped depending on test order.
    class ScopedLoggerCapture
    {
      public:
        ScopedLoggerCapture()
        {
            auto& logger = Spark::Logger::Get();
            logger.ClearSinks();
            logger.SetGlobalLevel(Spark::LogLevel::Trace);
            logger.AddSink(std::make_unique<CaptureSink>(m_lines));
        }

        ScopedLoggerCapture(const ScopedLoggerCapture&) = delete;
        ScopedLoggerCapture& operator=(const ScopedLoggerCapture&) = delete;

        const std::vector<CapturedLine>& Lines() const { return *m_lines; }

      private:
        ScopedLoggerBaseline m_baseline; // synchronous Logger in TestMain's state, before and after
        std::shared_ptr<std::vector<CapturedLine>> m_lines = std::make_shared<std::vector<CapturedLine>>();
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

    /// Initializes the host console for the test if nothing else has, and removes the probe commands on exit.
    struct ProbeConsoleScope final
    {
        Spark::SimpleConsole& console = Spark::SimpleConsole::GetInstance();
        bool restoreUninitialized = !console.IsInitialized();

        ProbeConsoleScope()
        {
            if (restoreUninitialized)
                console.Initialize();
            Clear();
        }
        ~ProbeConsoleScope()
        {
            Clear();
            if (restoreUninitialized)
                console.Shutdown();
        }
        ProbeConsoleScope(const ProbeConsoleScope&) = delete;
        ProbeConsoleScope& operator=(const ProbeConsoleScope&) = delete;

        void Clear()
        {
            for (const char* name : {"kit_probe", "kit_empty_handler", "kit_taken"})
                console.UnregisterCommand(name);
        }
    };

    /// Gives the test a freshly initialized host detector and leaves it as it found it (rules reset to defaults).
    struct FreshDetectorScope final
    {
        Spark::InvalidStateDetector& detector = Spark::InvalidStateDetector::GetInstance();
        bool wasInitialized = detector.IsInitialized();

        FreshDetectorScope()
        {
            detector.Shutdown();
            detector.Initialize();
        }
        ~FreshDetectorScope()
        {
            detector.Shutdown(); // also drops the scanned world pointer
            if (wasInitialized)
            {
                detector.Initialize();
            }
        }
        FreshDetectorScope(const FreshDetectorScope&) = delete;
        FreshDetectorScope& operator=(const FreshDetectorScope&) = delete;
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

TEST(PrototypeModuleKit_ConsoleRegistersThroughSdkContext)
{
    ProbeConsoleScope scope;
    Spark::SimpleConsole& host = scope.console;
    EngineContext context;
    Spark::IEngineContext* moduleView = &context;
    Spark::IConsole* console = moduleView->GetConsole();
    ASSERT_TRUE(console != nullptr);

    // A command registered through the SDK is dispatched by the host console, arguments and reply included.
    std::vector<std::string> received;
    ASSERT_TRUE(console->RegisterCommand(
        "kit_probe",
        [&received](const std::vector<std::string>& args) -> std::string
        {
            received = args;
            return "kit_probe got " + std::to_string(args.size()) + " args";
        },
        "Probe command", "Kit", "kit_probe <a> <b>"));
    EXPECT_TRUE(host.HasCommand("kit_probe"));
    ASSERT_TRUE(host.ExecuteCommand("kit_probe alpha 7"));
    EXPECT_TRUE(received == (std::vector<std::string>{"alpha", "7"}));
    const auto history = host.GetLogHistory();
    ASSERT_FALSE(history.empty());
    EXPECT_EQ(history.back().message, std::string("kit_probe got 2 args"));

    // Nameless and handler-less registrations are refused before they reach the registry.
    EXPECT_FALSE(
        console->RegisterCommand("", [](const std::vector<std::string>&) { return std::string(); }, "", "Kit", ""));
    EXPECT_FALSE(console->RegisterCommand("kit_empty_handler", Spark::IConsole::CommandHandler{}, "", "Kit", ""));
    EXPECT_FALSE(host.HasCommand("kit_empty_handler"));

    // A name another registrant owns is refused, and the owner's handler stays in place.
    ASSERT_TRUE(host.RegisterCommand(
        "kit_taken", [](const std::vector<std::string>&) { return std::string("owner handler"); }, "", "Kit", "",
        Spark::CommandPermission::Player, "other.module"));
    EXPECT_FALSE(console->RegisterCommand(
        "kit_taken", [](const std::vector<std::string>&) { return std::string("intruder"); }, "", "Kit", ""));
    EXPECT_EQ(host.GetCommandOwner("kit_taken"), std::string("other.module"));
    ASSERT_TRUE(host.ExecuteCommand("kit_taken"));
    EXPECT_EQ(host.GetLogHistory().back().message, std::string("owner handler"));

    // Unregistering through the SDK removes the command from the host; an unknown name is a no-op.
    console->UnregisterCommand("kit_probe");
    EXPECT_FALSE(host.HasCommand("kit_probe"));
    received.clear();
    EXPECT_FALSE(host.ExecuteCommand("kit_probe alpha 7"));
    EXPECT_TRUE(received.empty());
    console->UnregisterCommand("kit_never_registered");
    EXPECT_TRUE(host.HasCommand("kit_taken"));
}

namespace
{
    /// Unbinds Spark::ModuleLog on scope exit, so a failing test cannot leave a dangling context bound.
    struct ScopedModuleLogBinding final
    {
        explicit ScopedModuleLogBinding(Spark::IEngineContext* context) { Spark::ModuleLog::Bind(context); }
        ~ScopedModuleLogBinding() { Spark::ModuleLog::Bind(nullptr); }
        ScopedModuleLogBinding(const ScopedModuleLogBinding&) = delete;
        ScopedModuleLogBinding& operator=(const ScopedModuleLogBinding&) = delete;
    };

    /// True when the host console's newest history line is exactly @p message with severity tag @p type.
    bool NewestConsoleLineIs(const Spark::SimpleConsole& console, const std::string& message, const std::string& type)
    {
        const auto history = console.GetLogHistory();
        return !history.empty() && history.back().message == message && history.back().type == type;
    }
} // namespace

TEST(PrototypeModuleKit_ConsolePrintReachesHostConsole)
{
    ProbeConsoleScope scope;
    EngineContext context;
    Spark::IEngineContext* moduleView = &context;
    Spark::IConsole* console = moduleView->GetConsole();
    ASSERT_TRUE(console != nullptr);

    // IConsole::Print lands in the host console history with the severity tag the module chose.
    console->Print("[Kit] print probe", "SUCCESS");
    EXPECT_TRUE(NewestConsoleLineIs(scope.console, "[Kit] print probe", "SUCCESS"));

    // A module that bound its context prints and logs without passing it around.
    ScopedLoggerCapture capture;
    ScopedModuleLogBinding binding(moduleView);
    EXPECT_TRUE(Spark::ModuleLog::BoundContext() == moduleView);
    Spark::ModuleLog::Print("[Kit] bound print", "WARNING");
    EXPECT_TRUE(NewestConsoleLineIs(scope.console, "[Kit] bound print", "WARNING"));
    Spark::ModuleLog::Error("[Kit] bound error {}", 7);
    ASSERT_EQ(capture.Lines().size(), static_cast<size_t>(1));
    EXPECT_TRUE(capture.Lines().front().level == Spark::LogLevel::Error);
    EXPECT_EQ(capture.Lines().front().message, std::string("[Kit] bound error 7"));
}

TEST(PrototypeModuleKit_BoundModuleLogIsNullSafe)
{
    ProbeConsoleScope scope;
    ScopedLoggerCapture capture;
    // The history is capped, so compare its newest line rather than its size.
    const auto newestLine = [&scope]() -> std::string
    {
        const auto history = scope.console.GetLogHistory();
        return history.empty() ? std::string() : history.back().message;
    };
    const std::string newestBefore = newestLine();

    // Nothing bound: every context-free helper is a silent no-op.
    Spark::ModuleLog::Bind(nullptr);
    Spark::ModuleLog::Info("[Kit] unbound {}", 1);
    Spark::ModuleLog::Warn("[Kit] unbound {}", 2);
    Spark::ModuleLog::Error("[Kit] unbound {}", 3);
    Spark::ModuleLog::Debug("[Kit] unbound {}", 4);
    Spark::ModuleLog::Print("[Kit] unbound print", "INFO");
    EXPECT_TRUE(capture.Lines().empty());
    EXPECT_TRUE(newestLine() == newestBefore);

    // A bound host without a console drops Print but still logs through its own logger.
    RecordingLogger recorder;
    LoggerOnlyContext noConsole(&recorder);
    {
        ScopedModuleLogBinding binding(&noConsole);
        Spark::ModuleLog::Print("[Kit] no console", "INFO");
        Spark::ModuleLog::Info("[Kit] routed {}", 5);
    }
    EXPECT_TRUE(newestLine() == newestBefore);
    EXPECT_TRUE(recorder.Saw("info", "[Kit] routed 5"));
    EXPECT_EQ(recorder.Count(), static_cast<size_t>(1));
    EXPECT_TRUE(Spark::ModuleLog::BoundContext() == nullptr);
    EXPECT_TRUE(capture.Lines().empty());
}

TEST(PrototypeModuleKit_StateRulesRegisterThroughSdkContext)
{
    EngineContext context;
    Spark::IEngineContext* moduleView = &context;
    Spark::IStateValidation* rules = moduleView->GetStateValidation();
    ASSERT_TRUE(rules != nullptr);

    const auto probeCheck = [](EntityID probe, int& runs)
    {
        return [probe, &runs](World&, std::vector<Spark::StateViolation>& out)
        {
            ++runs;
            out.push_back({"Kit.Probe", static_cast<uint32_t>(probe), "probe violation",
                           Spark::StateViolationSeverity::Critical});
        };
    };

    World world;
    const EntityID probe = world.CreateEntity("KitProbe");
    int runs = 0;
    {
        // The windowed hosts load game modules before the lifecycle initializes the detector, so a rule added
        // while it is not running must survive Initialize() and run on the host's scan.
        auto& detector = Spark::InvalidStateDetector::GetInstance();
        const bool wasInitialized = detector.IsInitialized();
        detector.Shutdown();
        const uint32_t rulesBeforeStart = detector.GetRuleCount();
        EXPECT_TRUE(rules->AddRule("Kit.Early", "Kit", Spark::StateViolationSeverity::Error, probeCheck(probe, runs)));
        EXPECT_EQ(detector.GetRuleCount(), rulesBeforeStart + 1u);
        // Shutdown() before start must still drop it: a module unloaded ahead of the host start would otherwise
        // leave a callback into its unloaded image for the next Initialize().
        detector.Shutdown();
        EXPECT_FALSE(detector.HasRule("Kit.Early"));
        EXPECT_TRUE(rules->AddRule("Kit.Early", "Kit", Spark::StateViolationSeverity::Error, probeCheck(probe, runs)));
        // Public category cleanup must also release an early callback before host startup.
        rules->RemoveRulesByCategory("Kit");
        rules->RemoveRulesByCategory("Kit");
        EXPECT_FALSE(detector.HasRule("Kit.Early"));
        EXPECT_EQ(detector.GetRuleCount(), rulesBeforeStart);
        EXPECT_TRUE(rules->AddRule("Kit.Early", "Kit", Spark::StateViolationSeverity::Error, probeCheck(probe, runs)));
        detector.Initialize();
        EXPECT_TRUE(detector.HasRule("Kit.Early"));
        detector.SetWorld(&world);
        detector.Update(detector.GetConfig().checkIntervalSec);
        EXPECT_EQ(runs, 1);
        rules->RemoveRulesByCategory("Kit");
        EXPECT_FALSE(detector.HasRule("Kit.Early"));
        detector.Shutdown(); // also drops the scanned world pointer
        if (wasInitialized)
        {
            detector.Initialize();
        }
        runs = 0;
    }

    FreshDetectorScope scope;
    Spark::InvalidStateDetector& host = scope.detector;
    const uint32_t defaultRules = host.GetRuleCount();

    // A rule added through the SDK lands in the host detector and runs on the host's scan of the world.
    ASSERT_TRUE(rules->AddRule("Kit.Probe", "Kit", Spark::StateViolationSeverity::Critical, probeCheck(probe, runs)));
    EXPECT_TRUE(host.HasRule("Kit.Probe"));
    EXPECT_EQ(host.GetRuleCount(), defaultRules + 1u);
    host.SetWorld(&world);
    host.Update(host.GetConfig().checkIntervalSec);
    EXPECT_EQ(runs, 1);
    const auto status = host.GetStatus();
    const auto reported = std::find_if(status.recentViolations.begin(), status.recentViolations.end(),
                                       [](const Spark::StateViolation& v) { return v.ruleName == "Kit.Probe"; });
    ASSERT_TRUE(reported != status.recentViolations.end());
    EXPECT_EQ(reported->entityId, static_cast<uint32_t>(probe));
    EXPECT_TRUE(reported->severity == Spark::StateViolationSeverity::Critical);

    // Nameless, category-less and check-less rules are refused before they reach the detector.
    EXPECT_FALSE(rules->AddRule("", "Kit", Spark::StateViolationSeverity::Error, probeCheck(probe, runs)));
    EXPECT_FALSE(rules->AddRule("Kit.NoCategory", "", Spark::StateViolationSeverity::Error, probeCheck(probe, runs)));
    EXPECT_FALSE(rules->AddRule("Kit.NoCheck", "Kit", Spark::StateViolationSeverity::Error, Spark::StateCheckFn{}));
    EXPECT_FALSE(host.HasRule("Kit.NoCategory"));
    EXPECT_FALSE(host.HasRule("Kit.NoCheck"));
    EXPECT_EQ(host.GetRuleCount(), defaultRules + 1u);

    // Registrations and removals are attributed to the module whose lifecycle callback is running, so one
    // module's RemoveRulesByCategory leaves another module's rule in the same category alone.
    {
        Spark::InvalidStateDetector::ScopedRegistrationOwner owner(host, "kit.other");
        ASSERT_TRUE(
            rules->AddRule("Kit.Other", "Kit", Spark::StateViolationSeverity::Warning, probeCheck(probe, runs)));
    }
    {
        Spark::InvalidStateDetector::ScopedRegistrationOwner owner(host, "kit.self");
        ASSERT_TRUE(rules->AddRule("Kit.Self", "Kit", Spark::StateViolationSeverity::Warning, probeCheck(probe, runs)));
        rules->RemoveRulesByCategory("Kit");
    }
    EXPECT_FALSE(host.HasRule("Kit.Self"));
    EXPECT_TRUE(host.HasRule("Kit.Other"));
    EXPECT_EQ(host.RemoveRulesByOwner("kit.other"), static_cast<size_t>(1));

    // Outside any module callback the category is removed whole; an unknown category is a no-op.
    rules->RemoveRulesByCategory("Kit");
    EXPECT_FALSE(host.HasRule("Kit.Probe"));
    rules->RemoveRulesByCategory("Kit.NeverAdded");
    EXPECT_EQ(host.GetRuleCount(), defaultRules);
    runs = 0;
    host.Update(host.GetConfig().checkIntervalSec);
    EXPECT_EQ(runs, 0);
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
