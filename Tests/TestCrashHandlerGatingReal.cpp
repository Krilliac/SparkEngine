// TestCrashHandlerGatingReal.cpp - Real crash-path helpers: the stack hash the
// uploader actually calls, the redaction applied before transport, the ungated
// crash-report entry point, and the shipping-build watchdog gate.

#include "TestFramework.h"
#include "Core/Platform.h"
#include "Utils/CrashHandler.h"
#include "Utils/CrashHandlerSupport.h"
#include "Utils/CrashReportUploader.h"
#include "Utils/FreezeDetector.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// =============================================================================
// utils-05 — the hash must recognise the format the engine itself writes
// =============================================================================

namespace
{
    /// Byte-for-byte the shape CrashHandler::SymStackTrace() produces.
    std::string MakeEngineStackTrace(const std::string& topSymbol)
    {
        return std::string("*** STACK TRACE ***\n") + "  " + kStackFrameMarker + topSymbol + " +0x42\n" + "  " +
               kStackFrameMarker + "Spark::GraphicsEngine::Present +0x1a\n" + "  " + kStackFrameMarker +
               "Spark::Engine::Run +0xff\n";
    }
} // namespace

TEST(CrashHash_EngineStackTraceFormatProducesAHash)
{
    const std::string hash = ComputeStackHash(MakeEngineStackTrace("Spark::Renderer::Draw"));
    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(hash.size(), static_cast<size_t>(8));
}

TEST(CrashHash_EngineStackTraceHashIsStableAndDiscriminating)
{
    const std::string first = ComputeStackHash(MakeEngineStackTrace("Spark::Renderer::Draw"));
    const std::string second = ComputeStackHash(MakeEngineStackTrace("Spark::Renderer::Draw"));
    const std::string other = ComputeStackHash(MakeEngineStackTrace("Spark::Physics::Step"));
    EXPECT_EQ(first, second);
    EXPECT_FALSE(other.empty());
    EXPECT_NE(first, other);
}

TEST(CrashHash_ThreadStackSectionDoesNotFeedTheHash)
{
    // ThreadStacks() deliberately writes unmarked lines: only the faulting
    // thread's stack may decide which issue a crash is deduplicated onto.
    const std::string threadStacksOnly = "*** THREAD STACKS ***\n"
                                         "\nThread 0x1234\n"
                                         " Spark::Worker::Wait +0x10\n"
                                         " Spark::JobSystem::Run +0x20\n";
    EXPECT_TRUE(ComputeStackHash(threadStacksOnly).empty());
}

// =============================================================================
// utils-08 — no profile path or account name may leave the machine
// =============================================================================

namespace
{
    Spark::CrashHandlerDetail::CrashRedactionContext MakeTestRedactionContext()
    {
        Spark::CrashHandlerDetail::CrashRedactionContext context;
        // Longest first, as MakeCrashRedactionContext() orders them.
        context.pathTokens.emplace_back("C:\\Users\\jane\\AppData\\Local", "%LOCALAPPDATA%");
        context.pathTokens.emplace_back("C:\\Users\\jane", "%USERPROFILE%");
        context.userName = "jane";
        context.machineName = "JANE-DESKTOP";
        return context;
    }
} // namespace

TEST(CrashRedaction_ProfilePathAndAccountNameAreRemoved)
{
    const std::string log = "Faulting Module   : C:\\Users\\jane\\AppData\\Local\\Spark\\SparkEngine.exe\n"
                            "Config            : C:\\Users\\jane\\Documents\\Spark\\settings.ini\n"
                            "Machine           : JANE-DESKTOP\n";
    const std::string redacted =
        Spark::CrashHandlerDetail::RedactCrashText(log, MakeTestRedactionContext());

    EXPECT_TRUE(redacted.find("jane") == std::string::npos);
    EXPECT_TRUE(redacted.find("JANE-DESKTOP") == std::string::npos);
    EXPECT_STR_CONTAINS(redacted, "%LOCALAPPDATA%\\Spark\\SparkEngine.exe");
    EXPECT_STR_CONTAINS(redacted, "%USERPROFILE%\\Documents");
    EXPECT_STR_CONTAINS(redacted, "<machine>");
}

