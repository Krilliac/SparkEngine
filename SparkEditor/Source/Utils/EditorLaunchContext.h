/**
 * @file EditorLaunchContext.h
 * @brief Side-effect-free path policy for editor module discovery and play launches.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SparkEditor
{
    namespace LaunchContext
    {
        inline std::filesystem::path PathFromUtf8(std::string_view path)
        {
            return std::filesystem::u8path(path.begin(), path.end());
        }

        inline std::string PathToUtf8(const std::filesystem::path& path)
        {
            const std::u8string utf8 = path.generic_u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

        inline std::filesystem::path CanonicalPath(const std::filesystem::path& path)
        {
            if (path.empty())
                return {};

            std::error_code ec;
            std::filesystem::path absolute = path;
            if (!absolute.is_absolute())
            {
                absolute = std::filesystem::absolute(absolute, ec);
                if (ec)
                    absolute = path;
            }

            ec.clear();
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
            return (ec ? absolute : canonical).lexically_normal();
        }

        inline std::string PathKey(const std::filesystem::path& path)
        {
            const std::u8string utf8 = CanonicalPath(path).generic_u8string();
            std::string key(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#ifdef _WIN32
            // Windows paths are case-insensitive. ASCII folding covers drive
            // letters and the normal build/config names used by this policy.
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
            return key;
        }

        inline bool SamePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
        {
            return !lhs.empty() && !rhs.empty() && PathKey(lhs) == PathKey(rhs);
        }

        inline bool IsPathWithin(const std::filesystem::path& candidate, const std::filesystem::path& directory)
        {
            std::string candidateKey = PathKey(candidate);
            std::string directoryKey = PathKey(directory);
            if (candidateKey.empty() || directoryKey.empty())
                return false;
            if (candidateKey == directoryKey)
                return true;
            if (directoryKey.back() != '/')
                directoryKey.push_back('/');
            return candidateKey.starts_with(directoryKey);
        }

        inline void AppendUniquePath(std::vector<std::filesystem::path>& paths, std::unordered_set<std::string>& seen,
                                     const std::filesystem::path& path)
        {
            if (path.empty())
                return;
            const std::filesystem::path canonical = CanonicalPath(path);
            if (seen.insert(PathKey(canonical)).second)
                paths.push_back(canonical);
        }

        /**
         * @brief Ordered, bounded directories that may contain an active project's module.
         *
         * The editor directory remains first for engine-tree modules. Project
         * entries cover the exact BuildPipeline layouts for single-config and
         * Visual Studio multi-config builds plus the assembled package directory.
         * No recursive filesystem walk is performed.
         */
        inline std::vector<std::filesystem::path> ModuleDiscoveryDirectories(
            const std::filesystem::path& editorExecutableDirectory, const std::filesystem::path& activeProjectRoot)
        {
            std::vector<std::filesystem::path> directories;
            std::unordered_set<std::string> seen;
            AppendUniquePath(directories, seen, editorExecutableDirectory);

            if (activeProjectRoot.empty())
                return directories;

            const std::filesystem::path projectRoot = CanonicalPath(activeProjectRoot);
            const std::filesystem::path buildRoot = projectRoot / "build";
            AppendUniquePath(directories, seen, buildRoot);

            constexpr const char* configurations[] = {"Debug", "RelWithDebInfo", "Release", "MinSizeRel"};
            for (const char* configuration : configurations)
            {
                const std::filesystem::path configuredBuild = buildRoot / configuration;
                AppendUniquePath(directories, seen, configuredBuild);
                AppendUniquePath(directories, seen, configuredBuild / configuration);
            }

            AppendUniquePath(directories, seen, projectRoot / "Build" / "Output");
            return directories;
        }

        template <typename DiscoverFn>
        inline std::vector<std::filesystem::path> DiscoverUniqueModules(
            const std::vector<std::filesystem::path>& directories, DiscoverFn&& discover)
        {
            std::vector<std::filesystem::path> modules;
            std::unordered_set<std::string> seen;
            for (const std::filesystem::path& directory : directories)
            {
                for (const auto& candidate : discover(directory))
                {
                    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(candidate)>, std::string>)
                        AppendUniquePath(modules, seen, PathFromUtf8(candidate));
                    else
                        AppendUniquePath(modules, seen, std::filesystem::path(candidate));
                }
            }
            return modules;
        }

        /**
         * @brief Locate a selected module after a discovery refresh.
         *
         * Discovery can reorder candidates or remove a module that was deleted
         * after the prior scan. The caller deliberately clears its persisted
         * selection when this returns std::nullopt rather than retaining a
         * stale path that an out-of-process launch could later consume.
         */
        inline std::optional<size_t> FindSelectedModuleIndex(const std::vector<std::filesystem::path>& modules,
                                                             const std::filesystem::path& selectedModule)
        {
            if (selectedModule.empty())
                return std::nullopt;

            for (size_t index = 0; index < modules.size(); ++index)
            {
                if (SamePath(modules[index], selectedModule))
                    return index;
            }
            return std::nullopt;
        }

        inline bool IsDirectory(const std::filesystem::path& path)
        {
            if (path.empty())
                return false;
            std::error_code ec;
            return std::filesystem::is_directory(path, ec) && !ec;
        }

        /** @brief Validate the selected native game module immediately before launch. */
        inline std::string ValidateGameModuleForLaunch(const std::filesystem::path& modulePath)
        {
            if (modulePath.empty())
                return "No game module DLL is selected";

            std::error_code ec;
            if (!std::filesystem::is_regular_file(modulePath, ec) || ec)
                return "Selected game module DLL is missing or is not a regular file: " + PathToUtf8(modulePath);

            std::string extension = PathToUtf8(modulePath.extension());
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (extension != ".dll")
                return "Selected game module must be a DLL: " + PathToUtf8(modulePath);

            return {};
        }

        /**
         * @brief Resolve the cwd used by all out-of-process play surfaces.
         *
         * A self-contained Build/Output package owns its own launch context,
         * while ordinary project build modules use the project root for source
         * assets and configuration. Engine-tree modules must not inherit an
         * unrelated project's Assets or Config.
         */
        inline std::filesystem::path ResolveWorkingDirectory(const std::filesystem::path& activeProjectRoot,
                                                             const std::filesystem::path& modulePath,
                                                             const std::filesystem::path& engineExecutable)
        {
            if (IsDirectory(activeProjectRoot) && !modulePath.empty())
            {
                const std::filesystem::path projectRoot = CanonicalPath(activeProjectRoot);
                const std::filesystem::path packageRoot = projectRoot / "Build" / "Output";
                if (IsDirectory(packageRoot) && IsPathWithin(modulePath, packageRoot))
                    return CanonicalPath(packageRoot);
                if (IsPathWithin(modulePath, projectRoot))
                    return projectRoot;
            }

            const std::filesystem::path moduleDirectory = modulePath.parent_path();
            if (IsDirectory(moduleDirectory))
                return CanonicalPath(moduleDirectory);

            const std::filesystem::path engineDirectory = engineExecutable.parent_path();
            if (IsDirectory(engineDirectory))
                return CanonicalPath(engineDirectory);

            return {};
        }

        inline std::filesystem::path ResolveContextFile(const std::filesystem::path& activeProjectRoot,
                                                        const std::filesystem::path& fallbackDirectory,
                                                        const std::filesystem::path& filename)
        {
            if (IsDirectory(activeProjectRoot))
                return CanonicalPath(activeProjectRoot) / filename;
            if (IsDirectory(fallbackDirectory))
                return CanonicalPath(fallbackDirectory) / filename;
            return {};
        }

        inline std::string ManifestModuleReference(const std::filesystem::path& manifestDirectory,
                                                   const std::filesystem::path& modulePath)
        {
            const std::filesystem::path canonicalDirectory = CanonicalPath(manifestDirectory);
            const std::filesystem::path canonicalModule = CanonicalPath(modulePath);
            std::error_code ec;
            const std::filesystem::path relative = std::filesystem::relative(canonicalModule, canonicalDirectory, ec);
            if (!ec && !relative.empty() && *relative.begin() != "..")
                return PathToUtf8(relative);
            return PathToUtf8(canonicalModule);
        }
    } // namespace LaunchContext
} // namespace SparkEditor
