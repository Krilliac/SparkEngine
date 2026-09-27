/**
 * @file RuntimePackage.h
 * @brief Packaged-runtime root discovery shared by every platform entry point.
 */
#pragma once

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

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

    /**
     * @brief Candidate locations of read-only content that ships beside the executable.
     *
     * Returns, in search order, `executableDirectory / relative` and then
     * `workingDirectory / relative`, lexically normalized, skipping an empty base
     * and a candidate equal to an earlier one. Staged content next to the
     * executable therefore wins over a copy under the working directory, and a
     * launch from another directory (a `-game` run from the repository root, a
     * shortcut) still finds it. Pure path arithmetic: nothing is checked on disk.
     *
     * @param relative            Content directory relative to a runtime root. An
     *                            empty, rooted or `..`-escaping path yields no candidates.
     * @param executableDirectory GetExecutableDirectory(), or empty when unknown
     * @param workingDirectory    std::filesystem::current_path(), or empty when unknown
     */
    inline std::vector<std::filesystem::path> ResolveContentRoots(const std::filesystem::path& relative,
                                                                  const std::filesystem::path& executableDirectory,
                                                                  const std::filesystem::path& workingDirectory)
    {
        std::vector<std::filesystem::path> roots;
        const std::filesystem::path normalized = relative.lexically_normal();
        if (relative.empty() || relative.has_root_path() || normalized.empty() ||
            std::find(normalized.begin(), normalized.end(), std::filesystem::path("..")) != normalized.end())
        {
            return roots;
        }

        for (const std::filesystem::path* base : {&executableDirectory, &workingDirectory})
        {
            if (base->empty())
                continue;
            std::filesystem::path candidate = (*base / normalized).lexically_normal();
            if (std::find(roots.begin(), roots.end(), candidate) == roots.end())
                roots.push_back(std::move(candidate));
        }
        return roots;
    }
} // namespace Spark::RuntimePackage