TEST(CrashRedaction_MatchesRegardlessOfCaseOrSeparator)
{
    const std::string log = "Module: c:/users/JANE/appdata/local/Spark/SparkEngine.exe\n";
    const std::string redacted =
        Spark::CrashHandlerDetail::RedactCrashText(log, MakeTestRedactionContext());
    EXPECT_STR_CONTAINS(redacted, "%LOCALAPPDATA%");
    EXPECT_TRUE(redacted.find("JANE") == std::string::npos);
}

TEST(CrashRedaction_SystemPathsAndDiagnosticsSurvive)
{
    // The value of a crash log is the module and symbol names; redaction must
    // not shred anything that identifies nobody.
    const std::string log = "Faulting Module   : C:\\Windows\\System32\\ntdll.dll\n"
                            "  FRAME Spark::Renderer::Draw +0x42\n";
    const std::string redacted =
        Spark::CrashHandlerDetail::RedactCrashText(log, MakeTestRedactionContext());
    EXPECT_EQ(redacted, log);
}

TEST(CrashRedaction_ShortAccountNamesAreNotSubstitutedEverywhere)
{
    Spark::CrashHandlerDetail::CrashRedactionContext context;
    context.userName = "jo";
    const std::string log = "Faulting Module   : SparkEngine.exe (job system)\n";
    EXPECT_EQ(Spark::CrashHandlerDetail::RedactCrashText(log, context), log);
}

#ifdef _WIN32
namespace
{
    /// Clears one environment variable for the life of the object and restores it.
    class ScopedClearedEnvironmentVariable
    {
      public:
        explicit ScopedClearedEnvironmentVariable(const char* name) : m_name(name)
        {
            if (const char* value = std::getenv(name); value)
            {
                m_wasSet = true;
                m_value = value;
            }
            _putenv_s(m_name, "");
        }

        ~ScopedClearedEnvironmentVariable()
        {
            _putenv_s(m_name, m_wasSet ? m_value.c_str() : "");
        }

        ScopedClearedEnvironmentVariable(const ScopedClearedEnvironmentVariable&) = delete;
        ScopedClearedEnvironmentVariable& operator=(const ScopedClearedEnvironmentVariable&) = delete;

      private:
        const char* m_name;
        bool m_wasSet = false;
        std::string m_value;
    };
} // namespace

TEST(CrashRedaction_ContextIsStillBuiltWhenTheEnvironmentIsSanitized)
{
    // Services, session-0 processes and launchers that scrub their child's
    // environment leave every one of these unset.
    const ScopedClearedEnvironmentVariable temp("TEMP");
    const ScopedClearedEnvironmentVariable localAppData("LOCALAPPDATA");
    const ScopedClearedEnvironmentVariable appData("APPDATA");
    const ScopedClearedEnvironmentVariable userProfile("USERPROFILE");
    const ScopedClearedEnvironmentVariable userName("USERNAME");
    const ScopedClearedEnvironmentVariable computerName("COMPUTERNAME");

    const auto context = Spark::CrashHandlerDetail::MakeCrashRedactionContext();

    // With getenv() as the only source this context comes back empty and
    // RedactCrashText() becomes the identity function — on a report headed for a
    // public issue tracker, with the profile path and account name intact.
    EXPECT_TRUE(Spark::CrashHandlerDetail::HasRedactionRules(context));
    EXPECT_FALSE(context.pathTokens.empty());
    EXPECT_FALSE(context.userName.empty());
}

TEST(CrashRedaction_AnEmptyContextIsReportedAsHavingNoRules)
{
    // The guard the uploader consults: an all-empty context must never read as
    // "redaction succeeded".
    const Spark::CrashHandlerDetail::CrashRedactionContext empty;
    EXPECT_FALSE(Spark::CrashHandlerDetail::HasRedactionRules(empty));
    EXPECT_TRUE(Spark::CrashHandlerDetail::HasRedactionRules(MakeTestRedactionContext()));
}
#endif // _WIN32

// =============================================================================
// utils-02 — the ungated report entry point exists alongside the gated one
// =============================================================================

#if defined(SPARK_PLATFORM_WINDOWS) && defined(SPARK_MINIZ_AVAILABLE)

namespace
{
    /// The artifact root InstallCrashHandler() creates: temp/spark_crash_<pid>_<random>.
    std::filesystem::path FindCrashArtifactDirectory()
    {
        namespace fs = std::filesystem;
        std::error_code error;
        const fs::path temp = fs::temp_directory_path(error);
        if (error)
            return {};

        const std::string prefix = "spark_crash_" + std::to_string(GetCurrentProcessId()) + "_";
        for (fs::directory_iterator it(temp, error), end; !error && it != end; it.increment(error))
        {
            if (it->is_directory(error) && it->path().filename().string().rfind(prefix, 0) == 0)
                return it->path();
        }
        return {};
    }

