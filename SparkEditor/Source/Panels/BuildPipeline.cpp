/**
 * @file BuildPipeline.cpp
 * @brief Async CMake subprocess management for build orchestration
 */

#include "BuildPipeline.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace SparkEditor
{
    namespace
    {
        std::string FormatCommandForLog(const std::string& executable, const std::vector<std::string>& arguments)
        {
            std::ostringstream stream;
            stream << executable;
            for (const auto& argument : arguments)
            {
                stream << ' ';
                const bool needsQuotes = argument.find_first_of(" \t\"") != std::string::npos;
                if (!needsQuotes)
                {
                    stream << argument;
                    continue;
                }

                stream << '"';
                for (char c : argument)
                {
                    if (c == '"')
                        stream << '\\';
                    stream << c;
                }
                stream << '"';
            }
            return stream.str();
        }

#ifdef _WIN32
        std::string QuoteWindowsArgument(const std::string& argument)
        {
            if (argument.empty())
                return "\"\"";

            const bool needsQuotes = argument.find_first_of(" \t\"") != std::string::npos;
            if (!needsQuotes)
                return argument;

            std::string quoted = "\"";
            size_t backslashes = 0;
            for (char c : argument)
            {
                if (c == '\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == '"')
                {
                    quoted.append(backslashes * 2 + 1, '\\');
                    quoted.push_back(c);
                }
                else
                {
                    quoted.append(backslashes, '\\');
                    quoted.push_back(c);
                }
                backslashes = 0;
            }
            quoted.append(backslashes * 2, '\\');
            quoted.push_back('"');
            return quoted;
        }
#endif
    } // namespace

    BuildPipeline::BuildPipeline() = default;

    BuildPipeline::~BuildPipeline()
    {
        Cancel();
        if (m_worker.joinable())
            m_worker.join();
    }

    // ========================================================================
    // Public API
    // ========================================================================

    bool BuildPipeline::StartBuild(const BuildSettings& settings, const std::string& projectRoot)
    {
        if (m_running.load())
            return false;

        if (m_worker.joinable())
            m_worker.join();

        m_running.store(true);
        m_cancelRequested.store(false);
        m_progress.store(0.0f);
        m_result.store(BuildResult::None);
        {
            std::lock_guard lk(m_logMutex);
            m_logQueue.clear();
        }

        auto preset = SettingsToPreset(settings);
        auto buildType = SettingsToBuildType(settings);
        auto defines = SettingsToDefines(settings);
        std::string buildDir = projectRoot + "/build";

        PushLog(BuildLogLine::Level::Info, "Starting " + buildType + " build (preset: " + preset + ")");

        m_worker = std::thread(&BuildPipeline::WorkerThread, this, std::move(preset), std::move(buildType),
                               std::move(buildDir), projectRoot, std::move(defines));
        return true;
    }

    bool BuildPipeline::StartCookOnly(const BuildSettings& settings, const std::string& projectRoot)
    {
        if (m_running.load())
            return false;

        if (m_worker.joinable())
            m_worker.join();

        m_running.store(true);
        m_cancelRequested.store(false);
        m_progress.store(0.0f);
        m_result.store(BuildResult::None);
        {
            std::lock_guard lk(m_logMutex);
            m_logQueue.clear();
        }

        std::string outputDir = projectRoot + "/" + settings.outputDirectory;
        m_worker = std::thread(&BuildPipeline::CookWorkerThread, this, std::move(outputDir), projectRoot);
        return true;
    }

    void BuildPipeline::Cancel()
    {
        if (!m_running.load())
            return;
        m_cancelRequested.store(true);

#ifdef _WIN32
        if (m_processHandle)
            TerminateProcess(m_processHandle, 1);
#else
        if (m_childPid > 0)
            kill(m_childPid, SIGTERM);
#endif
    }

    std::vector<BuildLogLine> BuildPipeline::DrainLogLines()
    {
        std::lock_guard lk(m_logMutex);
        std::vector<BuildLogLine> out;
        out.swap(m_logQueue);
        return out;
    }

    std::string BuildPipeline::GetStatusText() const
    {
        std::lock_guard lk(m_statusMutex);
        return m_statusText;
    }

    // ========================================================================
    // Worker threads
    // ========================================================================

    void BuildPipeline::WorkerThread(std::string cmakePreset, std::string buildType, std::string buildDir,
                                     std::string projectRoot, std::vector<std::string> extraDefines)
    {
        // Phase 1 — Configure
        {
            std::lock_guard lk(m_statusMutex);
            m_statusText = "Configuring...";
        }
        m_progress.store(0.05f);

        // Validate preset name (alphanumeric, dash, underscore only)
        for (char c : cmakePreset)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            {
                PushLog(BuildLogLine::Level::Error, "Invalid preset name: " + cmakePreset);
                m_result.store(BuildResult::Failed);
                m_running.store(false);
                return;
            }
        }

        std::vector<std::string> configArgs = {"--preset", cmakePreset};
        for (const auto& def : extraDefines)
        {
            // Validate defines contain no shell metacharacters
            bool safe = true;
            for (char c : def)
            {
                if (c == ';' || c == '|' || c == '&' || c == '$' || c == '`' || c == '\n')
                {
                    safe = false;
                    break;
                }
            }
            if (!safe)
            {
                PushLog(BuildLogLine::Level::Error, "Unsafe characters in define: " + def);
                m_result.store(BuildResult::Failed);
                m_running.store(false);
                return;
            }
            configArgs.push_back("-D" + def);
        }

        PushLog(BuildLogLine::Level::Info, "> " + FormatCommandForLog("cmake", configArgs));
        int configExit = RunCommand("cmake", configArgs);

        if (m_cancelRequested.load())
        {
            m_result.store(BuildResult::Cancelled);
            m_running.store(false);
            return;
        }
        if (configExit != 0)
        {
            PushLog(BuildLogLine::Level::Error, "CMake configure failed (exit " + std::to_string(configExit) + ")");
            m_result.store(BuildResult::Failed);
            m_running.store(false);
            return;
        }

        m_progress.store(0.15f);

        // Phase 2 — Build
        {
            std::lock_guard lk(m_statusMutex);
            m_statusText = "Compiling...";
        }

        std::vector<std::string> buildArgs = {"--build", buildDir, "--config", buildType};

        PushLog(BuildLogLine::Level::Info, "> " + FormatCommandForLog("cmake", buildArgs));
        int buildExit = RunCommand("cmake", buildArgs);

        if (m_cancelRequested.load())
        {
            m_result.store(BuildResult::Cancelled);
            m_running.store(false);
            return;
        }

        m_progress.store(1.0f);

        if (buildExit != 0)
        {
            PushLog(BuildLogLine::Level::Error, "Build failed (exit " + std::to_string(buildExit) + ")");
            {
                std::lock_guard lk(m_statusMutex);
                m_statusText = "Build Failed";
            }
            m_result.store(BuildResult::Failed);
        }
        else
        {
            PushLog(BuildLogLine::Level::Info, "Build completed successfully!");
            {
                std::lock_guard lk(m_statusMutex);
                m_statusText = "Build Complete";
            }
            m_result.store(BuildResult::Success);
        }

        m_running.store(false);
    }

    void BuildPipeline::CookWorkerThread(std::string outputDir, std::string projectRoot)
    {
        {
            std::lock_guard lk(m_statusMutex);
            m_statusText = "Cooking assets...";
        }
        PushLog(BuildLogLine::Level::Info, "Cooking assets to " + outputDir);

        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(outputDir, ec);
        if (ec)
        {
            PushLog(BuildLogLine::Level::Error, "Failed to create output directory: " + ec.message());
            m_result.store(BuildResult::Failed);
            m_running.store(false);
            return;
        }

        // Copy assets directory if it exists
        std::string assetsDir = projectRoot + "/Assets";
        if (fs::exists(assetsDir, ec))
        {
            m_progress.store(0.3f);
            PushLog(BuildLogLine::Level::Info, "Copying assets from " + assetsDir);
            fs::copy(assetsDir, outputDir + "/Assets", fs::copy_options::recursive | fs::copy_options::update_existing,
                     ec);
            if (ec)
                PushLog(BuildLogLine::Level::Warning, "Asset copy warning: " + ec.message());
        }
        else
        {
            PushLog(BuildLogLine::Level::Warning, "No Assets directory found — skipping asset cook");
        }

        m_progress.store(1.0f);
        PushLog(BuildLogLine::Level::Info, "Asset cooking complete");
        {
            std::lock_guard lk(m_statusMutex);
            m_statusText = "Cook Complete";
        }
        m_result.store(BuildResult::Success);
        m_running.store(false);
    }

    // ========================================================================
    // Subprocess execution
    // ========================================================================

    int BuildPipeline::RunCommand(const std::string& executable, const std::vector<std::string>& arguments)
    {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        {
            PushLog(BuildLogLine::Level::Error, "Failed to create subprocess pipe");
            return -1;
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA startup = {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        startup.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION process = {};
        std::string commandLine = QuoteWindowsArgument(executable);
        for (const auto& argument : arguments)
        {
            commandLine.push_back(' ');
            commandLine += QuoteWindowsArgument(argument);
        }

        BOOL ok = CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup, &process);
        CloseHandle(writePipe);
        if (!ok)
        {
            CloseHandle(readPipe);
            PushLog(BuildLogLine::Level::Error, "Failed to launch subprocess");
            return -1;
        }

        {
            std::lock_guard lock(m_statusMutex);
            m_processHandle = process.hProcess;
        }

        std::array<char, 512> buffer{};
        std::string lineAccum;
        DWORD bytesRead = 0;
        while (!m_cancelRequested.load() &&
               ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &bytesRead, nullptr) &&
               bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            lineAccum += buffer.data();
            size_t pos;
            while ((pos = lineAccum.find('\n')) != std::string::npos)
            {
                std::string line = lineAccum.substr(0, pos);
                lineAccum.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                ParseLine(line);
            }
        }

        if (!lineAccum.empty())
            ParseLine(lineAccum);

        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);

        {
            std::lock_guard lock(m_statusMutex);
            m_processHandle = nullptr;
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(readPipe);

        return static_cast<int>(exitCode);
#else
        int pipefd[2];
        if (pipe(pipefd) != 0)
        {
            PushLog(BuildLogLine::Level::Error, "Failed to create subprocess pipe");
            return -1;
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            close(pipefd[0]);
            close(pipefd[1]);
            PushLog(BuildLogLine::Level::Error, "Failed to fork subprocess");
            return -1;
        }

        if (pid == 0)
        {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            std::vector<char*> argv;
            argv.reserve(arguments.size() + 2);
            argv.push_back(const_cast<char*>(executable.c_str()));
            for (const auto& argument : arguments)
                argv.push_back(const_cast<char*>(argument.c_str()));
            argv.push_back(nullptr);

            execvp(executable.c_str(), argv.data());
            _exit(127);
        }

        close(pipefd[1]);
        m_childPid = pid;

        std::array<char, 512> buffer{};
        std::string lineAccum;

        while (!m_cancelRequested.load())
        {
            ssize_t bytesRead = read(pipefd[0], buffer.data(), buffer.size() - 1);
            if (bytesRead <= 0)
                break;

            buffer[bytesRead] = '\0';
            lineAccum += buffer.data();
            // Flush complete lines
            size_t pos;
            while ((pos = lineAccum.find('\n')) != std::string::npos)
            {
                std::string line = lineAccum.substr(0, pos);
                lineAccum.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                ParseLine(line);
            }
        }

        // Flush remainder
        if (!lineAccum.empty())
            ParseLine(lineAccum);

        close(pipefd[0]);

        int status = 0;
        waitpid(pid, &status, 0);
        m_childPid = 0;

        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

    // ========================================================================
    // Output parsing
    // ========================================================================

    void BuildPipeline::ParseLine(const std::string& line)
    {
        // Detect CMake build progress: [  X%] or [ X/Y]
        static const std::regex progressPct(R"(\[\s*(\d+)%\])");
        static const std::regex progressFrac(R"(\[\s*(\d+)/(\d+)\])");

        std::smatch match;
        if (std::regex_search(line, match, progressPct))
        {
            float pct = std::stof(match[1].str()) / 100.0f;
            // Map compile progress to [0.15, 0.95] range (configure takes 0-0.15)
            m_progress.store(0.15f + pct * 0.80f);
        }
        else if (std::regex_search(line, match, progressFrac))
        {
            float current = std::stof(match[1].str());
            float total = std::stof(match[2].str());
            if (total > 0.0f)
            {
                float pct = current / total;
                m_progress.store(0.15f + pct * 0.80f);

                std::lock_guard lk(m_statusMutex);
                m_statusText = "Compiling [" + match[1].str() + "/" + match[2].str() + "]";
            }
        }

        // Classify severity
        BuildLogLine::Level level = BuildLogLine::Level::Info;
        if (line.find("error") != std::string::npos || line.find("Error") != std::string::npos ||
            line.find("FAILED") != std::string::npos)
        {
            level = BuildLogLine::Level::Error;
        }
        else if (line.find("warning") != std::string::npos || line.find("Warning") != std::string::npos)
        {
            level = BuildLogLine::Level::Warning;
        }

        PushLog(level, line);
    }

    void BuildPipeline::PushLog(BuildLogLine::Level level, std::string text)
    {
        std::lock_guard lk(m_logMutex);
        m_logQueue.push_back({level, std::move(text)});
    }

    // ========================================================================
    // Settings mapping
    // ========================================================================

    std::string BuildPipeline::SettingsToPreset(const BuildSettings& settings)
    {
        // Map platform + profile to a CMake preset name
        switch (settings.platform)
        {
        case BuildCookPanel::TargetPlatform::LinuxX64:
            return (settings.profile == BuildCookPanel::BuildProfile::Debug ||
                    settings.profile == BuildCookPanel::BuildProfile::Development)
                       ? "linux-gcc-debug"
                       : "linux-gcc-release";
        case BuildCookPanel::TargetPlatform::MacOSX64:
        case BuildCookPanel::TargetPlatform::MacOSARM64:
            return (settings.profile == BuildCookPanel::BuildProfile::Debug) ? "macos-debug" : "macos-release";
        default: // Windows
            return (settings.profile == BuildCookPanel::BuildProfile::Debug ||
                    settings.profile == BuildCookPanel::BuildProfile::Development)
                       ? "windows-debug"
                       : "windows-release";
        }
    }

    std::string BuildPipeline::SettingsToBuildType(const BuildSettings& settings)
    {
        switch (settings.profile)
        {
        case BuildCookPanel::BuildProfile::Debug:
            return "Debug";
        case BuildCookPanel::BuildProfile::Development:
            return "RelWithDebInfo";
        case BuildCookPanel::BuildProfile::Release:
        case BuildCookPanel::BuildProfile::Shipping:
            return "Release";
        default:
            return "Release";
        }
    }

    std::vector<std::string> BuildPipeline::SettingsToDefines(const BuildSettings& settings)
    {
        std::vector<std::string> defs;

        defs.push_back("BUILD_TESTS=" + std::string(settings.includeDevCommands ? "ON" : "OFF"));
        defs.push_back("ENABLE_EDITOR=" + std::string(settings.includeEditor ? "ON" : "OFF"));

        if (settings.enableLTO)
            defs.push_back("CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON");

        if (!settings.includeConsole)
            defs.push_back("ENABLE_CONSOLE=OFF");

        return defs;
    }

} // namespace SparkEditor
