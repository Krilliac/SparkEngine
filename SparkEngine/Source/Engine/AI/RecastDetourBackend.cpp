/**
 * @file RecastDetourBackend.cpp
 * @brief Recast/Detour NavMesh generation implementation
 */

#include "RecastDetourBackend.h"
#include "../../Utils/Validate.h"

#ifdef SPARK_RECAST_AVAILABLE

#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Spark::AI
{

    bool IsRecastAvailable()
    {
        return true;
    }

    std::unique_ptr<NavMeshData> BuildNavMeshWithRecast(const std::vector<XMFLOAT3>& vertices,
                                                        const std::vector<uint32_t>& indices,
                                                        const NavMeshBuildSettings& settings)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::AI);

        if (vertices.empty() || indices.size() < 3)
        {
            SPARK_LOG_WARN(Spark::LogCategory::AI, "BuildNavMeshWithRecast: empty geometry");
            return std::make_unique<NavMeshData>();
        }

        // Convert vertices to flat float array (Recast expects float[3] per vertex)
        const int nverts = static_cast<int>(vertices.size());
        std::vector<float> verts(nverts * 3);
        for (int i = 0; i < nverts; ++i)
        {
            verts[i * 3 + 0] = vertices[i].x;
            verts[i * 3 + 1] = vertices[i].y;
            verts[i * 3 + 2] = vertices[i].z;
        }

        // Convert indices to int array
        const int ntris = static_cast<int>(indices.size()) / 3;
        std::vector<int> tris(indices.size());
        for (size_t i = 0; i < indices.size(); ++i)
        {
            tris[i] = static_cast<int>(indices[i]);
        }

        // Compute bounds
        float bmin[3] = {verts[0], verts[1], verts[2]};
        float bmax[3] = {verts[0], verts[1], verts[2]};
        for (int i = 1; i < nverts; ++i)
        {
            bmin[0] = (std::min)(bmin[0], verts[i * 3 + 0]);
            bmin[1] = (std::min)(bmin[1], verts[i * 3 + 1]);
            bmin[2] = (std::min)(bmin[2], verts[i * 3 + 2]);
            bmax[0] = (std::max)(bmax[0], verts[i * 3 + 0]);
            bmax[1] = (std::max)(bmax[1], verts[i * 3 + 1]);
            bmax[2] = (std::max)(bmax[2], verts[i * 3 + 2]);
        }

        // Configure Recast
        rcConfig cfg{};
        cfg.cs = settings.cellSize;
        cfg.ch = settings.cellHeight;
        cfg.walkableSlopeAngle = settings.agentMaxSlope;
        cfg.walkableHeight = static_cast<int>(std::ceil(settings.agentHeight / settings.cellHeight));
        cfg.walkableClimb = static_cast<int>(std::floor(settings.agentMaxClimb / settings.cellHeight));
        cfg.walkableRadius = static_cast<int>(std::ceil(settings.agentRadius / settings.cellSize));
        cfg.maxEdgeLen = static_cast<int>(settings.edgeMaxLen / settings.cellSize);
        cfg.maxSimplificationError = settings.edgeMaxError;
        cfg.minRegionArea = static_cast<int>(settings.regionMinSize * settings.regionMinSize);
        cfg.mergeRegionArea = static_cast<int>(settings.regionMergeSize * settings.regionMergeSize);
        cfg.maxVertsPerPoly = settings.vertsPerPoly;
        cfg.detailSampleDist = settings.detailSampleDist < 0.9f ? 0.0f : settings.cellSize * settings.detailSampleDist;
        cfg.detailSampleMaxError = settings.cellHeight * settings.detailSampleMaxError;
        rcVcopy(cfg.bmin, bmin);
        rcVcopy(cfg.bmax, bmax);
        rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

        SPARK_LOG_INFO(Spark::LogCategory::AI, "Recast build: %d verts, %d tris, grid %dx%d, cs=%.2f, ch=%.2f", nverts,
                       ntris, cfg.width, cfg.height, cfg.cs, cfg.ch);

        // Allocate build context
        rcContext ctx;

        // Step 1: Create heightfield
        rcHeightfield* solid = rcAllocHeightfield();
        if (!solid || !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::AI, "Recast: failed to create heightfield");
            rcFreeHeightField(solid);
            return nullptr;
        }

        // Mark walkable triangles
        std::vector<unsigned char> triAreas(ntris, 0);
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris,
                                triAreas.data());

        // Rasterize triangles into heightfield
        if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), triAreas.data(), ntris, *solid,
                                  cfg.walkableClimb))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::AI, "Recast: rasterization failed");
            rcFreeHeightField(solid);
            return nullptr;
        }

        // Step 2: Filter walkable surfaces
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

        // Step 3: Build compact heightfield
        rcCompactHeightfield* chf = rcAllocCompactHeightfield();
        if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::AI, "Recast: compact heightfield failed");
            rcFreeHeightField(solid);
            rcFreeCompactHeightfield(chf);
            return nullptr;
        }
        rcFreeHeightField(solid);

        // Erode walkable area by agent radius
        if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::AI, "Recast: erosion failed");
            rcFreeCompactHeightfield(chf);
            return nullptr;
        }

        // Step 4: Build regions (watershed partitioning)
        if (!rcBuildDistanceField(&ctx, *chf))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::AI, "Recast: distance field failed");
            rcFreeCompactHeightfield(chf);
            return nullptr;
        }
        if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::AI, "Recast: region building failed");
            rcFreeCompactHeightfield(chf);
            return nullptr;
        }

        // Step 5: Trace and simplify contours
        rcContourSet* cset = rcAllocContourSet();
        if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::AI, "Recast: contour building failed");
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            return nullptr;
        }

        // Step 6: Build polygon mesh
        rcPolyMesh* pmesh = rcAllocPolyMesh();
        if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::AI, "Recast: poly mesh failed");
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            rcFreePolyMesh(pmesh);
            return nullptr;
        }

        // Step 7: Build detail mesh
        rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
        if (!dmesh ||
            !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::AI, "Recast: detail mesh failed");
            rcFreeCompactHeightfield(chf);
            rcFreeContourSet(cset);
            rcFreePolyMesh(pmesh);
            rcFreePolyMeshDetail(dmesh);
            return nullptr;
        }

        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);

        SPARK_LOG_INFO(Spark::LogCategory::AI, "Recast build complete: %d polys, %d verts, %d detail tris",
                       pmesh->npolys, pmesh->nverts, dmesh->ntris);

        // Step 8: Convert Recast poly mesh to SparkEngine NavMeshData
        auto navMesh = std::make_unique<NavMeshData>();
        navMesh->cellSize = settings.cellSize;
        navMesh->cellHeight = settings.cellHeight;
        navMesh->agentHeight = settings.agentHeight;
        navMesh->agentRadius = settings.agentRadius;
        navMesh->agentMaxClimb = settings.agentMaxClimb;
        navMesh->agentMaxSlope = settings.agentMaxSlope;
        navMesh->boundsMin = {bmin[0], bmin[1], bmin[2]};
        navMesh->boundsMax = {bmax[0], bmax[1], bmax[2]};

        // Extract triangles from detail mesh (highest quality)
        // Detail mesh has sub-triangle precision for height accuracy
        for (int i = 0; i < dmesh->nmeshes; ++i)
        {
            const unsigned int* meshDef = &dmesh->meshes[i * 4];
            const unsigned int bverts = meshDef[0];
            const unsigned int btris = meshDef[2];
            const unsigned int ntrisMesh = meshDef[3];

            for (unsigned int j = 0; j < ntrisMesh; ++j)
            {
                const unsigned char* t = &dmesh->tris[(btris + j) * 4];

                uint32_t baseIdx = static_cast<uint32_t>(navMesh->vertices.size());

                for (int k = 0; k < 3; ++k)
                {
                    // All detail mesh vertices (both poly-sourced and detail-only)
                    // are stored as floats in dmesh->verts
                    const float* v = &dmesh->verts[(bverts + t[k]) * 3];
                    NavVertex nv;
                    nv.position = {v[0], v[1], v[2]};
                    navMesh->vertices.push_back(nv);
                }

                NavTriangle tri{};
                tri.indices[0] = baseIdx;
                tri.indices[1] = baseIdx + 1;
                tri.indices[2] = baseIdx + 2;
                tri.neighborTriangles[0] = UINT32_MAX;
                tri.neighborTriangles[1] = UINT32_MAX;
                tri.neighborTriangles[2] = UINT32_MAX;

                const auto& v0 = navMesh->vertices[baseIdx].position;
                const auto& v1 = navMesh->vertices[baseIdx + 1].position;
                const auto& v2 = navMesh->vertices[baseIdx + 2].position;

                tri.centroid.x = (v0.x + v1.x + v2.x) / 3.0f;
                tri.centroid.y = (v0.y + v1.y + v2.y) / 3.0f;
                tri.centroid.z = (v0.z + v1.z + v2.z) / 3.0f;

                // Compute normal
                float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
                float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
                float nx = e1y * e2z - e1z * e2y;
                float ny = e1z * e2x - e1x * e2z;
                float nz = e1x * e2y - e1y * e2x;
                float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 0.0001f)
                {
                    tri.normal = {nx / len, ny / len, nz / len};
                }
                else
                {
                    tri.normal = {0.0f, 1.0f, 0.0f};
                }

                // Area from cross product
                tri.area = len * 0.5f;
                tri.flags = 0xFFFF; // All walkable

                navMesh->triangles.push_back(tri);
            }
        }

        // Build adjacency (O(n^2) but runs once at bake time)
        const size_t triCount = navMesh->triangles.size();
        for (size_t i = 0; i < triCount; ++i)
        {
            for (size_t j = i + 1; j < triCount; ++j)
            {
                int shared = 0;
                for (int ei = 0; ei < 3; ++ei)
                {
                    for (int ej = 0; ej < 3; ++ej)
                    {
                        const auto& vi = navMesh->vertices[navMesh->triangles[i].indices[ei]].position;
                        const auto& vj = navMesh->vertices[navMesh->triangles[j].indices[ej]].position;
                        float dx = vi.x - vj.x, dy = vi.y - vj.y, dz = vi.z - vj.z;
                        if (dx * dx + dy * dy + dz * dz < 0.001f)
                            shared++;
                    }
                }
                if (shared >= 2)
                {
                    // Find empty neighbor slot
                    for (int e = 0; e < 3; ++e)
                    {
                        if (navMesh->triangles[i].neighborTriangles[e] == UINT32_MAX)
                        {
                            navMesh->triangles[i].neighborTriangles[e] = static_cast<uint32_t>(j);
                            break;
                        }
                    }
                    for (int e = 0; e < 3; ++e)
                    {
                        if (navMesh->triangles[j].neighborTriangles[e] == UINT32_MAX)
                        {
                            navMesh->triangles[j].neighborTriangles[e] = static_cast<uint32_t>(i);
                            break;
                        }
                    }
                }
            }
        }

        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);

        SPARK_LOG_INFO(Spark::LogCategory::AI, "NavMesh conversion: %zu triangles, %zu vertices",
                       navMesh->triangles.size(), navMesh->vertices.size());

        return navMesh;
    }

} // namespace Spark::AI

#else // !SPARK_RECAST_AVAILABLE

namespace Spark::AI
{

    bool IsRecastAvailable()
    {
        return false;
    }

    std::unique_ptr<NavMeshData> BuildNavMeshWithRecast(const std::vector<XMFLOAT3>& /*vertices*/,
                                                        const std::vector<uint32_t>& /*indices*/,
                                                        const NavMeshBuildSettings& /*settings*/)
    {
        return nullptr;
    }

} // namespace Spark::AI

#endif // SPARK_RECAST_AVAILABLE
