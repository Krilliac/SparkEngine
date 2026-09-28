/**
 * @file NavMeshFormat.cpp
 * @brief .snav decoder and fixed edge-adjacency rebuild
 *
 * Kept apart from NavMesh.cpp so the decoder has no Recast/Detour or logger dependency: the
 * SparkFuzzNavMesh harness links this file alone.
 */

#include "NavMesh.h"

#include <cmath>
#include <cstring>
#include <format>
#include <istream>
#include <utility>

namespace Spark::AI
{

    namespace
    {
        /// On-disk record sizes. Every field is a little-endian 32-bit value.
        constexpr uint64_t kVertexRecordBytes = sizeof(float) * 3;
        /// Smallest triangle record: indices(12) + centroid(12) + normal(12) + area(4) + adjacency count(4).
        constexpr uint64_t kMinTriangleRecordBytes = sizeof(uint32_t) * 3 + sizeof(float) * 7 + sizeof(uint32_t);

        constexpr uint32_t kMaxVertices = 10'000'000;
        constexpr uint32_t kMaxTriangles = 10'000'000;
        constexpr uint32_t kMaxAdjacencyPerTriangle = 10'000;
        constexpr uint32_t kMaxVersion = 1;

        /// The format stores no per-triangle area flags. Both builders mark every triangle walkable, and
        /// NavMeshQuery::FindPath skips a neighbor whose flags miss the request's include mask, so a
        /// loaded mesh gets the same default instead of flags 0 (which made every multi-triangle path fail).
        constexpr uint16_t kDefaultTriangleFlags = 0xFFFF;

        /// Bounded cursor: a read never runs past `size`, and the first failure is sticky.
        class SnavCursor
        {
          public:
            SnavCursor(std::istream& stream, uint64_t size) : m_stream(stream), m_remaining(size) {}

            uint64_t Remaining() const { return m_remaining; }

            bool Read(void* destination, uint64_t count)
            {
                if (m_failed || count > m_remaining)
                {
                    m_failed = true;
                    return false;
                }
                if (count == 0)
                    return true;
                m_stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
                if (!m_stream.good())
                {
                    m_failed = true;
                    return false;
                }
                m_remaining -= count;
                return true;
            }

            bool ReadU32(uint32_t& out) { return Read(&out, sizeof(out)); }

            bool ReadFloat3(XMFLOAT3& out)
            {
                float values[3] = {};
                if (!Read(values, sizeof(values)))
                    return false;
                out = XMFLOAT3{values[0], values[1], values[2]};
                return true;
            }

          private:
            std::istream& m_stream;
            uint64_t m_remaining;
            bool m_failed = false;
        };

        bool IsFinite(const XMFLOAT3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        std::string CountBeyondFile(const char* what, uint64_t count, uint64_t recordBytes, uint64_t remaining)
        {
            return std::format("declares {} {} ({} bytes each) but only {} bytes remain in the file", count, what,
                               recordBytes, remaining);
        }
    } // namespace

