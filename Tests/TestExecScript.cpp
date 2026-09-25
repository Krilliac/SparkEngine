/**
 * @file TestExecScript.cpp
 * @brief TF-110: shared `-exec` timeline parser/scheduler, `-test-seconds`, and credential redaction.
 *
 * The unit tests drive Core/ExecScript directly against the process console.
 * The Linux process test launches the built SparkEngine headless with the
 * TERRAFRONT module, a scripted dedicated-server timeline, a private
 * `-exec-audit` file and `-test-seconds`, proving the Linux entry point
 * parses and runs the same timeline the Windows smokes use and that a
 * sensitive command's password never reaches the audit trail or the log.
 */

#include "TestFramework.h"

#include "Core/ExecScript.h"
#include "Utils/SparkConsole.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include "Utils/Process.h"

#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
    std::string ReadWholeFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::filesystem::path UniqueTempPath(const std::string& stem)
    {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() /
               (stem + "-" + std::to_string(stamp) + "-" + std::to_string(++counter));
    }

    std::vector<Spark::ScriptedCommand> Parse(const std::string& text)
    {
        std::istringstream input(text);
        return Spark::ParseExecScript(input);
    }
} // namespace

TEST(ExecScript_ParserReadsTimedFrameCommentAndCrlfEntries)
{
    const auto commands = Parse("# comment line\r\n"
                                "\r\n"
                                "t1.5 tf_status\r\n"
                                "30 tf_perf   \r\n"
                                "plain_command arg\n"
                                "t2 tf_dedicated 27120\n");
    ASSERT_EQ(commands.size(), size_t{4});

    // Sorted by due time: frame 0 (0 s), frame 30 (0.5 s at 60 fps), t1.5, t2.
    EXPECT_EQ(commands[0].command, std::string("plain_command arg"));
    EXPECT_EQ(commands[0].frame, 0);
    EXPECT_TRUE(commands[0].atSec < 0.0);

    EXPECT_EQ(commands[1].command, std::string("tf_perf"));
    EXPECT_EQ(commands[1].frame, 30);

    EXPECT_EQ(commands[2].command, std::string("tf_status"));
    EXPECT_NEAR(commands[2].atSec, 1.5, 1e-9);

    EXPECT_EQ(commands[3].command, std::string("tf_dedicated 27120"));
    EXPECT_NEAR(commands[3].atSec, 2.0, 1e-9);
}

TEST(ExecScript_ParserTreatsMalformedPrefixesAsFrameZeroCommands)
{
    const auto commands = Parse("tfoo bar\n"
                                "t1.2.3 twice_dotted\n"
                                "t5\n"
                                "99999999999999999999 overflowing_frame\n");
    ASSERT_EQ(commands.size(), size_t{4});
    for (const auto& command : commands)
    {
        EXPECT_EQ(command.frame, 0);
        EXPECT_TRUE(command.atSec < 0.0);
    }
    EXPECT_EQ(commands[0].command, std::string("tfoo bar"));
    EXPECT_EQ(commands[1].command, std::string("t1.2.3 twice_dotted"));
    EXPECT_EQ(commands[2].command, std::string("t5"));
    EXPECT_EQ(commands[3].command, std::string("99999999999999999999 overflowing_frame"));
}

