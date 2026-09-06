/**
 * @file FPSAssetPaths.h
 * @brief Single asset-root resolution for the SparkGameFPS module.
 *
 * The module used to mix two incompatible roots: scene files were loaded
 * as "Assets/..." (relative to the working directory) while every model was
 * loaded as "../Assets/..." (relative to the parent of the working directory).
 * Exactly one of those can be correct for a given layout, so the staged and
 * installed packages silently rendered an empty arena.
 *
 * Every module asset path now goes through Resolve(), which is relative to a
 * single asset root discovered once from the executable directory and the
 * working directory.
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Spark
{
    namespace FPSAssets
    {
        /**
         * @brief Pick the first search base that holds a usable asset tree.
         *
         * A base qualifies when `<base>/Assets/Models` is a directory, which is
         * the marker every staged, installed and repository layout shares.
         *
         * @param searchBases Directories to probe, in priority order.
         * @return The matching `<base>/Assets` directory, or an empty path.
         */
        std::filesystem::path FindAssetRoot(const std::vector<std::filesystem::path>& searchBases);

        /**
         * @brief The bases probed by Root(): the host executable directory and
         *        its parent, then the working directory and its two parents.
         */
        std::vector<std::filesystem::path> DefaultSearchBases();

        /**
         * @brief The resolved asset root, computed once per process.
         * @return The discovered root, or `<working directory>/Assets` when the
         *         search found nothing (so error messages name a real path).
         */
        const std::filesystem::path& Root();

        /** @brief True when Root() actually exists on disk. */
        bool RootExists();

        /**
         * @brief Resolve a path expressed relative to the asset root.
         * @param relativeToAssetRoot For example `L"Models/pistol.obj"`.
         */
        std::wstring Resolve(const std::wstring& relativeToAssetRoot);

        /**
         * @brief UTF-8 form of Resolve(), for APIs that take std::string paths.
         * @param relativeToAssetRoot UTF-8 relative path, for example "Scenes/level1.scene".
         * @return The absolute path encoded as UTF-8 (not the platform narrow encoding).
         */
        std::string ResolveUtf8(const std::string& relativeToAssetRoot);
    } // namespace FPSAssets
} // namespace Spark
