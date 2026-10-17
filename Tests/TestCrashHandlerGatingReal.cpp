// TestCrashHandlerGatingReal.cpp - Real crash-path helpers: the redaction applied
// to crash artifacts, the ungated crash-report entry point, and the
// shipping-build watchdog gate.

#include "TestFramework.h"
#include "Core/Platform.h"
#include "Utils/CrashHandler.h"
#include "Utils/CrashHandlerSupport.h"
#include "Utils/FreezeDetector.h"
#include "Utils/Process.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
#include <csignal>
#include <unistd.h>
#endif
#ifdef SPARK_PLATFORM_LINUX
#include <sys/prctl.h>
#include <sys/resource.h>
#endif

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
    const std::string redacted = Spark::CrashHandlerDetail::RedactCrashText(log, MakeTestRedactionContext());

    EXPECT_TRUE(redacted.find("jane") == std::string::npos);
    EXPECT_TRUE(redacted.find("JANE-DESKTOP") == std::string::npos);
    EXPECT_STR_CONTAINS(redacted, "%LOCALAPPDATA%\\Spark\\SparkEngine.exe");
    EXPECT_STR_CONTAINS(redacted, "%USERPROFILE%\\Documents");
    EXPECT_STR_CONTAINS(redacted, "<machine>");
}

TEST(CrashRedaction_MatchesRegardlessOfCaseOrSeparator)
{
    const std::string log = "Module: c:/users/JANE/appdata/local/Spark/SparkEngine.exe\n";
    const std::string redacted = Spark::CrashHandlerDetail::RedactCrashText(log, MakeTestRedactionContext());
    EXPECT_STR_CONTAINS(redacted, "%LOCALAPPDATA%");
    EXPECT_TRUE(redacted.find("JANE") == std::string::npos);
}

TEST(CrashRedaction_SystemPathsAndDiagnosticsSurvive)
{
    // The value of a crash log is the module and symbol names; redaction must
    // not shred anything that identifies nobody.
    const std::string log = "Faulting Module   : C:\\Windows\\System32\\ntdll.dll\n"
                            "  FRAME Spark::Renderer::Draw +0x42\n";
    const std::string redacted = Spark::CrashHandlerDetail::RedactCrashText(log, MakeTestRedactionContext());
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

        ~ScopedClearedEnvironmentVariable() { _putenv_s(m_name, m_wasSet ? m_value.c_str() : ""); }

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

// The production producer (CrashHandler.cpp) is compiled only when miniz is
// found; otherwise CMake links CrashHandlerStub.cpp on every platform, which
// writes no artifacts. Windows, Linux and macOS run the same entry points.
#if defined(SPARK_MINIZ_AVAILABLE) &&                                                                                  \
    (defined(SPARK_PLATFORM_WINDOWS) || defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS))
#define SPARK_TEST_CRASH_PRODUCER 1
#endif

#ifdef SPARK_TEST_CRASH_PRODUCER

namespace
{
    unsigned long CurrentProcessIdForCrashArtifacts()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return static_cast<unsigned long>(GetCurrentProcessId());
#else
        return static_cast<unsigned long>(getpid());
#endif
    }

#if !defined(SPARK_PLATFORM_WINDOWS)
    /// InstallCrashHandler() replaces the suite's crash-signal handlers (which
    /// name the crashing test) with its own one-shot handlers. Put the suite's
    /// handlers back when the test ends so later tests keep their diagnostics.
    class ScopedCrashSignalDispositions
    {
      public:
        ScopedCrashSignalDispositions()
        {
            for (size_t index = 0; index < kSignalCount; ++index)
                m_saved[index] = sigaction(kSignals[index], nullptr, &m_previous[index]) == 0;
        }

        ~ScopedCrashSignalDispositions()
        {
            for (size_t index = 0; index < kSignalCount; ++index)
            {
                if (m_saved[index])
                    sigaction(kSignals[index], &m_previous[index], nullptr);
            }
        }

        ScopedCrashSignalDispositions(const ScopedCrashSignalDispositions&) = delete;
        ScopedCrashSignalDispositions& operator=(const ScopedCrashSignalDispositions&) = delete;

      private:
        // The exact set InstallCrashHandler() hooks on POSIX.
        static constexpr int kSignals[] = {SIGSEGV, SIGFPE, SIGABRT, SIGBUS, SIGILL, SIGTRAP};
        static constexpr size_t kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
        struct sigaction m_previous[kSignalCount]{};
        bool m_saved[kSignalCount]{};
    };