    /// Number of .log artifacts in @p directory whose text contains @p token.
    size_t CountReportsContaining(const std::filesystem::path& directory, const std::string& token)
    {
        namespace fs = std::filesystem;
        if (directory.empty())
            return 0;

        size_t matches = 0;
        std::error_code error;
        for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
        {
            if (!it->is_regular_file(error) || it->path().extension() != ".log")
                continue;
            std::ifstream report(it->path());
            if (!report.is_open())
                continue;
            std::ostringstream contents;
            contents << report.rdbuf();
            if (contents.str().find(token) != std::string::npos)
                ++matches;
        }
        return matches;
    }
} // namespace

TEST(CrashHandler_UngatedReportWritesAnArtifactAndTheAssertGateDoesNot)
{
    // The suite installs its own unhandled-exception filter to report crashing
    // tests; InstallCrashHandler() replaces it, so put it back afterwards.
    LPTOP_LEVEL_EXCEPTION_FILTER harnessFilter = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(harnessFilter);

    CrashConfig config;
    config.dumpPrefix = L"SparkTestCrash";
    config.captureScreenshot = false;  // no swap chain in the test process
    config.captureSystemInfo = false;  // no DXGI enumeration
    config.captureAllThreads = false;  // no suspending the test runner's threads
    config.zipBeforeUpload = false;
    config.enableCrashReporting = false; // never upload from a test
    config.requireConsent = false;
    config.headlessMode = true; // no dialogs
    config.promptUserDescription = false;
    config.triggerCrashOnAssert = false; // the production default this test is about
    InstallCrashHandler(config);
    SetUnhandledExceptionFilter(harnessFilter);

    const std::filesystem::path artifacts = FindCrashArtifactDirectory();
    ASSERT_FALSE(artifacts.empty());

    // Gated entry point with the toggle off: logs, writes no report.
    TriggerCrashHandler("gated-entry-probe-a1b2c3");
    EXPECT_EQ(CountReportsContaining(artifacts, "gated-entry-probe-a1b2c3"), static_cast<size_t>(0));

    // Ungated entry point: this is the one FreezeDetector::OnCriticalFreeze and
    // Assert::Fail rely on, and it must leave an artifact on disk. A stubbed or
    // re-gated TriggerCrashReport fails here.
    TriggerCrashReport("ungated-entry-probe-d4e5f6");
    EXPECT_EQ(CountReportsContaining(artifacts, "ungated-entry-probe-d4e5f6"), static_cast<size_t>(1));

    // One report per process: the duplicate a fatal assert produces (gated call
    // followed by ungated call) must not write a second dump/log pair.
    TriggerCrashReport("duplicate-entry-probe-778899");
    EXPECT_EQ(CountReportsContaining(artifacts, "duplicate-entry-probe-778899"), static_cast<size_t>(0));

    std::error_code error;
    std::filesystem::remove_all(artifacts, error);
}

#endif // SPARK_PLATFORM_WINDOWS && SPARK_MINIZ_AVAILABLE

// =============================================================================
// utils-13 — the watchdog must not run where heartbeats are compiled out
// =============================================================================

TEST(FreezeDetector_StartHonoursTheShippingHeartbeatGate)
{
    auto& detector = Spark::FreezeDetector::GetInstance();
    const bool wasRunning = detector.IsRunning();
    if (wasRunning)
    {
        detector.Stop();
    }

    // Hour-long thresholds and no termination: this test must never be able to
    // take the process down, whatever the watchdog decides.
    Spark::FreezeDetectorConfig config;
    config.warningThresholdSec = 3600.0f;
    config.recoveryThresholdSec = 3600.0f;
    config.crashThresholdSec = 3600.0f;
    config.generateDumpOnFreeze = false;
    config.terminateOnFreeze = false;
    detector.Configure(config);

    detector.Start();
#if defined(SPARK_BUILD_SHIPPING)
    // SPARK_HEARTBEAT() is a no-op here, so a running watchdog would see zero
    // heartbeats and _Exit(1) the game after crashThresholdSec.
    EXPECT_FALSE(detector.IsRunning());
#else
    EXPECT_TRUE(detector.IsRunning());
#endif

    detector.Stop();
    EXPECT_FALSE(detector.IsRunning());
}
