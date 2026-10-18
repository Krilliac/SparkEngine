/**
 * @file BatchOutputPath.h
 * @brief Containment-safe relative path for SparkShaderCompiler -batch -o outputs.
 *
 * Thread affinity: any thread (pure function, no filesystem access).
 * Ownership: returns a value; holds no state.
 * Allocation: allocates only the returned path (tool code, not a hot path).
 */

#pragma once

#include <filesystem>
#include <optional>

namespace SparkShaderCompiler
{
    /**
     * @brief Path of @p source relative to @p batchRoot, computed lexically.
     *
     * The result is joined under the -o directory, so it must never leave it. It is
     * derived purely from the spelling of the path the directory iterator returned:
     * std::filesystem::relative() is not used because it resolves symlinks and
     * junctions (weakly_canonical), which turned `<batch>/x.hlsl -> /elsewhere/x.hlsl`
     * into `../../elsewhere/x.hlsl` and wrote the artifact outside the output directory.
     *
     * @param source    A discovered shader path (normally `batchRoot / ...`).
     * @param batchRoot The -batch directory as given on the command line.
     * @return The relative path, or std::nullopt when it is empty, absolute, rooted, or
     *         contains a `.`, `..` or empty component (the caller must refuse the plan).
     */
    inline std::optional<std::filesystem::path> SafeBatchRelativePath(const std::filesystem::path& source,
                                                                      const std::filesystem::path& batchRoot)
    {
        namespace fs = std::filesystem;

        // "shaders/" normalises to "shaders/", whose trailing empty element would count
        // as a directory level in lexically_relative; drop it.
        fs::path root = batchRoot.lexically_normal();
        if (!root.empty() && !root.has_filename())
            root = root.parent_path();

        const fs::path relative = source.lexically_normal().lexically_relative(root);
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
            return std::nullopt;

        for (const fs::path& component : relative)
        {
            if (component.empty() || component == "." || component == "..")
                return std::nullopt;
        }
        return relative;
    }
} // namespace SparkShaderCompiler
