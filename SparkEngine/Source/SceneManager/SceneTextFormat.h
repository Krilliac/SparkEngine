/**
 * @file SceneTextFormat.h
 * @brief Pure parsers and writer for SceneManager's text scene formats.
 *
 * SceneManager reads three text dialects: the versioned rows SaveScene writes
 * ("# SparkEngine Scene v1.0"), the authored INI sections ([Scene], [Object],
 * [Camera], [SpawnPoint]) and the oldest "Type x y z [params]" object lines.
 * These functions decode them without touching files, graphics or the live
 * scene, so SceneManager only reads the file and commits the result, and the
 * SEC-120 fuzz target (FuzzerTests/FuzzSceneManagerText.cpp) drives the exact
 * code the loader runs.
 *
 * Contract: thread-agnostic and reentrant (no shared state); every parser
 * writes its outputs only when it accepts the whole document, so a rejected
 * document leaves them untouched; allocation is proportional to the input and
 * capped at kMaxSceneTextNodes nodes. Scalability: linear in the input size,
 * hierarchy validation included.
 */

#pragma once

#include "SceneManager/SceneManagerTypes.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{
    /// Most nodes any text scene may declare, matching the streaming scene-manifest
    /// entry cap. A document with more rows is rejected before it is committed.
    inline constexpr std::size_t kMaxSceneTextNodes = 100000;

    /// Sphere tessellation the legacy object lines may request. SphereObject
    /// requires at least 3 slices and 2 stacks; the upper bound keeps one row from
    /// sizing an unbounded mesh.
    inline constexpr int kMinLegacySphereSlices = 3;
    inline constexpr int kMinLegacySphereStacks = 2;
    inline constexpr int kMaxLegacySphereTessellation = 256;

    /// Which dialect SceneManager::LoadCustom reads a .scene document as.
    enum class SceneTextDialect
    {
        Versioned,    ///< "# SparkEngine Scene v1.0" header (SaveScene output).
        Ini,          ///< [Scene]/[Object]/[Camera]/[SpawnPoint] sections.
        LegacyObjects ///< "Type x y z [params]" lines.
    };

    /// @brief Choose the dialect for a .scene document, as SceneManager::LoadCustom does.
    SceneTextDialect DetectSceneTextDialect(std::string_view content);

    /**
     * @brief Parse the versioned row format written by SceneManager::SaveScene.
     * @param content Whole document.
     * @param metadata In: the metadata to start from (header lines override fields). Out: the
     *        scene's metadata; untouched on rejection.
     * @param nodes Out: the nodes with rebuilt childIndices; untouched on rejection.
     * @return true when every row is well formed, finite and the hierarchy is acyclic.
     */
    bool ParseVersionedSceneText(std::string_view content, SceneMetadata& metadata, std::vector<SceneNode>& nodes);

    /**
     * @brief Parse the authored INI scene dialect.
     * @param content Whole document.
     * @param metadata Out: the [Scene] metadata over SceneMetadata defaults; untouched on rejection.
     * @param nodes Out: one node per object/camera/spawn section; untouched on rejection.
     * @return true when every section is well formed and node names are unique.
     */
    bool ParseIniSceneText(std::string_view content, SceneMetadata& metadata, std::vector<SceneNode>& nodes);

    /// One validated "Type x y z [params]" line of the legacy object format.
    struct LegacyObjectRow
    {
        std::string type; ///< Cube, Plane, Sphere, Pyramid, Ramp or Wall.
        int lineNumber = 0;
        DirectX::XMFLOAT3 position = {0, 0, 0};
        float primary = 1.0f;   ///< Cube/Pyramid size, Plane/Wall width, Ramp length, Sphere radius.
        float secondary = 1.0f; ///< Plane depth, Ramp/Wall height; unused by the other shapes.
        int slices = 16;        ///< Sphere only.
        int stacks = 16;        ///< Sphere only.
    };

    /**
     * @brief Parse the legacy object-line dialect into rows the primitive constructors accept.
     *
     * Every dimension is finite and positive and sphere tessellation lies within
     * [kMinLegacySphereSlices|Stacks, kMaxLegacySphereTessellation], so constructing the
     * object can neither trip a primitive's precondition nor size an unbounded mesh.
     * @param content Whole document.
     * @param rows Out: one row per object line; untouched on rejection.
     * @return true when every line is a known shape with valid parameters.
     */
    bool ParseLegacyObjectLines(std::string_view content, std::vector<LegacyObjectRow>& rows);

    /**
     * @brief Write @p nodes in the versioned row format ParseVersionedSceneText reads.
     *
     * Nodes with an empty type are dropped and parent indices remapped around them.
     * @param metadata Scene metadata; every numeric field must be finite.
     * @param nodes Scene nodes.
     * @param text Out: the document; untouched on failure.
     * @return false when the remapped hierarchy or the metadata is malformed, or a
     *         node could not be read back (a type that would read as a comment).
     */
    bool SerializeVersionedSceneText(const SceneMetadata& metadata, const std::vector<SceneNode>& nodes,
                                     std::string& text);
} // namespace Spark
