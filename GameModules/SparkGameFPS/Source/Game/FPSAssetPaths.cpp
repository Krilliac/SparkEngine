/**
 * @file FPSAssetPaths.cpp
 * @brief Asset-root discovery for the SparkGameFPS module.
 */

#include "FPSAssetPaths.h"

#include <algorithm>
#include <string_view>

// Executable discovery needs the native API, not private engine platform types.
#ifdef _WIN32
#include <windows.h>
#endif // _WIN32

namespace Spark
{
    namespace FPSAssets
    {
        namespace
        {
            std::filesystem::path ExecutableDirectory()
            {
#ifdef _WIN32
                wchar_t buffer[MAX_PATH] = {};
                const DWORD length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(MAX_PATH));
                if (length == 0 || length >= static_cast<DWORD>(MAX_PATH))
                    return {};
                return std::filesystem::path(buffer, buffer + length).parent_path();
#else
                std::error_code error;
                const std::filesystem::path exePath = std::filesystem::read_symlink("/proc/self/exe", error);
                if (error)
                    return {};
                return exePath.parent_path();
#endif // _WIN32
            }
        } // namespace

        std::filesystem::path FindAssetRoot(const std::vector<std::filesystem::path>& searchBases)
        {
            std::error_code error;
            for (const auto& base : searchBases)
            {
                if (base.empty())
                    continue;
                const std::filesystem::path candidate = base / "Assets";
                if (std::filesystem::is_directory(candidate / "Models", error))
                    return candidate;
            }
            return {};
        }

        std::vector<std::filesystem::path> DefaultSearchBases()
        {
            std::vector<std::filesystem::path> bases;
            const std::filesystem::path exeDir = ExecutableDirectory();
            if (!exeDir.empty())
            {
                bases.push_back(exeDir);
                bases.push_back(exeDir.parent_path());
            }

            std::error_code error;
            const std::filesystem::path workingDir = std::filesystem::current_path(error);
            if (!error)
            {
                bases.push_back(workingDir);
                bases.push_back(workingDir.parent_path());
                bases.push_back(workingDir.parent_path().parent_path());
            }
            return bases;
        }

        const std::filesystem::path& Root()
        {
            static const std::filesystem::path resolved = []
            {
                std::filesystem::path found = FindAssetRoot(DefaultSearchBases());
                if (!found.empty())
                    return found;

                std::error_code error;
                const std::filesystem::path workingDir = std::filesystem::current_path(error);
                return error ? std::filesystem::path("Assets") : (workingDir / "Assets");
            }();
            return resolved;
        }

        bool RootExists()
        {
            std::error_code error;
            return std::filesystem::is_directory(Root(), error);
        }

        std::wstring Resolve(const std::wstring& relativeToAssetRoot)
        {
            std::filesystem::path full = Root() / relativeToAssetRoot;
            full.make_preferred();
            return full.wstring();
        }

        std::string ResolveUtf8(const std::string& relativeToAssetRoot)
        {
            // Both ends of this function are UTF-8. std::filesystem::path's narrow
            // conversions go through the active code page on Windows, so building the
            // path from a plain std::string and returning path::string() would mangle
            // (or throw on) any install or user-profile directory outside that code
            // page - exactly the case the wide Resolve() overload exists to handle.
            const std::filesystem::path relative(
                reinterpret_cast<const char8_t*>(relativeToAssetRoot.c_str()),
                reinterpret_cast<const char8_t*>(relativeToAssetRoot.c_str() + relativeToAssetRoot.size()));
            std::filesystem::path full = Root() / relative;
            full.make_preferred();

            const std::u8string utf8 = full.u8string();
            return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        }

        bool ResolveScenePath(const std::string& userPath, std::filesystem::path& resolved, std::string& error)
        {
            resolved.clear();
            error.clear();
            if (userPath.empty())
            {
                error = "scene path is empty";
                return false;
            }
            if (userPath.size() > 4096 || userPath.find('\0') != std::string::npos)
            {
                error = "scene path is malformed or too long";
                return false;
            }

            // Normalize separators before parsing. This also makes traversal
            // checks identical on Windows and POSIX package smoke runners.
            std::string normalized = userPath;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            const std::u8string normalizedU8(reinterpret_cast<const char8_t*>(normalized.data()), normalized.size());
            const std::filesystem::path supplied = std::filesystem::u8path(normalizedU8);
            if (supplied.is_absolute() || !supplied.root_name().empty() || normalized.front() == '/')
            {
                error = "absolute scene paths are not permitted";
                return false;
            }

            constexpr std::string_view kAssetsScenes = "Assets/Scenes/";
            constexpr std::string_view kScenes = "Scenes/";
            if (normalized.starts_with(kAssetsScenes))
                normalized.erase(0, kAssetsScenes.size());
            else if (normalized.starts_with(kScenes))
                normalized.erase(0, kScenes.size());

            const std::u8string relativeU8(reinterpret_cast<const char8_t*>(normalized.data()), normalized.size());
            const std::filesystem::path relative = std::filesystem::u8path(relativeU8);
            if (normalized.empty() || relative.has_root_path() || relative.extension() != ".scene")
            {
                error = "scene must be a relative .scene file";
                return false;
            }
            for (const auto& component : relative)
            {
                if (component == ".." || component == "." || component.empty())
                {
                    error = "scene traversal or empty path component is not permitted";
                    return false;
                }
            }

            std::error_code ec;
            const auto sceneRoot = std::filesystem::weakly_canonical(Root() / "Scenes", ec);
            if (ec || !std::filesystem::is_directory(sceneRoot, ec))
            {
                error = "trusted scene directory is unavailable";
                return false;
            }
            const auto candidate = std::filesystem::weakly_canonical(sceneRoot / relative, ec);
            if (ec || !std::filesystem::is_regular_file(candidate, ec))
            {
                error = "scene file does not exist";
                return false;
            }
            const auto relativeCandidate = candidate.lexically_relative(sceneRoot);
            if (relativeCandidate.empty() || relativeCandidate.is_absolute() ||
                relativeCandidate.begin()->string() == "..")
            {
                error = "scene resolves outside the trusted scene directory";
                return false;
            }
            resolved = candidate;
            return true;
        }
    } // namespace FPSAssets
} // namespace Spark
