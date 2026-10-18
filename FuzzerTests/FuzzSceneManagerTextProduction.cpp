/**
 * @file FuzzSceneManagerTextProduction.cpp
 * @brief libc++-compiled production adapter for the SceneManager text-scene libFuzzer harness.
 *
 * SceneManager::LoadScene reads a .json path with the versioned row reader and
 * a .scene path through LoadCustom's dialect dispatch. The adapter follows both:
 * every input goes to Spark::ParseVersionedSceneText, and then to the parser
 * Spark::DetectSceneTextDialect picks for it, exactly as LoadCustom does. A
 * violation of the readers' contract aborts so libFuzzer records a crash rather
 * than a silent pass:
 *  - a rejected document leaves the caller's outputs untouched (the loaders
 *    rely on this to keep the live scene when a replacement is malformed),
 *  - an accepted node list is non-empty (LoadScene treats empty as failure) and
 *    holds at most kMaxSceneTextNodes nodes, each with a type, a name, finite
 *    position/rotation/scale, a parentIndex in [-1, n) other than itself, no
 *    parent cycle (a bounded walk here, independent of the reader's own check),
 *    and childIndices that list exactly the nodes naming it as parent,
 *  - accepted INI node names are unique (LoadCustom indexes nodes by name),
 *  - accepted legacy rows name a known primitive with finite, positive
 *    dimensions and sphere tessellation inside [3|2, 256], so constructing the
 *    object cannot trip a primitive's precondition or size an unbounded mesh,
 *  - a versioned scene SaveScene would write reloads: SerializeVersionedSceneText
 *    accepts every versioned document the reader accepts, the output parses back
 *    to the same node count, and writing that result reproduces it byte for byte.
 *    An INI scene the writer accepts must reload the same way.
 *
 * The 100,000-node cap lies above what the smoke's -max_len can express; the
 * SceneManager_TextParsersRejectNodeCountAboveCap test pins it.
 */

#include "FuzzSceneManagerTextProduction.h"

