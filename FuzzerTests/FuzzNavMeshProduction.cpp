/**
 * @file FuzzNavMeshProduction.cpp
 * @brief libc++-compiled production adapter for the .snav libFuzzer harness.
 *
 * NavMeshManager::LoadNavMesh opens the file and hands its bytes to DecodeSnav, then runs
 * RebuildTriangleAdjacency on the result. The adapter drives the same two functions over an
 * in-memory stream and checks the invariants every NavMeshQuery relies on. A violation aborts so
 * libFuzzer records a crash rather than a silent pass:
 *  - a rejected mesh leaves the caller's NavMeshData untouched,
 *  - every triangle index is below the vertex count and every dynamic adjacency entry below the
 *    triangle count,
 *  - every stored float is finite, triangles are walkable and carry no fixed links yet,
 *  - after the rebuild every fixed link names another in-range triangle or is UINT32_MAX.
 */

#include "FuzzNavMeshProduction.h"

#include "Engine/AI/NavMesh.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    /// LoadNavMesh rebuilds up to kMaxSnavAdjacencyRebuild triangles; 64 KiB holds at most ~1500, so
    /// every accepted input is rebuilt here, at a cost the -timeout budget covers.
    constexpr std::size_t kMaxRebuildTriangles = 2000;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzNavMesh: DecodeSnav violated: %s\n", what);
        std::abort();
    }

    bool IsFinite(const XMFLOAT3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    void CheckDecoded(const Spark::AI::NavMeshData& mesh)
    {
        if (!std::isfinite(mesh.cellSize) || !std::isfinite(mesh.agentHeight) || !std::isfinite(mesh.agentRadius) ||
            !IsFinite(mesh.boundsMin) || !IsFinite(mesh.boundsMax))
            InvariantFailure("non-finite setting or bound");
        for (const auto& vertex : mesh.vertices)
        {
            if (!IsFinite(vertex.position))
                InvariantFailure("non-finite vertex position");
        }
        const std::size_t vertexCount = mesh.vertices.size();
        const std::size_t triangleCount = mesh.triangles.size();
        for (const auto& tri : mesh.triangles)
        {
            for (const std::uint32_t index : tri.indices)
            {
                if (index >= vertexCount)
                    InvariantFailure("triangle index past the vertex array");
            }
            for (const std::uint32_t neighbor : tri.adjacency)
            {
                if (neighbor >= triangleCount)
                    InvariantFailure("adjacency entry past the triangle array");
            }
            if (!IsFinite(tri.centroid) || !IsFinite(tri.normal) || !std::isfinite(tri.area))
                InvariantFailure("non-finite centroid, normal or area");
            if (tri.flags != 0xFFFF)
                InvariantFailure("decoded triangle is not walkable");
            for (const std::uint32_t link : tri.neighborTriangles)
            {
                if (link != UINT32_MAX)
                    InvariantFailure("decoded triangle carries a fixed link the format does not store");
            }
        }
    }

    void CheckRebuilt(const Spark::AI::NavMeshData& mesh)
    {
        const std::size_t triangleCount = mesh.triangles.size();
        for (std::size_t t = 0; t < triangleCount; ++t)
        {
            for (const std::uint32_t link : mesh.triangles[t].neighborTriangles)
            {
                if (link == UINT32_MAX)
                    continue;
                if (link >= triangleCount || link == t)
                    InvariantFailure("rebuilt fixed link is out of range or self-referential");
            }
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoder.
extern "C" int SparkFuzzDecodeSnav(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    std::istringstream stream(size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size));

    // A sentinel mesh: a rejected decode must not touch it.
    Spark::AI::NavMeshData mesh;
    mesh.vertices.resize(1);
    mesh.vertices[0].position = XMFLOAT3{7.0f, 7.0f, 7.0f};
    std::string error;
    if (!Spark::AI::DecodeSnav(stream, size, mesh, error))
    {
        if (error.empty())
            InvariantFailure("rejected without a reason");
        if (mesh.vertices.size() != 1 || mesh.vertices[0].position.x != 7.0f || !mesh.triangles.empty())
            InvariantFailure("a rejected mesh modified the caller's data");
        return 0;
    }

    CheckDecoded(mesh);
    if (mesh.triangles.size() <= kMaxRebuildTriangles)
    {
        Spark::AI::RebuildTriangleAdjacency(mesh);
        CheckRebuilt(mesh);
    }
    return 0;
}