    bool DecodeSnav(std::istream& stream, uint64_t size, NavMeshData& out, std::string& error)
    {
        SnavCursor cursor(stream, size);

        char magic[4] = {};
        if (!cursor.Read(magic, sizeof(magic)) || std::memcmp(magic, "SNAV", sizeof(magic)) != 0)
        {
            error = "is not a SparkEngine navmesh (bad magic)";
            return false;
        }

        uint32_t version = 0;
        if (!cursor.ReadU32(version) || version > kMaxVersion)
        {
            error = std::format("has unsupported version {} (max supported: {})", version, kMaxVersion);
            return false;
        }

        NavMeshData mesh;
        cursor.Read(&mesh.cellSize, sizeof(float));
        cursor.Read(&mesh.agentHeight, sizeof(float));
        cursor.Read(&mesh.agentRadius, sizeof(float));
        cursor.ReadFloat3(mesh.boundsMin);
        if (!cursor.ReadFloat3(mesh.boundsMax))
        {
            error = "is truncated in its settings block";
            return false;
        }
        if (!std::isfinite(mesh.cellSize) || !std::isfinite(mesh.agentHeight) || !std::isfinite(mesh.agentRadius) ||
            !IsFinite(mesh.boundsMin) || !IsFinite(mesh.boundsMax))
        {
            error = "has a non-finite setting or bound";
            return false;
        }

        uint32_t vertexCount = 0;
        if (!cursor.ReadU32(vertexCount) || vertexCount > kMaxVertices)
        {
            error = std::format("declares {} vertices (max {})", vertexCount, kMaxVertices);
            return false;
        }
        if (vertexCount > cursor.Remaining() / kVertexRecordBytes)
        {
            error = CountBeyondFile("vertices", vertexCount, kVertexRecordBytes, cursor.Remaining());
            return false;
        }
        mesh.vertices.resize(vertexCount);
        for (uint32_t v = 0; v < vertexCount; ++v)
        {
            if (!cursor.ReadFloat3(mesh.vertices[v].position))
            {
                error = "is truncated in its vertex block";
                return false;
            }
            if (!IsFinite(mesh.vertices[v].position))
            {
                error = std::format("has a non-finite position on vertex {}", v);
                return false;
            }
        }

        uint32_t triangleCount = 0;
        if (!cursor.ReadU32(triangleCount) || triangleCount > kMaxTriangles)
        {
            error = std::format("declares {} triangles (max {})", triangleCount, kMaxTriangles);
            return false;
        }
        if (triangleCount > cursor.Remaining() / kMinTriangleRecordBytes)
        {
            error = CountBeyondFile("triangles", triangleCount, kMinTriangleRecordBytes, cursor.Remaining());
            return false;
        }
        mesh.triangles.resize(triangleCount);
        for (uint32_t t = 0; t < triangleCount; ++t)
        {
            NavTriangle& tri = mesh.triangles[t];
            cursor.ReadU32(tri.indices[0]);
            cursor.ReadU32(tri.indices[1]);
            cursor.ReadU32(tri.indices[2]);
            cursor.ReadFloat3(tri.centroid);
            cursor.ReadFloat3(tri.normal);
            cursor.Read(&tri.area, sizeof(float));
            uint32_t adjacencyCount = 0;
            if (!cursor.ReadU32(adjacencyCount))
            {
                error = std::format("is truncated in triangle {}", t);
                return false;
            }

            // Every NavMeshQuery indexes vertices[indices[k]]; an index past the vertex array is a
            // corrupt mesh, not a triangle to skip.
            for (uint32_t k = 0; k < 3; ++k)
            {
                if (tri.indices[k] >= vertexCount)
                {
                    error = std::format("triangle {} names vertex {} but the mesh has {} vertices", t, tri.indices[k],
                                        vertexCount);
                    return false;
                }
            }
            if (!IsFinite(tri.centroid) || !IsFinite(tri.normal) || !std::isfinite(tri.area))
            {
                error = std::format("has a non-finite centroid, normal or area on triangle {}", t);
                return false;
            }

            // The fixed edge links are not stored; UINT32_MAX (no neighbor) until RebuildTriangleAdjacency.
            tri.neighborTriangles[0] = UINT32_MAX;
            tri.neighborTriangles[1] = UINT32_MAX;
            tri.neighborTriangles[2] = UINT32_MAX;
            tri.flags = kDefaultTriangleFlags;

            if (adjacencyCount > kMaxAdjacencyPerTriangle)
            {
                error = std::format("declares {} adjacency entries on triangle {} (max {})", adjacencyCount, t,
                                    kMaxAdjacencyPerTriangle);
                return false;
            }
            if (adjacencyCount > cursor.Remaining() / sizeof(uint32_t))
            {
                error = CountBeyondFile("adjacency entries", adjacencyCount, sizeof(uint32_t), cursor.Remaining());
                return false;
            }
            tri.adjacency.resize(adjacencyCount);
            if (!cursor.Read(tri.adjacency.data(), uint64_t{adjacencyCount} * sizeof(uint32_t)))
            {
                error = std::format("is truncated in the adjacency of triangle {}", t);
                return false;
            }
            for (const uint32_t neighbor : tri.adjacency)
            {
                if (neighbor >= triangleCount)
                {
                    error = std::format("triangle {} links to triangle {} but the mesh has {} triangles", t, neighbor,
                                        triangleCount);
                    return false;
                }
            }
        }

        out = std::move(mesh);
        return true;
    }

    void RebuildTriangleAdjacency(NavMeshData& navMesh)
    {
        for (auto& tri : navMesh.triangles)
        {
            tri.neighborTriangles[0] = UINT32_MAX;
            tri.neighborTriangles[1] = UINT32_MAX;
            tri.neighborTriangles[2] = UINT32_MAX;
        }

        const size_t vertexCount = navMesh.vertices.size();
        for (size_t i = 0; i < navMesh.triangles.size(); ++i)
        {
            for (size_t j = i + 1; j < navMesh.triangles.size(); ++j)
            {
                int shared = 0;
                for (int ei = 0; ei < 3; ++ei)
                {
                    for (int ej = 0; ej < 3; ++ej)
                    {
                        const uint32_t viIdx = navMesh.triangles[i].indices[ei];
                        const uint32_t vjIdx = navMesh.triangles[j].indices[ej];
                        if (viIdx >= vertexCount || vjIdx >= vertexCount)
                            continue;
                        const auto& vi = navMesh.vertices[viIdx].position;
                        const auto& vj = navMesh.vertices[vjIdx].position;
                        const float dx = vi.x - vj.x, dy = vi.y - vj.y, dz = vi.z - vj.z;
                        if (dx * dx + dy * dy + dz * dz < 0.001f)
                            shared++;
                    }
                }
                if (shared < 2)
                    continue;
                for (int e = 0; e < 3; ++e)
                {
                    if (navMesh.triangles[i].neighborTriangles[e] == UINT32_MAX)
                    {
                        navMesh.triangles[i].neighborTriangles[e] = static_cast<uint32_t>(j);
                        break;
                    }
                }
                for (int e = 0; e < 3; ++e)
                {
                    if (navMesh.triangles[j].neighborTriangles[e] == UINT32_MAX)
                    {
                        navMesh.triangles[j].neighborTriangles[e] = static_cast<uint32_t>(i);
                        break;
                    }
                }
            }
        }
    }

} // namespace Spark::AI
