/**
 * @file RuntimePackage.h
 * @brief Packaged-runtime root discovery shared by every platform entry point.
 */
#pragma once

#include <filesystem>
#include <system_error>

namespace Spark::RuntimePackage
{
    enum class WorkingDirectoryResult
    {
        NotPackaged,
        AlreadyAnchored,
        Anchored,
        Failed,
    };

    /** @brief Return the directory containing the running executable. */
    std::filesystem::path GetExecutableDirectory();

    /**
     * @brief Anchor a spark-cli package to its executable directory.
     *
     * A directory is treated as a package only when both manifest.json and
     * spark.modules.json are regular files. Development builds and explicit
     * command-line launch roots therefore retain the caller's working directory.
     */
    WorkingDirectoryResult AnchorWorkingDirectory(const std::filesystem::path& executableDirectory,
                                                  std::error_code& error);
} // namespace Spark::RuntimePackage
