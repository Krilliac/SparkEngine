/** @file LauncherPaths.h @brief SparkLauncher executable and template path discovery. */
#pragma once

#include <filesystem>

namespace SparkLauncher
{
    /** @brief Return the absolute path of the running SparkLauncher executable. */
    std::filesystem::path GetLauncherExecutablePath();

    /**
     * @brief Locate project templates for development and installed layouts.
     * @param executablePath Path to the launcher executable.
     * @param currentDirectory Process working directory used by development builds.
     */
    std::filesystem::path FindLauncherTemplatesDirectory(const std::filesystem::path& executablePath,
                                                         const std::filesystem::path& currentDirectory);
} // namespace SparkLauncher