TEST(ExecScript_SchedulerRunsInDueOrderAndKeepsFileOrderForTies)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    EXPECT_TRUE(console.Initialize());

    std::vector<std::string> ran;
    console.RegisterCommand("exec_order_probe",
                            [&](const std::vector<std::string>& args)
                            {
                                ran.push_back(args.empty() ? std::string() : args[0]);
                                return std::string{};
                            });

    const auto auditPath = UniqueTempPath("spark-exec-order") += ".log";
    Spark::ExecScriptPlayer player;
    player.SetAuditPath(auditPath.string());
    player.Load(Parse("t1 exec_order_probe t1-first\n"
                      "t1 exec_order_probe t1-second\n"
                      "5 exec_order_probe frame5\n"
                      "exec_order_probe frame0\n"
                      "t0.5 exec_order_probe t0.5\n"));
    EXPECT_EQ(player.GetPendingCount(), size_t{5});

    EXPECT_EQ(player.RunDueAt(0, 0.0, console), size_t{1});
    EXPECT_EQ(player.RunDueAt(4, 0.2, console), size_t{0});
    // Frame 5 (~0.083 s) is due; t0.5 is not yet.
    EXPECT_EQ(player.RunDueAt(5, 0.3, console), size_t{1});
    // A late frame releases everything whose time has come, in schedule order.
    EXPECT_EQ(player.RunDueAt(6, 1.0, console), size_t{3});
    EXPECT_EQ(player.GetPendingCount(), size_t{0});
    EXPECT_EQ(player.RunDueAt(7, 9.0, console), size_t{0});

    std::string order;
    for (const auto& entry : ran)
        order += entry + ";";
    EXPECT_EQ(order, std::string("frame0;frame5;t0.5;t1-first;t1-second;"));

    // Byte-compatible audit line format consumed by Tests/PackageSmoke/*.cmake.
    const std::string audit = ReadWholeFile(auditPath);
    EXPECT_STR_CONTAINS(audit, "frame 0 t=0.0s | ok  | exec_order_probe frame0\n");
    EXPECT_STR_CONTAINS(audit, "frame 6 t=1.0s | ok  | exec_order_probe t1-second\n");

    EXPECT_TRUE(console.UnregisterCommand("exec_order_probe"));
    std::error_code error;
    std::filesystem::remove(auditPath, error);
}

TEST(ExecScript_UnknownCommandIsAuditedAsError)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    EXPECT_TRUE(console.Initialize());

    const auto auditPath = UniqueTempPath("spark-exec-err") += ".log";
    Spark::ExecScriptPlayer player;
    player.SetAuditPath(auditPath.string());
    player.Load(Parse("exec_command_that_does_not_exist\n"));
    EXPECT_EQ(player.RunDueAt(0, 0.0, console), size_t{1});
    EXPECT_STR_CONTAINS(ReadWholeFile(auditPath), "| ERR | exec_command_that_does_not_exist\n");

    std::error_code error;
    std::filesystem::remove(auditPath, error);
}

TEST(ExecScript_RedactsSensitiveArgumentsInConsoleAndAudit)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    EXPECT_TRUE(console.Initialize());

    const std::string secret = "exec-redaction-Pa55word";
    std::vector<std::string> received;
    console.RegisterSensitiveCommand("exec_login_probe",
                                     [&](const std::vector<std::string>& args)
                                     {
                                         received = args;
                                         return std::string("[probe] login handled");
                                     });
    console.SetAlias("exec_login_alias", "exec_login_probe aliasuser " + secret);

    EXPECT_EQ(console.RedactSensitiveArguments("exec_login_probe user " + secret),
              std::string("exec_login_probe <arguments-redacted>"));
    EXPECT_EQ(console.RedactSensitiveArguments("exec_login_alias"),
              std::string("exec_login_alias <arguments-redacted>"));
    // Nothing to redact on a bare command name, registered or not.
    EXPECT_EQ(console.RedactSensitiveArguments("tf_status"), std::string("tf_status"));

    const auto auditPath = UniqueTempPath("spark-exec-redact") += ".log";
    Spark::ExecScriptPlayer player;
    player.SetAuditPath(auditPath.string());
    player.Load(Parse("t1 exec_login_probe user " + secret + "\n2 exec_login_alias\n"));
    EXPECT_EQ(player.RunDueAt(3, 1.0, console), size_t{2});

    // The handler still receives the real credential.
    ASSERT_EQ(received.size(), size_t{2});
    EXPECT_EQ(received[1], secret);

    const std::string audit = ReadWholeFile(auditPath);
    EXPECT_STR_CONTAINS(audit, "| ok  | exec_login_probe <arguments-redacted>\n");
    EXPECT_STR_CONTAINS(audit, "| ok  | exec_login_alias <arguments-redacted>\n");
    EXPECT_STR_CONTAINS(audit, "[probe] login handled");
    EXPECT_TRUE(audit.find(secret) == std::string::npos);

    for (const auto& entry : console.GetLogHistory())
        EXPECT_TRUE(entry.message.find(secret) == std::string::npos);

    console.RemoveAlias("exec_login_alias");
    EXPECT_TRUE(console.UnregisterCommand("exec_login_probe"));
    std::error_code error;
    std::filesystem::remove(auditPath, error);
}