#endif

    /// Artifact roots InstallCrashHandler() creates: temp/spark_crash_<pid>_<random>.
    std::vector<std::filesystem::path> FindCrashArtifactDirectories()
    {
        namespace fs = std::filesystem;
        std::vector<fs::path> directories;
        std::error_code error;
        const fs::path temp = fs::temp_directory_path(error);
        if (error)
            return directories;

        const std::string prefix = "spark_crash_" + std::to_string(CurrentProcessIdForCrashArtifacts()) + "_";
        for (fs::directory_iterator it(temp, error), end; !error && it != end; it.increment(error))
        {
            if (it->is_directory(error) && it->path().filename().string().rfind(prefix, 0) == 0)
                directories.push_back(it->path());
        }
        return directories;
    }

    /// Select the artifact root created by the current InstallCrashHandler() call.
    std::filesystem::path FindNewCrashArtifactDirectory(const std::vector<std::filesystem::path>& existingDirectories)
    {
        const std::vector<std::filesystem::path> currentDirectories = FindCrashArtifactDirectories();
        const auto found = std::find_if(currentDirectories.begin(), currentDirectories.end(),
                                        [&existingDirectories](const std::filesystem::path& candidate) {
                                            return std::find(existingDirectories.begin(), existingDirectories.end(),
                                                             candidate) == existingDirectories.end();
                                        });
        return found == currentDirectories.end() ? std::filesystem::path{} : *found;
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

    constexpr std::string_view kCanaryPrefix = "SPARKCANARY-";
    constexpr size_t kCanaryHexDigits = 16;

    /// Per-run secret stand-in. Tests/Tools/run_crash_capture_security.py supplies it through
    /// SPARK_TEST_CRASH_CANARY so it can rescan the untouched artifacts after this process exits.
    std::string MakeCrashCanary()
    {
        if (const char* supplied = std::getenv("SPARK_TEST_CRASH_CANARY"))
        {
            const std::string value(supplied);
            if (value.size() == kCanaryPrefix.size() + kCanaryHexDigits && value.starts_with(kCanaryPrefix))
                return value;
        }
        std::random_device device;
        std::uniform_int_distribution<int> nibble(0, 15);
        std::string canary(kCanaryPrefix);
        for (size_t digit = 0; digit < kCanaryHexDigits; ++digit)
            canary += "0123456789abcdef"[nibble(device)];
        return canary;
    }

    /// Names of the files in @p directory holding @p canary as ASCII or UTF-16LE bytes.
    std::vector<std::string> FilesContainingCanary(const std::filesystem::path& directory, const std::string& canary)
    {
        namespace fs = std::filesystem;
        std::string utf16;
        for (const char character : canary)
        {
            utf16 += character;
            utf16 += '\0';
        }

        std::vector<std::string> hits;
        std::error_code error;
        for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
        {
            if (!it->is_regular_file(error))
                continue;
            std::ifstream artifact(it->path(), std::ios::binary);
            std::ostringstream contents;
            contents << artifact.rdbuf();
            const std::string bytes = contents.str();
            if (bytes.find(canary) != std::string::npos || bytes.find(utf16) != std::string::npos)
                hits.push_back(it->path().filename().string());
        }
        return hits;
    }
} // namespace

