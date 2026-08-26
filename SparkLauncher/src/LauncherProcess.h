/**
 * @file LauncherProcess.h
 * @brief Validated launch requests for SparkLauncher project actions.
 */

#pragma once

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
