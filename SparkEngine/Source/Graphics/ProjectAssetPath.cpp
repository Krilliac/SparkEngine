/**
 * @file ProjectAssetPath.cpp
 * @brief Canonical, project-confined asset path resolution implementation.
 */

#include "Graphics/ProjectAssetPath.h"

#include "Core/Platform.h"

#include <climits>
#include <limits>
#include <system_error>
#include <utility>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Spark
{
    namespace
    {
        namespace fs = std::filesystem;

        std::optional<fs::path> PathFromUtf8(std::string_view value)
        {
            if (value.empty())
                return fs::path{};

#ifdef SPARK_PLATFORM_WINDOWS
            if (value.size() > static_cast<size_t>(INT_MAX))
                return std::nullopt;
            const int inputLength = static_cast<int>(value.size());
            const int wideLength =
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength, nullptr, 0);
            if (wideLength <= 0)
                return std::nullopt;

            std::wstring wide(static_cast<size_t>(wideLength), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength, wide.data(),
                                    wideLength) != wideLength)
            {
                return std::nullopt;
            }
            return fs::path(std::move(wide));
#else
            return fs::path(std::string(value));
#endif
        }

        std::optional<std::string> PathToUtf8(const fs::path& path)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            const std::wstring& wide = path.native();
            if (wide.empty())
                return std::string{};
            if (wide.size() > static_cast<size_t>(INT_MAX))
                return std::nullopt;

            const int inputLength = static_cast<int>(wide.size());
            const int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), inputLength, nullptr,
                                                       0, nullptr, nullptr);
            if (utf8Length <= 0)
                return std::nullopt;

            std::string utf8(static_cast<size_t>(utf8Length), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), inputLength, utf8.data(), utf8Length,
                                    nullptr, nullptr) != utf8Length)
            {
                return std::nullopt;
            }
            return utf8;
#else
            return path.generic_string();
#endif
        }

        std::optional<std::string> CacheKeyFromPath(const fs::path& path)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            std::wstring folded = path.native();
            if (folded.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max()))
                return std::nullopt;
            if (!folded.empty() &&
                CharLowerBuffW(folded.data(), static_cast<DWORD>(folded.size())) != static_cast<DWORD>(folded.size()))
                return std::nullopt;
            return PathToUtf8(fs::path(std::move(folded)));
#else
            return PathToUtf8(path);
#endif
        }

        bool ComponentsEqual(const fs::path& lhs, const fs::path& rhs)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            return _wcsicmp(lhs.native().c_str(), rhs.native().c_str()) == 0;
#else
            return lhs == rhs;
#endif
        }

        bool IsContainedBy(const fs::path& root, const fs::path& candidate)
        {
            auto rootIt = root.begin();
            auto candidateIt = candidate.begin();
            for (; rootIt != root.end(); ++rootIt, ++candidateIt)
            {
                if (candidateIt == candidate.end() || !ComponentsEqual(*rootIt, *candidateIt))
                    return false;
            }
            return true;
        }

        bool HasParentTraversal(const fs::path& relativePath)
        {
            for (const fs::path& component : relativePath)
            {
                if (component == "..")
                    return true;
            }
            return false;
        }
    } // namespace

    std::optional<ResolvedProjectAssetPath> CanonicalizeFilesystemPath(std::string_view utf8Path)
    {
        const auto input = PathFromUtf8(utf8Path);
        if (!input || input->empty())
            return std::nullopt;

        std::error_code ec;
        const fs::path absolute = fs::absolute(*input, ec);
        if (ec)
            return std::nullopt;

        const fs::path canonical = fs::weakly_canonical(absolute, ec);
        if (ec || canonical.empty())
            return std::nullopt;

        auto cacheKey = CacheKeyFromPath(canonical);
        if (!cacheKey || cacheKey->empty())
            return std::nullopt;
        return ResolvedProjectAssetPath{canonical, std::move(*cacheKey)};
    }

    std::optional<ResolvedProjectAssetPath> ResolveProjectAssetPath(std::string_view projectRootUtf8,
                                                                    std::string_view assetPathUtf8)
    {
        const auto rootInput = PathFromUtf8(projectRootUtf8);
        const auto assetInput = PathFromUtf8(assetPathUtf8);
        if (!rootInput || rootInput->empty() || !assetInput || assetInput->empty())
            return std::nullopt;
        if (assetInput->is_absolute() || assetInput->has_root_name() || assetInput->has_root_directory() ||
            HasParentTraversal(*assetInput))
        {
            return std::nullopt;
        }

        const auto first = assetInput->begin();
        if (first == assetInput->end() || !ComponentsEqual(*first, fs::path("Assets")))
            return std::nullopt;

        std::error_code ec;
        const fs::path canonicalRoot = fs::canonical(*rootInput, ec);
        if (ec || canonicalRoot.empty() || !fs::is_directory(canonicalRoot, ec) || ec)
            return std::nullopt;

        const fs::path candidate = fs::weakly_canonical(canonicalRoot / *assetInput, ec);
        if (ec || candidate.empty() || !IsContainedBy(canonicalRoot, candidate))
            return std::nullopt;

        auto cacheKey = CacheKeyFromPath(candidate);
        if (!cacheKey || cacheKey->empty())
            return std::nullopt;
        return ResolvedProjectAssetPath{candidate, std::move(*cacheKey)};
    }

    std::optional<std::string> DeriveProjectRootFromScenePath(std::string_view scenePathUtf8)
    {
        const auto scene = CanonicalizeFilesystemPath(scenePathUtf8);
        if (!scene)
            return std::nullopt;

        std::error_code ec;
        if (!fs::is_regular_file(scene->nativePath, ec) || ec)
            return std::nullopt;

        fs::path current = scene->nativePath.parent_path();
        while (!current.empty())
        {
            if (ComponentsEqual(current.filename(), fs::path("Scenes")))
            {
                const fs::path root = current.parent_path();
                if (!root.empty() && IsContainedBy(root, scene->nativePath))
                {
                    ec.clear();
                    if (fs::is_directory(root / "Assets", ec) && !ec)
                        return PathToUtf8(root);
                }
            }

            const fs::path parent = current.parent_path();
            if (parent == current)
                break;
            current = parent;
        }
        return std::nullopt;
    }

} // namespace Spark
