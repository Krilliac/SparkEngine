/**
 * @file BuildPipeline.cpp
 * @brief Async CMake subprocess management for build orchestration
 */

#include "BuildPipeline.h"

#include <array>
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

        std::ostringstream configCmd;
        configCmd << "cmake --preset " << cmakePreset;
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
            configCmd << " -D" << def;
        }
        configCmd << " 2>&1";

        PushLog(BuildLogLine::Level::Info, "> " + configCmd.str());
        int configExit = RunCommand(configCmd.str());

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

        std::ostringstream buildCmd;
        buildCmd << "cmake --build " << buildDir << " --config " << buildType << " 2>&1";

        PushLog(BuildLogLine::Level::Info, "> " + buildCmd.str());
        int buildExit = RunCommand(buildCmd.str());

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

    int BuildPipeline::RunCommand(const std::string& command)
    {
#ifdef _WIN32
        // Use _popen on Windows
        FILE* pipe = _popen(command.c_str(), "r");
#else
        FILE* pipe = popen(command.c_str(), "r");
#endif
        if (!pipe)
        {
            PushLog(BuildLogLine::Level::Error, "Failed to launch subprocess");
            return -1;
        }

        std::array<char, 512> buffer{};
        std::string lineAccum;

        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        {
            if (m_cancelRequested.load())
                break;

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

#ifdef _WIN32
        int exitCode = _pclose(pipe);
#else
        int status = pclose(pipe);
        int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
        return exitCode;
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