TEST(ExecScript_UnregisteredCommandArgumentsAreRedactedFailClosed)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    EXPECT_TRUE(console.Initialize());

    // No module registered tf_login here: its credential must still stay out of every sink.
    const std::string secret = "UnloadedModulePl41nS3cret";
    ASSERT_FALSE(console.HasCommand("tf_login"));
    EXPECT_EQ(console.RedactSensitiveArguments("tf_login alice " + secret),
              std::string("tf_login <arguments-redacted>"));

    // A registered, non-sensitive command keeps its arguments for diagnosis.
    console.RegisterCommand("exec_plain_probe", [](const std::vector<std::string>&) { return std::string{}; });
    EXPECT_EQ(console.RedactSensitiveArguments("exec_plain_probe value"), std::string("exec_plain_probe value"));

    const auto auditPath = UniqueTempPath("spark-exec-unloaded") += ".log";
    Spark::ExecScriptPlayer player;
    player.SetAuditPath(auditPath.string());
    player.Load(Parse("t0.2 tf_login alice " + secret + "\n"));
    EXPECT_EQ(player.RunDueAt(12, 0.2, console), size_t{1});

    const std::string audit = ReadWholeFile(auditPath);
    EXPECT_STR_CONTAINS(audit, "frame 12 t=0.2s | ERR | tf_login <arguments-redacted>\n");
    EXPECT_TRUE(audit.find(secret) == std::string::npos);
    for (const auto& entry : console.GetLogHistory())
        EXPECT_TRUE(entry.message.find(secret) == std::string::npos);

    EXPECT_TRUE(console.UnregisterCommand("exec_plain_probe"));
    std::error_code error;
    std::filesystem::remove(auditPath, error);
}

TEST(ExecScript_TestSecondsLimitAndMissingScript)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    EXPECT_TRUE(console.Initialize());

    Spark::ExecScriptPlayer player;
    EXPECT_FALSE(player.TestSecondsLimitReached());
    player.SetTestSecondsLimit(-3.0);
    EXPECT_EQ(player.GetTestSecondsLimit(), 0.0);
    EXPECT_FALSE(player.TestSecondsLimitReached());

    player.SetTestSecondsLimit(1e-9);
    EXPECT_TRUE(player.ElapsedSeconds() >= 0.0);
    // The clock started on the first call above; any later read exceeds a nanosecond.
    while (player.ElapsedSeconds() < 1e-6)
    {
    }
    EXPECT_TRUE(player.TestSecondsLimitReached());

    EXPECT_FALSE(player.LoadFile(UniqueTempPath("spark-exec-missing").string() + ".cfg", console));
    EXPECT_EQ(player.GetPendingCount(), size_t{0});
}

#if defined(__linux__)

namespace
{
    std::filesystem::path TestBinaryDirectory()
    {
        std::error_code error;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", error);
        return error ? std::filesystem::path{} : exe.parent_path();
    }

