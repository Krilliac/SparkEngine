/**
 * @file TestPLT210RuntimeProcessesReal.cpp
 * @brief PLT-210 Linux runtime defects proven against the built executables.
 *
 *  - SparkConsole --engine-pipe: stdout is the engine's command channel, so it
 *    must carry commands only. It used to echo every engine log line, its prompt
 *    and results there, which the engine re-executed as "Unknown command"s.
 *  - SparkGameMMOFPS: libSparkGameMMOFPS.so failed dlopen(RTLD_NOW) because three
 *    GraphicsEngine basic-path methods had no Linux definition.
 *
 * The process tests launch the sibling binaries of SparkTests (bin/) and skip
 * only when a configuration did not build them.
 */

#include "TestFramework.h"

#if defined(__linux__)

#include "Graphics/GraphicsEngine.h"
#include "Utils/Process.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include <sys/resource.h>

namespace
{
    std::filesystem::path TestBinaryDirectory()
    {
        std::error_code error;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", error);
        return error ? std::filesystem::path{} : exe.parent_path();
    }

    // The sanitizer wrapper caps the soft RLIMIT_FSIZE at 16 MiB (inherited by children) and
    // leaves the hard limit so tests can lift it. ModuleManager copies the module into a private
    // staging directory before dlopen, and a sanitizer-instrumented Debug libSparkGameMMOFPS.so is
    // larger than that cap, so the child's copy fails with EFBIG. Lift the soft limit to the hard
    // limit for the child's lifetime and restore it afterwards.
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

TEST(PLT210_GraphicsBasicPath_MMOFPSSurfaceDefinedOnLinux)
{
    GraphicsEngine graphics;
    graphics.Initialize(nullptr);

    // These are the symbols libSparkGameMMOFPS.so imports; calling them proves
    // the Linux engine defines them, and they must be safe no-ops on the RHI path.
    graphics.SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Alpha);
    graphics.SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Additive);
    graphics.SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
    graphics.SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);
    graphics.SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
    EXPECT_TRUE(graphics.GetOrCreateSoftCircleShadowSRV() == nullptr);
    graphics.SetBasicTexture(graphics.GetOrCreateSoftCircleShadowSRV());

    graphics.Shutdown();
}

TEST(PLT210_Module_MMOFPSLoadsInHeadlessEngine)
{
    const auto bin = TestBinaryDirectory();
    const auto engine = bin / "SparkEngine";
    const auto module = bin / "libSparkGameMMOFPS.so";
    std::error_code error;
    if (!std::filesystem::is_regular_file(engine, error) || !std::filesystem::is_regular_file(module, error))
        SKIP_TEST("SparkEngine or libSparkGameMMOFPS.so was not built in this configuration");

    const ScopedUnboundedFileSize fileSizeLimit;
    auto launched = Spark::Process::Builder(engine.string())
                        .Arg("-headless")
                        .Arg("-no-subprocess")
                        .Arg("-game")
                        .Arg(module.string())
                        .Arg("-require-game")
                        .Arg("-test-frames")
                        .Arg("5")
                        .WorkingDirectory(bin.string())
                        .CaptureStdout()
                        .MergeStderrIntoStdout()
                        .Launch();
    ASSERT_TRUE(launched.has_value());

    const std::string output = launched->ReadAllStdout();
    // -require-game exits 2 when no module initialized (e.g. dlopen failed).
    const int exitCode = launched->WaitForExit();
    EXPECT_EQ(exitCode, 0);
    EXPECT_STR_CONTAINS(output, "SPARK_MODULE_READY count=1");
    if (exitCode != 0)
    {
        // Assertion messages truncate the child output; print the tail, where the load error is.
        constexpr size_t kTailBytes = 4096;
        std::fprintf(stderr, "---- SparkEngine child output (tail) ----\n%s\n----\n",
                     output.substr(output.size() > kTailBytes ? output.size() - kTailBytes : 0).c_str());
    }
}

TEST(PLT210_SparkConsole_EnginePipeStdoutCarriesOnlyCommands)
{
    const auto console = TestBinaryDirectory() / "SparkConsole";
    std::error_code error;
    if (!std::filesystem::is_regular_file(console, error))
        SKIP_TEST("SparkConsole was not built in this configuration");

    // Launched exactly as ConsoleProcessManager does: stdin carries engine
    // logs, stdout is read back as console commands.
    auto launched = Spark::Process::Builder(console.string())
                        .Arg("--engine-pipe")
                        .CaptureStdin()
                        .CaptureStdout()
                        .CaptureStderr()
                        .Launch();
    ASSERT_TRUE(launched.has_value());

    for (int i = 0; i < 5; ++i)
        launched->WriteStdin("[INFO] [Core] PLT210 engine log line " + std::to_string(i) + "\n");

    // Let the console render several prompt cycles (it redraws every 100 ms).
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    launched->CloseStdin();
    if (!launched->WaitForExit(std::chrono::milliseconds(2000)))
        launched->Kill();

    std::string stdoutText;
    std::string line;
    while (launched->TryReadLine(line))
        stdoutText += line + "\n";
    stdoutText += launched->ReadAllStdout();
    const std::string stderrText = launched->ReadAllStderr();

    // Nothing was typed, so the command channel must stay empty: no echoed
    // log lines, prompts, banners or results for the engine to execute.
    EXPECT_EQ(stdoutText, std::string());
    // The logs were received and displayed on the human-facing stream instead.
    EXPECT_STR_CONTAINS(stderrText, "PLT210 engine log line 4");
}

#endif // __linux__
