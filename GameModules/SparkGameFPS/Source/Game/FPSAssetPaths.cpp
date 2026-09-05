/**
 * @file FPSAssetPaths.cpp
 * @brief Asset-root discovery for the SparkGameFPS module.
 */

#include "FPSAssetPaths.h"

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS

namespace Spark
{
    namespace FPSAssets
    {
        namespace
        {
            std::filesystem::path ExecutableDirectory()
            {
#ifdef SPARK_PLATFORM_WINDOWS
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
#endif // SPARK_PLATFORM_WINDOWS
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
    } // namespace FPSAssets
} // namespace Spark
