/**
 * @file EditorPluginDiscovery.h
 * @brief Safe, bounded discovery policy for project-owned editor plugins.
 */

#pragma once

#include "Utils/EditorLaunchContext.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SparkEditor::PluginDiscovery
{
    inline std::string NativeLibraryExtension()
    {
#if defined(_WIN32)
        return ".dll";
#elif defined(__APPLE__)
        return ".dylib";
#else
        return ".so";
#endif
    }

    inline std::filesystem::path ProjectRoot(const std::string& projectPath)
    {
        const auto requested = LaunchContext::PathFromUtf8(projectPath);
        return LaunchContext::CanonicalPath(requested);
    }

    /**
     * @brief Resolve and enumerate native plugins from one explicit project-owned directory.
     *
     * Discovery is deliberately non-recursive. The requested directory must resolve inside
     * the project root, may not itself be a symlink, and candidates must be regular non-symlink
     * native libraries with regular sibling .sparkplugin.json metadata files.
     */
    inline bool Discover(const std::string& projectPath, std::string_view requestedDirectory,
                         std::vector<std::filesystem::path>& plugins, std::string& error)
    {
        plugins.clear();
        error.clear();

        if (requestedDirectory.empty())
        {
            error = "editor plugin directory is empty";
            return false;
        }

        if (projectPath.empty())
        {
            error = "editor plugin discovery requires an active project root";
            return false;
        }

        const std::filesystem::path projectRoot = ProjectRoot(projectPath);
        if (!LaunchContext::IsDirectory(projectRoot))
        {
            error =
                "editor plugin project root is not an existing directory: " + LaunchContext::PathToUtf8(projectRoot);
            return false;
        }

        const std::filesystem::path requested = LaunchContext::PathFromUtf8(requestedDirectory);
        const std::filesystem::path unresolvedCandidate = requested.is_absolute() ? requested : projectRoot / requested;
        std::error_code ec;
        const std::filesystem::file_status unresolvedStatus = std::filesystem::symlink_status(unresolvedCandidate, ec);
        if (!ec && std::filesystem::is_symlink(unresolvedStatus))
        {
            error = "editor plugin directory may not be a symlink: " + LaunchContext::PathToUtf8(unresolvedCandidate);
            return false;
        }

        const std::filesystem::path candidate = LaunchContext::CanonicalPath(unresolvedCandidate);
        if (!LaunchContext::IsPathWithin(candidate, projectRoot))
        {
            error =
                "editor plugin directory must remain inside the project root: " + LaunchContext::PathToUtf8(candidate);
            return false;
        }

        ec.clear();
        const std::filesystem::file_status directoryStatus = std::filesystem::symlink_status(candidate, ec);
        if (ec || std::filesystem::is_symlink(directoryStatus) || !std::filesystem::is_directory(directoryStatus))
        {
            error = "editor plugin directory is missing, not a directory, or a symlink: " +
                    LaunchContext::PathToUtf8(candidate);
            return false;
        }

        const std::string nativeExtension = NativeLibraryExtension();
        std::filesystem::directory_iterator iterator(candidate,
                                                     std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::directory_iterator end;
        while (!ec && iterator != end)
        {
            const std::filesystem::directory_entry& entry = *iterator;
            std::error_code statusError;
            const std::filesystem::file_status status = entry.symlink_status(statusError);
            if (!statusError && !std::filesystem::is_symlink(status) && std::filesystem::is_regular_file(status))
            {
                std::string extension = LaunchContext::PathToUtf8(entry.path().extension());
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (extension == nativeExtension)
                {
                    std::filesystem::path metadata = entry.path();
                    metadata += ".sparkplugin.json";
                    const std::filesystem::file_status metadataStatus =
                        std::filesystem::symlink_status(metadata, statusError);
                    if (!statusError && !std::filesystem::is_symlink(metadataStatus) &&
                        std::filesystem::is_regular_file(metadataStatus))
                    {
                        plugins.push_back(LaunchContext::CanonicalPath(entry.path()));
                    }
                }
            }

            iterator.increment(ec);
        }

        if (ec)
        {
            plugins.clear();
            error = "failed to enumerate editor plugin directory: " + ec.message();
            return false;
        }

        std::sort(plugins.begin(), plugins.end(), [](const auto& lhs, const auto& rhs)
                  { return LaunchContext::PathKey(lhs) < LaunchContext::PathKey(rhs); });
        return true;
    }
} // namespace SparkEditor::PluginDiscovery