#include "SceneManager/SceneTextFormat.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr const char* kSentinelName = "sentinel-node-left-by-a-rejected-document";

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzSceneManagerText: text scene reader violated: %s\n", what);
        std::abort();
    }

    bool IsFinite(const DirectX::XMFLOAT3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    std::vector<SceneNode> SentinelNodes()
    {
        SceneNode sentinel;
        sentinel.type = "Sentinel";
        sentinel.name = kSentinelName;
        return {sentinel};
    }

    bool IsUntouched(const std::vector<SceneNode>& nodes, const SceneMetadata& metadata)
    {
        return nodes.size() == 1 && nodes.front().name == kSentinelName && metadata.sceneName == kSentinelName;
    }

    void CheckNodes(const std::vector<SceneNode>& nodes)
    {
        const std::size_t count = nodes.size();
        if (count == 0)
            InvariantFailure("accepted an empty node list");
        if (count > Spark::kMaxSceneTextNodes)
            InvariantFailure("node count above kMaxSceneTextNodes");

        std::size_t linkedChildren = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const SceneNode& node = nodes[i];
            if (node.type.empty() || node.name.empty())
                InvariantFailure("node without a type or name");
            if (!IsFinite(node.position) || !IsFinite(node.rotation) || !IsFinite(node.scale))
                InvariantFailure("non-finite node transform");
            if (node.parentIndex < -1 || node.parentIndex >= static_cast<int>(count) ||
                node.parentIndex == static_cast<int>(i))
                InvariantFailure("parentIndex outside [-1, n) or naming the node itself");
            if (node.parentIndex >= 0)
                ++linkedChildren;
            for (const int child : node.childIndices)
            {
                if (child < 0 || child >= static_cast<int>(count) ||
                    nodes[static_cast<std::size_t>(child)].parentIndex != static_cast<int>(i))
                    InvariantFailure("childIndices entry that does not name this node as parent");
            }
        }

        std::size_t listedChildren = 0;
        for (const SceneNode& node : nodes)
            listedChildren += node.childIndices.size();
        if (listedChildren != linkedChildren)
            InvariantFailure("childIndices do not list every parented node exactly once");

        // Bounded walk: a chain longer than the node count must revisit a node.
        for (std::size_t i = 0; i < count; ++i)
        {
            int current = static_cast<int>(i);
            for (std::size_t steps = 0; current >= 0; ++steps)
            {
                if (steps > count)
                    InvariantFailure("parent chain cycles");
                current = nodes[static_cast<std::size_t>(current)].parentIndex;
            }
        }
    }

    /// A document the writer accepts must reload to the same node count, and
    /// writing the reloaded scene must reproduce the text byte for byte.
    void CheckWriterRoundTrip(const SceneMetadata& metadata, const std::vector<SceneNode>& nodes, bool mustWrite)
    {
        std::string written;
        if (!Spark::SerializeVersionedSceneText(metadata, nodes, written))
        {
            if (mustWrite)
                InvariantFailure("an accepted versioned scene cannot be written back");
            return;
        }
        SceneMetadata reloadedMetadata;
        std::vector<SceneNode> reloaded;
        if (!Spark::ParseVersionedSceneText(written, reloadedMetadata, reloaded))
            InvariantFailure("SerializeVersionedSceneText output does not reload");
        if (reloaded.size() != nodes.size())
            InvariantFailure("reloaded scene has a different node count");
        std::string rewritten;
        if (!Spark::SerializeVersionedSceneText(reloadedMetadata, reloaded, rewritten) || rewritten != written)
            InvariantFailure("writing a reloaded scene does not reproduce the saved text");
    }

    void CheckLegacyRows(const std::vector<Spark::LegacyObjectRow>& rows)
    {
        if (rows.empty())
            InvariantFailure("accepted an empty legacy object list");
        if (rows.size() > Spark::kMaxSceneTextNodes)
            InvariantFailure("legacy row count above kMaxSceneTextNodes");
        for (const Spark::LegacyObjectRow& row : rows)
        {
            const bool known = row.type == "Cube" || row.type == "Plane" || row.type == "Sphere" ||
                               row.type == "Pyramid" || row.type == "Ramp" || row.type == "Wall";
            if (!known)
                InvariantFailure("legacy row with an unknown primitive type");
            if (!IsFinite(row.position))
                InvariantFailure("legacy row with a non-finite position");
            if (!std::isfinite(row.primary) || row.primary <= 0.0f || !std::isfinite(row.secondary) ||
                row.secondary <= 0.0f)
                InvariantFailure("legacy row with a non-positive or non-finite dimension");
            if (row.type == "Sphere" &&
                (row.slices < Spark::kMinLegacySphereSlices || row.slices > Spark::kMaxLegacySphereTessellation ||
                 row.stacks < Spark::kMinLegacySphereStacks || row.stacks > Spark::kMaxLegacySphereTessellation))
                InvariantFailure("legacy sphere tessellation outside its bounds");
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production parsers.
extern "C" int SparkFuzzParseSceneManagerText(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;
    const std::string_view content =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);

    // .json path: LoadJSON reads any content as versioned rows.
    SceneMetadata metadata;
    metadata.sceneName = kSentinelName;
    std::vector<SceneNode> nodes = SentinelNodes();
    if (Spark::ParseVersionedSceneText(content, metadata, nodes))
    {
        CheckNodes(nodes);
        CheckWriterRoundTrip(metadata, nodes, true);
    }
    else if (!IsUntouched(nodes, metadata))
        InvariantFailure("a rejected versioned document modified the caller's outputs");

    // .scene path: LoadCustom's dispatch (the versioned case is covered above).
    switch (Spark::DetectSceneTextDialect(content))
    {
    case Spark::SceneTextDialect::Versioned:
        break;
    case Spark::SceneTextDialect::Ini:
    {
        SceneMetadata iniMetadata;
        iniMetadata.sceneName = kSentinelName;
        std::vector<SceneNode> iniNodes = SentinelNodes();
        if (Spark::ParseIniSceneText(content, iniMetadata, iniNodes))
        {
            CheckNodes(iniNodes);
            for (std::size_t i = 0; i < iniNodes.size(); ++i)
            {
                for (std::size_t j = i + 1; j < iniNodes.size(); ++j)
                {
                    if (iniNodes[i].name == iniNodes[j].name)
                        InvariantFailure("accepted INI scene with duplicate node names");
                }
            }
            CheckWriterRoundTrip(iniMetadata, iniNodes, false);
        }
        else if (!IsUntouched(iniNodes, iniMetadata))
            InvariantFailure("a rejected INI document modified the caller's outputs");
        break;
    }
    case Spark::SceneTextDialect::LegacyObjects:
    {
        std::vector<Spark::LegacyObjectRow> rows(1);
        rows.front().type = kSentinelName;
        if (Spark::ParseLegacyObjectLines(content, rows))
            CheckLegacyRows(rows);
        else if (rows.size() != 1 || rows.front().type != kSentinelName)
            InvariantFailure("a rejected legacy document modified the caller's outputs");
        break;
    }
    }
    return 0;
}
