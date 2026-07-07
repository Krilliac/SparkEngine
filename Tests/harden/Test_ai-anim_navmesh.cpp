// Test_ai-anim_navmesh.cpp - Hardening regression tests for the NavMesh subsystem.
//
// Covers two audit findings against SparkEngine/Source/Engine/AI/NavMesh.cpp:
//   * P1: NavMeshManager::LoadNavMesh never populated NavTriangle::neighborTriangles,
//         so loaded meshes had corrupt A* adjacency (every edge pointed at triangle 0).
//   * P3: NavMeshQuery::IsPointOnNavMesh ignored its documented `tolerance` parameter.
//
// These exercise the real engine implementation (SparkTests links SparkEngineLib).

#include "TestFramework.h"

#include "Engine/AI/NavMesh.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Spark::AI;

namespace
{
    // Append raw little-endian bytes of a trivially-copyable value to a byte buffer.
    template <typename T> void PutBytes(std::vector<char>& buf, const T& value)
    {
        const char* p = reinterpret_cast<const char*>(&value);
        buf.insert(buf.end(), p, p + sizeof(T));
    }

    // Write a minimal but format-correct .snav file with the given vertices/triangles.
    // Matches the binary layout read by NavMeshManager::LoadNavMesh exactly. The fixed
    // neighborTriangles array is intentionally NOT part of the format — the loader must
    // reconstruct it — which is precisely what this test verifies.
    std::string WriteSnav(const std::vector<XMFLOAT3>& verts,
                          const std::vector<std::array<uint32_t, 3>>& triIndices)
    {
        std::vector<char> buf;
        buf.insert(buf.end(), {'S', 'N', 'A', 'V'});
        PutBytes(buf, uint32_t{1}); // version

        PutBytes(buf, float{0.3f}); // cellSize
        PutBytes(buf, float{2.0f}); // agentHeight
        PutBytes(buf, float{0.6f}); // agentRadius
        PutBytes(buf, XMFLOAT3{0, 0, 0});    // boundsMin
        PutBytes(buf, XMFLOAT3{1, 0, 1});    // boundsMax

        PutBytes(buf, static_cast<uint32_t>(verts.size()));
        for (const auto& v : verts)
            PutBytes(buf, v);

        PutBytes(buf, static_cast<uint32_t>(triIndices.size()));
        for (const auto& idx : triIndices)
        {
            PutBytes(buf, idx[0]);
            PutBytes(buf, idx[1]);
            PutBytes(buf, idx[2]);
            PutBytes(buf, XMFLOAT3{0, 0, 0}); // centroid (unused by adjacency)
            PutBytes(buf, XMFLOAT3{0, 1, 0}); // normal
            PutBytes(buf, float{0.5f});       // area
            PutBytes(buf, uint32_t{0});       // adjCount (dynamic adjacency)
        }

        std::filesystem::path path =
            std::filesystem::temp_directory_path() / "spark_harden_navmesh.snav";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        out.close();
        return path.string();
    }
} // namespace

// ============================================================================
// P1 regression: loaded meshes get correct fixed edge adjacency, not {0,0,0}.
// ============================================================================

TEST(NavMesh_LoadNavMesh_RebuildsEdgeAdjacency)
{
    // Two triangles of a quad sharing edge (v1, v2).
    std::vector<XMFLOAT3> verts = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1}};
    std::vector<std::array<uint32_t, 3>> tris = {{0, 1, 2}, {1, 3, 2}};

    std::string path = WriteSnav(verts, tris);

    auto& mgr = NavMeshManager::GetInstance();
    bool loaded = mgr.LoadNavMesh("harden_adjacency", path);
    EXPECT_TRUE(loaded);

    const NavMeshData* nm = mgr.GetNavMesh("harden_adjacency");
    EXPECT_TRUE(nm != nullptr);
    if (nm)
    {
        EXPECT_EQ(nm->triangles.size(), 2u);

        // The two triangles share an edge, so each must reference the other.
        EXPECT_EQ(nm->triangles[0].neighborTriangles[0], 1u);
        EXPECT_EQ(nm->triangles[1].neighborTriangles[0], 0u);

        // Non-shared edges must be the "no neighbor" sentinel, NOT the pre-fix
        // value of 0 (which A* would have misread as "borders triangle 0").
        EXPECT_EQ(nm->triangles[0].neighborTriangles[1], UINT32_MAX);
        EXPECT_EQ(nm->triangles[0].neighborTriangles[2], UINT32_MAX);
        EXPECT_EQ(nm->triangles[1].neighborTriangles[1], UINT32_MAX);
    }

    mgr.RemoveNavMesh("harden_adjacency");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ============================================================================
// P3 regression: IsPointOnNavMesh honors the `tolerance` argument.
// ============================================================================

TEST(NavMesh_IsPointOnNavMesh_HonorsTolerance)
{
    // A single large flat quad on the y=0 plane.
    std::vector<XMFLOAT3> verts = {{-5, 0, -5}, {5, 0, -5}, {-5, 0, 5}, {5, 0, 5}};
    std::vector<uint32_t> indices = {0, 1, 2, 1, 3, 2};

    auto& mgr = NavMeshManager::GetInstance();
    NavMeshBuildSettings settings;
    bool built = mgr.BuildNavMesh("harden_tolerance", verts, indices, settings);
    EXPECT_TRUE(built);

    auto query = mgr.CreateQuery("harden_tolerance");
    EXPECT_TRUE(query != nullptr);
    if (query)
    {
        // A point on the surface is on the navmesh at any sane tolerance.
        EXPECT_TRUE(query->IsPointOnNavMesh({0.0f, 0.0f, 0.0f}, 0.5f));

        // A point 1 m above the surface is OUTSIDE a 0.5 m tolerance (this returned
        // true before the fix because the tolerance argument was discarded)...
        EXPECT_FALSE(query->IsPointOnNavMesh({0.0f, 1.0f, 0.0f}, 0.5f));

        // ...but WITHIN a 2 m tolerance.
        EXPECT_TRUE(query->IsPointOnNavMesh({0.0f, 1.0f, 0.0f}, 2.0f));
    }

    mgr.RemoveNavMesh("harden_tolerance");
}
