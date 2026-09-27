/**
 * @file LauncherProcess.h
 * @brief Validated launch requests for SparkLauncher project actions.
 */

#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace SparkLauncher
{
    enum class LaunchTarget
    {
        Editor,
        Game,
        DedicatedServer,
        ServiceTopology
    };

    /**
     * @brief Largest spark.modules.json the Game target will read (1 MiB).
     *
     * A real manifest lists a handful of modules and is a few hundred bytes; the
     * cap keeps a hostile project's manifest from being read into memory whole.
     */
    inline constexpr std::size_t kMaxModuleManifestBytes = std::size_t{1024} * 1024;

    struct LaunchRequest
    {
        std::filesystem::path executable;
        std::filesystem::path workingDirectory;
        std::vector<std::string> arguments;
    };

    /** Build and validate the executable, project, and target-specific inputs. */
    [[nodiscard]] std::expected<LaunchRequest, std::string> BuildLaunchRequest(
        const std::filesystem::path& binaryDirectory, const std::filesystem::path& projectFile, LaunchTarget target);

    /** Start an independent child and return after the operating system accepts it. */
    [[nodiscard]] std::expected<void, std::string> LaunchDetached(const LaunchRequest& request);

    [[nodiscard]] const char* LaunchTargetName(LaunchTarget target);
} // namespace SparkLauncher