    /// A UDP port that was free a moment ago, so concurrent test runs do not collide on a fixed port.
    uint16_t ProbeFreeUdpPort()
    {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            return 27120;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        uint16_t port = 27120;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0)
            port = ntohs(address.sin_port);
        ::close(fd);
        return port;
    }

    /// Lift RLIMIT_FSIZE for the child: the module copy can exceed the sanitizer wrapper's soft cap.
    class ScopedUnboundedFileSize
    {
      public:
        ScopedUnboundedFileSize()
        {
            m_saved = ::getrlimit(RLIMIT_FSIZE, &m_previous) == 0;
            if (m_saved && m_previous.rlim_cur != m_previous.rlim_max)
            {
                rlimit raised = m_previous;
                raised.rlim_cur = m_previous.rlim_max;
                ::setrlimit(RLIMIT_FSIZE, &raised);
            }
        }
        ~ScopedUnboundedFileSize()
        {
            if (m_saved)
                ::setrlimit(RLIMIT_FSIZE, &m_previous);
        }
        ScopedUnboundedFileSize(const ScopedUnboundedFileSize&) = delete;
        ScopedUnboundedFileSize& operator=(const ScopedUnboundedFileSize&) = delete;

      private:
        rlimit m_previous{};
        bool m_saved = false;
    };
} // namespace

TEST(ExecScript_LinuxHeadlessRunsTimelineWithRedactedAudit)
{
    const auto bin = TestBinaryDirectory();
    const auto engine = bin / "SparkEngine";
    const auto module = bin / "libSparkGameMMOFPS.so";
    std::error_code error;
    if (!std::filesystem::is_regular_file(engine, error) || !std::filesystem::is_regular_file(module, error))
        SKIP_TEST("SparkEngine or libSparkGameMMOFPS.so was not built in this configuration");

    const auto workDir = UniqueTempPath("spark-exec-process");
    std::filesystem::create_directories(workDir, error);
    ASSERT_FALSE(static_cast<bool>(error));
    const auto scriptPath = workDir / "timeline.cfg";
    const auto auditPath = workDir / "server-audit.log";
    const std::string secret = "ExecAuditS3cretPw";
    const std::string port = std::to_string(ProbeFreeUdpPort());
    {
        std::ofstream script(scriptPath, std::ios::binary);
        script << "# TF-110 Linux -exec parity\r\n"
               << "t1 tf_dedicated " << port << "\r\n"
               << "t2 tf_status\r\n"
               << "t2 tf_register exec_audit_user " << secret << "\r\n";
    }

    const ScopedUnboundedFileSize fileSizeLimit;
    auto launched = Spark::Process::Builder(engine.string())
                        .Arg("-headless")
                        .Arg("-no-subprocess")
                        .Arg("-game")
                        .Arg(module.string())
                        .Arg("-require-game")
                        .Arg("-exec")
                        .Arg(scriptPath.string())
                        .Arg("-exec-audit")
                        .Arg(auditPath.string())
                        .Arg("-test-seconds")
                        .Arg("4")
                        // Safety net only: without -test-seconds support the run would stop here
                        // (about 15 s at 60 Hz) and fail the frame assertion instead of hanging.
                        .Arg("-test-frames")
                        .Arg("900")
                        .WorkingDirectory(bin.string())
                        .CaptureStdout()
                        .MergeStderrIntoStdout()
                        .Launch();
    ASSERT_TRUE(launched.has_value());

    const auto launchedAt = std::chrono::steady_clock::now();
    const std::string output = launched->ReadAllStdout();
    const int exitCode = launched->WaitForExit();
    const double runSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - launchedAt).count();
    const std::string audit = ReadWholeFile(auditPath);

    EXPECT_EQ(exitCode, 0);
    // -test-seconds 4 ended the run. Judge that from the child's own tick
    // count, not the host clock, so slow sanitizer boot and teardown cannot
    // flake it: only the 900-frame cap (about 15 s at 60 Hz) could stop the
    // loop at frame 900. The host clock is used only as a lower bound.
    const std::string statsMarker = "SPARK_HEADLESS_TICK_STATS ";
    const size_t statsAt = output.find(statsMarker);
    ASSERT_TRUE(statsAt != std::string::npos);
    const size_t framesAt = output.find("frames=", statsAt);
    ASSERT_TRUE(framesAt != std::string::npos);
    int loopFrames = 0;
    ASSERT_EQ(std::sscanf(output.c_str() + framesAt, "frames=%d", &loopFrames), 1);
    EXPECT_GT(loopFrames, 0);
    EXPECT_LT(loopFrames, 900);
    EXPECT_GE(runSeconds, 4.0);

    // The first audited command carries the boot history, including the loader's line.
    EXPECT_STR_CONTAINS(audit, "[exec] loaded 3 scripted commands from ");

    EXPECT_STR_CONTAINS(audit, "| ok  | tf_dedicated " + port + "\n");
    EXPECT_STR_CONTAINS(audit, "[TF] dedicated server started on port " + port);
    EXPECT_STR_CONTAINS(audit, "| ok  | tf_status\n");
    EXPECT_STR_CONTAINS(audit, "[TF] TERRAFRONT  role=dedicated");
    EXPECT_STR_CONTAINS(audit, "| ok  | tf_register <arguments-redacted>\n");
    EXPECT_TRUE(audit.find(secret) == std::string::npos);
    EXPECT_TRUE(output.find(secret) == std::string::npos);
    // The default trail must not be written when -exec-audit redirects it.
    EXPECT_FALSE(std::filesystem::exists(workDir / "exec_audit.log"));

    if (exitCode != 0 || audit.empty())
    {
        constexpr size_t kTailBytes = 4096;
        std::fprintf(stderr, "---- SparkEngine child output (tail) ----\n%s\n----\n",
                     output.substr(output.size() > kTailBytes ? output.size() - kTailBytes : 0).c_str());
    }
    std::filesystem::remove_all(workDir, error);
}