TEST(CrashHandler_UngatedReportWritesAnArtifactAndTheAssertGateDoesNot)
{
    // Engine-lifecycle tests can install the crash handler before this test.
    // Capture those roots so that this test validates the fresh installation,
    // rather than an arbitrary older directory for the same process ID.
    const auto artifactDirectoriesBeforeInstall = FindCrashArtifactDirectories();

#ifdef SPARK_PLATFORM_WINDOWS
    // The suite installs its own unhandled-exception filter to report crashing
    // tests; InstallCrashHandler() replaces it, so put it back afterwards.
    LPTOP_LEVEL_EXCEPTION_FILTER harnessFilter = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(harnessFilter);
#else
    const ScopedCrashSignalDispositions harnessSignals;
#endif

    CrashConfig config;
    config.dumpPrefix = L"SparkTestCrash";
    config.captureScreenshot = false; // no swap chain in the test process
    config.captureSystemInfo = false; // no DXGI enumeration
    config.captureAllThreads = false; // no suspending the test runner's threads
    config.requireConsent = false;
    config.headlessMode = true; // no dialogs
    config.promptUserDescription = false;
    config.triggerCrashOnAssert = false; // the production default this test is about
    InstallCrashHandler(config);
#ifdef SPARK_PLATFORM_WINDOWS
    SetUnhandledExceptionFilter(harnessFilter);
#endif

    const std::filesystem::path artifacts = FindNewCrashArtifactDirectory(artifactDirectoriesBeforeInstall);
    ASSERT_FALSE(artifacts.empty());

    // Gated entry point with the toggle off: logs, writes no report.
    TriggerCrashHandler("gated-entry-probe-a1b2c3");
    EXPECT_EQ(CountReportsContaining(artifacts, "gated-entry-probe-a1b2c3"), static_cast<size_t>(0));

    // OPS-100: a secret live on this thread's stack and on the heap while the
    // report is written must not reach any artifact (dump, log or manifest).
    const std::string canary = MakeCrashCanary();
    volatile char stackSecret[64] = {};
    for (size_t index = 0; index < canary.size(); ++index)
        stackSecret[index] = canary[index];
    auto heapSecret = std::make_unique<char[]>(canary.size());
    std::memcpy(heapSecret.get(), canary.data(), canary.size());

    // Ungated entry point: this is the one FreezeDetector::OnCriticalFreeze and
    // Assert::Fail rely on, and it must leave an artifact on disk. A stubbed or
    // re-gated TriggerCrashReport fails here.
    TriggerCrashReport("ungated-entry-probe-d4e5f6");
    EXPECT_EQ(CountReportsContaining(artifacts, "ungated-entry-probe-d4e5f6"), static_cast<size_t>(1));

    const std::vector<std::string> leaked = FilesContainingCanary(artifacts, canary);
    for (const std::string& file : leaked)
        std::cerr << "  crash artifact holds the stack/heap canary: " << file << '\n';
    EXPECT_TRUE(leaked.empty());
    // Both copies stay live across the report, so the optimizer cannot drop them.
    const char stackFirst = stackSecret[0];
    EXPECT_EQ(stackFirst, 'S');
    EXPECT_EQ(heapSecret[0], 'S');

    // One report per process: the duplicate a fatal assert produces (gated call
    // followed by ungated call) must not write a second dump/log pair.
    TriggerCrashReport("duplicate-entry-probe-778899");
    EXPECT_EQ(CountReportsContaining(artifacts, "duplicate-entry-probe-778899"), static_cast<size_t>(0));

    // The isolated crash-security driver validates these exact bytes after this
    // process exits and releases its pinned directory handle. Normal runs keep
    // the existing cleanup behavior.
    const char* keepArtifacts = std::getenv("SPARK_TEST_KEEP_CRASH_ARTIFACTS");
    if (keepArtifacts == nullptr || std::string(keepArtifacts) != "1")
    {
        std::error_code error;
        std::filesystem::remove_all(artifacts, error);
    }
}

#endif // SPARK_TEST_CRASH_PRODUCER

#if defined(SPARK_PLATFORM_LINUX) && defined(SPARK_TEST_CRASH_PRODUCER)
namespace
{
    std::filesystem::path CrashPolicyTestBinary()
    {
        std::error_code error;
        const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
        return error ? std::filesystem::path{} : self;
    }

    std::string RunCrashPolicyProbe(const char* fullDump, const std::filesystem::path& scratch,
                                    const std::string& canary, bool signalFault = false)
    {
        const std::filesystem::path self = CrashPolicyTestBinary();
        ASSERT_FALSE(self.empty());
        std::filesystem::create_directories(scratch);

        Spark::Process::Builder builder("env");
        // env options must precede every NAME=value assignment.
        for (const char* selection : {"SPARK_TEST_FILE", "SPARK_TEST_NAME_PREFIX", "SPARK_TEST_EXPECT_COUNT",
                                      "SPARK_TEST_EXCLUDE", "SPARK_TEST_LIMIT", "SPARK_CRASH_FULL_DUMP"})
        {
            builder.Arg("-u").Arg(selection);
        }
        builder.Arg("SPARK_TEST_NAME=CrashPolicy_DefaultAndOptInKernelDumpPolicy");
        builder.Arg("SPARK_TEST_EXPECT_COUNT=1");
        builder.Arg(std::string("SPARK_CRASH_POLICY_PROBE=") + (signalFault ? "signal" : "policy"));
        builder.Arg("TMPDIR=" + scratch.string());
        builder.Arg("SPARK_TEST_CRASH_CANARY=" + canary);
        if (fullDump != nullptr)
        {
            builder.Arg(std::string("SPARK_CRASH_FULL_DUMP=") + fullDump);
        }
        builder.Arg(self.string()).Arg("--warn-is-error").Arg("--empty-is-error");
        builder.WorkingDirectory(scratch.string());
        builder.CaptureStdout().MergeStderrIntoStdout();

        auto child = builder.Launch();
        ASSERT_TRUE(child.has_value());
        Spark::Process process = std::move(*child);
        const bool exited = process.WaitForExit(std::chrono::seconds(20));
        if (!exited)
        {
            process.Kill();
            ASSERT_TRUE(process.WaitForExit(std::chrono::seconds(5)));
        }
        ASSERT_TRUE(exited);
        const std::string output = process.ReadAllStdout();
        // Process maps a POSIX signal termination to -1, rather than its signal number.
        EXPECT_EQ(process.GetExitCode().value_or(999), signalFault ? -1 : 0);
        EXPECT_STR_CONTAINS(output, "SPARK_CORE_POLICY_OK");
        return output;
    }
} // namespace

