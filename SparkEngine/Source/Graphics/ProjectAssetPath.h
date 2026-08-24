/**
 * @file ProjectAssetPath.h
 * @brief Canonical, project-confined asset path resolution for render-time loads.
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Spark
{
    /** @brief Native path plus its canonical UTF-8 cache identity. */
    struct ResolvedProjectAssetPath
    {
        std::filesystem::path nativePath;
        std::string cacheKey;
    };

    /**
     * @brief Canonicalize a UTF-8 filesystem path without requiring its leaf to exist.
     *
     * Existing parents and links are resolved by std::filesystem::weakly_canonical.
     * This lower-level helper does not impose project containment; render-facing
     * callers should normally use ResolveProjectAssetPath instead.
     */
    std::optional<ResolvedProjectAssetPath> CanonicalizeFilesystemPath(std::string_view utf8Path);

    /**
     * @brief Resolve an Assets/... UTF-8 path beneath an explicit project root.
     *
     * Absolute/rooted paths and every `..` component are rejected. Existing
     * symlinks/reparse points that exist at check time are followed before a
     * component-wise containment check. The asset leaf may be absent, allowing
     * a later retry after import or file creation. Callers that open paths from
     * untrusted writers must still account for the usual path-check/open race.
     */
    std::optional<ResolvedProjectAssetPath> ResolveProjectAssetPath(std::string_view projectRootUtf8,
                                                                    std::string_view assetPathUtf8);

    /**
     * @brief Derive a canonical project root from an existing loaded scene path.
     *
     * The canonical scene must be inside a directory named `Scenes`, and that
     * directory's parent must contain an `Assets` directory. No current-working-
     * directory or executable-location fallback is used.
     */
    std::optional<std::string> DeriveProjectRootFromScenePath(std::string_view scenePathUtf8);

} // namespace Spark
