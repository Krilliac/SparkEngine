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

        /// @brief Launch an asset-cook-only pass through SparkCooker.
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

        /// @brief Whether the current/last operation is an asset cook.
        bool IsCookOperation() const { return m_cooking.load(std::memory_order_relaxed); }

        /// @brief Locate the shipped SparkCooker executable using bounded paths.
        static std::string FindSparkCookerExecutable();

        /// @brief Locate the shipped SparkAutomation executable using bounded paths.
        static std::string FindSparkAutomationExecutable();

        /// @brief Construct SparkCooker's deterministic argument vector.
        static std::vector<std::string> CreateCookArguments(const std::string& sourceRoot,
                                                            const std::string& outputRoot,
                                                            const std::string& manifestPath, bool dryRun = false);

        /// @brief Construct a frame-bounded SparkAutomation runtime smoke-test plan.
        static std::vector<std::string> CreateAutomationArguments(const std::string& executable,
                                                                  const std::string& workingDirectory,
                                                                  const std::string& reportDirectory, int frames,
                                                                  int timeoutSeconds);

        /// @brief Validate an installed SparkEngine CMake package directory.
        /// Build-tree-only configs are rejected because they do not contain
        /// the exported targets and module helper required by game projects.
        static bool IsSparkEnginePackageDirectory(const std::string& directory);

        /// @brief Locate the installed SparkEngine CMake package used by builds.
        /// Checks explicit environment configuration first, then bounded paths
        /// relative to the running editor executable. Returns empty on failure.
        static std::string FindSparkEnginePackageDirectory(const std::string& configuration = {});

        /// @brief Construct the explicit project configure command arguments.
        /// Exposed for deterministic regression testing and build diagnostics.
        static std::vector<std::string> CreateConfigureArguments(const BuildSettings& settings,
                                                                 const std::string& projectRoot,
                                                                 const std::string& buildDir,
                                                                 const std::string& sparkEngineDirectory,
                                                                 const std::vector<std::string>& extraDefines);

        /// @brief Return the only target that this editor host can package and execute natively.
        static BuildCookPanel::TargetPlatform NativeTargetPlatform();
        static bool IsNativeTargetPlatform(BuildCookPanel::TargetPlatform platform);

        /// @brief Assemble a runnable native package from already-built artifacts.
        ///
        /// The package contains two deliberately separate launch modes: the
        /// renamed host plus module manifest for game-module execution, and an
        /// isolated scene-preview host with no manifest so `-scene` is honored.
        /// Exposed for focused filesystem regression tests.
        static bool AssembleNativePackage(const BuildSettings& settings, const std::string& projectRoot,
                                          const std::string& runtimeHost, const std::string& moduleBinary,
                                          const std::string& outputDirectory, std::string* error = nullptr,
                                          const std::string& dedicatedServerHost = {});

      private:
        /// Populate an already-isolated staging directory. The public wrapper
        /// owns activation/rollback so callers never observe a partial package.
        static bool AssembleNativePackageContents(const BuildSettings& settings, const std::string& projectRoot,
                                                  const std::string& runtimeHost, const std::string& moduleBinary,
                                                  const std::string& stagingDirectory, std::string* error,
                                                  const std::string& dedicatedServerHost);

        /// Worker entry point — runs CMake configure then build.
        void WorkerThread(BuildSettings settings, std::string buildType, std::string buildDir, std::string projectRoot,
                          std::vector<std::string> extraDefines);

        /// Worker for cook-only (copies assets to output directory).
        void CookWorkerThread(BuildSettings settings, std::string outputDir, std::string projectRoot);

        /// Execute a subprocess, read stdout/stderr line-by-line, feed to ParseLine.
        int RunCommand(const std::string& executable, const std::vector<std::string>& arguments);

        /// Parse a single output line from CMake/compiler, updating progress and log.
        void ParseLine(const std::string& line);

        /// Append a log line (thread-safe).
        void PushLog(BuildLogLine::Level level, std::string text);

        /// Map BuildSettings to CMake -D flags.
        static std::vector<std::string> SettingsToDefines(const BuildSettings& settings);

        /// Map BuildSettings to CMAKE_BUILD_TYPE.
        static std::string SettingsToBuildType(const BuildSettings& settings);

        // --- shared state (UI thread reads, worker writes) ---
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_cancelRequested{false};
        std::atomic<bool> m_cooking{false};
        std::atomic<float> m_progress{0.0f};
        std::atomic<BuildResult> m_result{BuildResult::None};

        mutable std::mutex m_logMutex;
        std::vector<BuildLogLine> m_logQueue;

        mutable std::mutex m_statusMutex;
        std::string m_statusText = "Idle";

        mutable std::mutex m_processMutex;

        // Background thread (joined in destructor).
        std::thread m_worker;

#ifdef _WIN32
        void* m_processHandle = nullptr; ///< HANDLE for the immediate child.
        void* m_jobHandle = nullptr;     ///< Kill-on-close Job Object for the process tree.
#else
        pid_t m_childPid = 0; ///< Process-group leader PID for cancellation.
#endif
    };

} // namespace SparkEditor