TEST(ExecScript_LinuxRejectsUnreadableScriptAndBadSeconds)
{
    const auto engine = TestBinaryDirectory() / "SparkEngine";
    std::error_code error;
    if (!std::filesystem::is_regular_file(engine, error))
        SKIP_TEST("SparkEngine was not built in this configuration");

    auto missingScript = Spark::Process::Builder(engine.string())
                             .Arg("-headless")
                             .Arg("-exec")
                             .Arg(UniqueTempPath("spark-exec-absent").string() + ".cfg")
                             .Arg("-test-frames")
                             .Arg("5")
                             .CaptureStdout()
                             .MergeStderrIntoStdout()
                             .Launch();
    ASSERT_TRUE(missingScript.has_value());
    const std::string missingOutput = missingScript->ReadAllStdout();
    EXPECT_NE(missingScript->WaitForExit(), 0);
    EXPECT_STR_CONTAINS(missingOutput, "cannot open -exec script");

    auto badSeconds = Spark::Process::Builder(engine.string())
                          .Arg("-headless")
                          .Arg("-test-seconds")
                          .Arg("4s")
                          .Arg("-test-frames")
                          .Arg("5")
                          .CaptureStdout()
                          .MergeStderrIntoStdout()
                          .Launch();
    ASSERT_TRUE(badSeconds.has_value());
    const std::string badOutput = badSeconds->ReadAllStdout();
    EXPECT_NE(badSeconds->WaitForExit(), 0);
    EXPECT_STR_CONTAINS(badOutput, "-test-seconds expects a positive number");

    // from_chars parses these, but none is a usable limit: each must fail the launch, not disable the limit.
    for (const char* unusable : {"nan", "inf", "-3", "0"})
    {
        auto rejected = Spark::Process::Builder(engine.string())
                            .Arg("-headless")
                            .Arg("-test-seconds")
                            .Arg(unusable)
                            .Arg("-test-frames")
                            .Arg("5")
                            .CaptureStdout()
                            .MergeStderrIntoStdout()
                            .Launch();
        ASSERT_TRUE(rejected.has_value());
        const std::string rejectedOutput = rejected->ReadAllStdout();
        EXPECT_NE(rejected->WaitForExit(), 0);
        EXPECT_STR_CONTAINS(rejectedOutput, std::string("-test-seconds expects a positive number, got '") + unusable);
    }
}

#endif // __linux__