TEST(CrashPolicy_DefaultAndOptInKernelDumpPolicy)
{
    if (const char* role = std::getenv("SPARK_CRASH_POLICY_PROBE"))
    {
        // exec normally enables dumpability. Set it explicitly so an inherited
        // zero core limit cannot make the opt-in assertion vacuous.
        ASSERT_EQ(prctl(PR_SET_DUMPABLE, 1L, 0L, 0L, 0L), 0);
        struct rlimit before
        {
        };
        ASSERT_EQ(getrlimit(RLIMIT_CORE, &before), 0);
        ASSERT_EQ(prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L), 1);

        CrashConfig config;
        config.dumpPrefix = L"SparkCorePolicyTest";
        config.headlessMode = true;
        config.captureScreenshot = false;
        config.promptUserDescription = false;
        const char* fullDump = std::getenv("SPARK_CRASH_FULL_DUMP");
        config.includeStackMemory = fullDump != nullptr && std::string_view(fullDump) == "1";
        InstallCrashHandler(config);

        struct rlimit after
        {
        };
        ASSERT_EQ(getrlimit(RLIMIT_CORE, &after), 0);
        if (config.includeStackMemory)
        {
            ASSERT_EQ(after.rlim_cur, before.rlim_cur);
            ASSERT_EQ(after.rlim_max, before.rlim_max);
            ASSERT_EQ(prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L), 1);
        }
        else
        {
            ASSERT_EQ(after.rlim_cur, 0u);
            ASSERT_EQ(after.rlim_max, 0u);
            ASSERT_EQ(prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L), 0);
        }
        std::cout << "SPARK_CORE_POLICY_OK\n" << std::flush;
        if (std::string_view(role) == "signal")
        {
            ASSERT_FALSE(config.includeStackMemory);
            const std::string canary = MakeCrashCanary();
            volatile char stackSecret[64] = {};
            auto heapSecret = std::make_unique<char[]>(canary.size());
            volatile char* heapBytes = heapSecret.get();
            for (size_t index = 0; index < canary.size(); ++index)
            {
                stackSecret[index] = canary[index];
                heapBytes[index] = canary[index];
            }
            std::raise(SIGSEGV);
            // Reaching this means the production handler failed to terminate.
            std::_Exit(stackSecret[0] == heapBytes[0] ? 98 : 99);
        }
        return;
    }

    namespace fs = std::filesystem;
    const std::string canary = MakeCrashCanary();
    const fs::path scratch = fs::temp_directory_path() / ("spark_core_policy_" + canary.substr(kCanaryPrefix.size()));
    RunCrashPolicyProbe(nullptr, scratch / "default", canary);
    RunCrashPolicyProbe("0", scratch / "not-opted-in", canary);
    RunCrashPolicyProbe("1", scratch / "opt-in", canary);
    RunCrashPolicyProbe(nullptr, scratch / "signal", canary, true);

    size_t signalLogs = 0;
    size_t manifests = 0;
    for (const auto& entry : fs::recursive_directory_iterator(scratch / "signal"))
    {
        if (entry.is_directory())
        {
            EXPECT_TRUE(FilesContainingCanary(entry.path(), canary).empty());
            signalLogs += CountReportsContaining(entry.path(), "SIGSEGV");
        }
        else if (entry.path().extension() == ".json")
        {
            ++manifests;
        }
        // The .core_hint is text; it is not a captured memory image.
        EXPECT_FALSE(entry.path().filename() == "core" || entry.path().extension() == ".core");
    }
    EXPECT_GT(signalLogs, 0u);
    EXPECT_GT(manifests, 0u);
    fs::remove_all(scratch);
}
#endif

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


