/**
 * @file HostScenePreview.h
 * @brief Platform-neutral `-scene <path>` load and machine-readable launch records.
 *
 * Runtime hosts that honour `-scene` load one reflected scene into a World and
 * must make both outcomes observable to a launching process: a scene that
 * cannot load fails the launch (exit code kHostSceneLoadFailedExitCode) rather
 * than leaving an empty engine running, and a scene that loads prints
 *
 *   SPARK_SCENE_LOADED entities=N renderables=M
 *   SPARK_SCENE_ASSETS refs=R missing=K
 *
 * on stdout. The asset record reports how many project-relative `Assets/...`
 * mesh/material references the scene carries and how many of them do not
 * resolve to an existing file under the project root derived from the scene
 * path, so a package that cannot reach its assets is visible without a GPU.
 */

#pragma once

#include <cstddef>
#include <string>

class World;

namespace Spark
{
    /// Process exit status for a `-scene` launch whose scene failed to load.
    inline constexpr int kHostSceneLoadFailedExitCode = 4;

    /** @brief Outcome of LoadHostScene. */
    struct HostSceneLoadReport
    {
        size_t entities = 0;      ///< Entities in the loaded world.
        size_t renderables = 0;   ///< Entities with Transform + MeshRenderer.
        size_t assetRefs = 0;     ///< Non-empty `Assets/...` mesh/material references on renderables.
        size_t assetsMissing = 0; ///< References that do not resolve to an existing project file.
        std::string projectRoot;  ///< Canonical project root, empty when the scene is not under `<root>/Scenes`.
        std::string error;        ///< Load failure reason; empty on success.
    };

    /**
     * @brief Load @p scenePath into @p world and audit its project asset references.
     *
     * The world is replaced only when the scene fully deserializes. Missing
     * assets do not fail the load (the renderer tolerates them); they are
     * counted so callers and launch checks can see them.
     *
     * @return true on success; false with @p report.error set otherwise.
     */
    bool LoadHostScene(::World& world, const std::string& scenePath, HostSceneLoadReport& report);

    /** @brief Print the SPARK_SCENE_LOADED and SPARK_SCENE_ASSETS records to stdout and flush. */
    void PrintHostSceneRecords(const HostSceneLoadReport& report);
} // namespace Spark
