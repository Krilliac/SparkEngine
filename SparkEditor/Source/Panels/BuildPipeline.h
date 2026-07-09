/**
 * @file BuildPipeline.h
 * @brief Async build orchestrator that invokes CMake as a subprocess
 *
 * Replaces the simulated build progress in BuildCookPanel with real
 * CMake configuration and compilation, running in a background thread
 * with thread-safe log and progress reporting.
 */

#pragma once

#include "BuildCookPanel.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace SparkEditor
{

    using BuildSettings = BuildCookPanel::BuildSettings;

    /// @brief Result of a completed build.
    enum class BuildResult
    {
        None,     ///< No build has run yet.
        Success,  ///< Build completed without errors.
        Failed,   ///< Build finished with errors.
        Cancelled ///< Build was cancelled by the user.
    };

    /// @brief A single line of build output with severity.
    struct BuildLogLine
    {
        enum class Level
        {
            Info,
            Warning,
            Error
        };
        Level level = Level::Info;
        std::string text;
    };

    /**
     * @brief Manages async CMake subprocess for building the project.
     *
     * Thread model: `StartBuild()` spawns a detached worker that writes to
     * atomic/mutex-protected state.  The UI thread polls `GetProgress()`,
     * `DrainLogLines()`, and `GetResult()` each frame.
     */
    class BuildPipeline
    {
      public:
        BuildPipeline();
        ~BuildPipeline();

        // Non-copyable, non-movable (owns thread).
        BuildPipeline(const BuildPipeline&) = delete;
        BuildPipeline& operator=(const BuildPipeline&) = delete;

        /// @brief Launch a full configure + build.
        /// @param settings  Build settings from the BuildCookPanel UI.
        /// @param projectRoot  Absolute path to the project root (contains CMakeLists.txt).
        /// @return false if a build is already running.
        bool StartBuild(const BuildSettings& settings, const std::string& projectRoot);

        /// @brief Launch an asset-cook-only pass (copies + placeholder transforms).
        bool StartCookOnly(const BuildSettings& settings, const std::string& projectRoot);

        /// @brief Request cancellation of the running build.
        void Cancel();

        /// @brief True while a build subprocess is active.
        bool IsRunning() const { return m_running.load(std::memory_order_relaxed); }

        /// @brief Normalised [0,1] progress estimate.
        float GetProgress() const { return m_progress.load(std::memory_order_relaxed); }

        /// @brief Drain accumulated log lines (returns and clears the internal queue).
        std::vector<BuildLogLine> DrainLogLines();

        /// @brief Result of the last completed build.
        BuildResult GetResult() const { return m_result.load(std::memory_order_relaxed); }

        /// @brief Human-readable status phrase ("Configuring...", "Compiling [12/48]", etc.).
        std::string GetStatusText() const;

      private:
        /// Worker entry point — runs CMake configure then build.
        void WorkerThread(std::string cmakePreset, std::string buildType, std::string buildDir, std::string projectRoot,
                          std::vector<std::string> extraDefines);

        /// Worker for cook-only (copies assets to output directory).
        void CookWorkerThread(std::string outputDir, std::string projectRoot);

        /// Execute a subprocess, read stdout/stderr line-by-line, feed to ParseLine.
        int RunCommand(const std::string& executable, const std::vector<std::string>& arguments);

        /// Parse a single output line from CMake/compiler, updating progress and log.
        void ParseLine(const std::string& line);

        /// Append a log line (thread-safe).
        void PushLog(BuildLogLine::Level level, std::string text);

        /// Map BuildSettings to a CMake preset name.
        static std::string SettingsToPreset(const BuildSettings& settings);

        /// Map BuildSettings to CMake -D flags.
        static std::vector<std::string> SettingsToDefines(const BuildSettings& settings);

        /// Map BuildSettings to CMAKE_BUILD_TYPE.
        static std::string SettingsToBuildType(const BuildSettings& settings);

        // --- shared state (UI thread reads, worker writes) ---
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_cancelRequested{false};
        std::atomic<float> m_progress{0.0f};
        std::atomic<BuildResult> m_result{BuildResult::None};

        mutable std::mutex m_logMutex;
        std::vector<BuildLogLine> m_logQueue;

        mutable std::mutex m_statusMutex;
        std::string m_statusText = "Idle";

        // Background thread (joined in destructor).
        std::thread m_worker;

#ifdef _WIN32
        void* m_processHandle = nullptr; ///< HANDLE for TerminateProcess on cancel.
#else
        pid_t m_childPid = 0; ///< PID for kill() on cancel.
#endif
    };

} // namespace SparkEditor