TEST(CrashSignalManifest_PreservesSchemaConsentAndCrashTime)
{
    Spark::CrashHandlerDetail::SignalCrashManifest manifest;
    manifest.processId = 4242;
    manifest.epochSeconds = 1709164800; // 2024-02-29T00:00:00Z
    manifest.logName = "quoted\"name.log";
    manifest.coreHintName = "local.core_hint";
    manifest.title = "SIGSEGV";
    manifest.requireConsent = true;
    manifest.allowScreenshotRefusal = false;
    manifest.promptUserDescription = true;
    manifest.fullMemoryDump = false;
    char output[2048]{};
    const size_t size = Spark::CrashHandlerDetail::FormatSignalCrashManifest(manifest, output, sizeof(output));
    ASSERT_TRUE(size > size_t{0});
    const std::string json(output, size);
    EXPECT_TRUE(json.find("\"enginePID\": \"4242\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"timestamp\": \"2024-02-29T00:00:00Z\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"logFile\": \"quoted\\\"name.log\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"dumpFile\": \"local.core_hint\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"screenshotFile\": \"\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"zipFile\": \"\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"requireConsent\": true") != std::string::npos);
    EXPECT_TRUE(json.find("\"allowScreenshotRefusal\": false") != std::string::npos);
    EXPECT_TRUE(json.find("\"promptUserDescription\": true") != std::string::npos);
    EXPECT_TRUE(json.find("\"fullMemoryDump\": false") != std::string::npos);
}

TEST(CrashSignalManifest_RejectsIncompleteOutputAndPreservesBoundaries)
{
    Spark::CrashHandlerDetail::SignalCrashManifest manifest;
    manifest.logName = "local.log";
    char output[2048]{};
    manifest.epochSeconds = 0;
    ASSERT_TRUE(Spark::CrashHandlerDetail::FormatSignalCrashManifest(manifest, output, sizeof(output)) > size_t{0});
    EXPECT_TRUE(std::string(output).find("1970-01-01T00:00:00Z") != std::string::npos);
    manifest.epochSeconds = 253402300799ULL;
    ASSERT_TRUE(Spark::CrashHandlerDetail::FormatSignalCrashManifest(manifest, output, sizeof(output)) > size_t{0});
    EXPECT_TRUE(std::string(output).find("9999-12-31T23:59:59Z") != std::string::npos);
    ++manifest.epochSeconds;
    EXPECT_EQ(Spark::CrashHandlerDetail::FormatSignalCrashManifest(manifest, output, sizeof(output)), size_t{0});
    manifest.epochSeconds = 0;
    char boundedOutput[33];
    std::memset(boundedOutput, 'X', sizeof(boundedOutput));
    EXPECT_EQ(Spark::CrashHandlerDetail::FormatSignalCrashManifest(manifest, boundedOutput, 32), size_t{0});
    EXPECT_EQ(boundedOutput[32], 'X');
    EXPECT_EQ(Spark::CrashHandlerDetail::FormatSignalCrashManifest(manifest, nullptr, 0), size_t{0});
    manifest.logName = {};
    EXPECT_EQ(Spark::CrashHandlerDetail::FormatSignalCrashManifest(manifest, output, sizeof(output)), size_t{0});
}

TEST(CrashSignalManifest_ReservesFatalSlotWithinExistingQueueBound)
{
    using Spark::CrashHandlerDetail::HasNonfatalCrashManifestCapacity;
    EXPECT_TRUE(HasNonfatalCrashManifestCapacity(30, true));
    EXPECT_FALSE(HasNonfatalCrashManifestCapacity(31, true));
    EXPECT_FALSE(HasNonfatalCrashManifestCapacity(32, true));
    EXPECT_TRUE(HasNonfatalCrashManifestCapacity(31, false));
    EXPECT_FALSE(HasNonfatalCrashManifestCapacity(32, false));
}

TEST(CrashSignalManifest_IncompleteQueueScanFailsClosed)
{
    using Spark::CrashHandlerDetail::CrashManifestCountOrFull;
    EXPECT_EQ(CrashManifestCountOrFull(0, true), size_t{0});
    EXPECT_EQ(CrashManifestCountOrFull(30, true), size_t{30});
    EXPECT_EQ(CrashManifestCountOrFull(33, true), size_t{32});
    EXPECT_EQ(CrashManifestCountOrFull(0, false), size_t{32});
    EXPECT_EQ(CrashManifestCountOrFull(30, false), size_t{32});
    EXPECT_FALSE(Spark::CrashHandlerDetail::HasNonfatalCrashManifestCapacity(CrashManifestCountOrFull(0, false), true));
}
