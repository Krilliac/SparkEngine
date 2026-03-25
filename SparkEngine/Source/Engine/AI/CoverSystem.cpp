/**
 * @file CoverSystem.cpp
 * @brief Cover analysis and querying implementation
 */

#include "CoverSystem.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>

namespace Spark::AI
{

    // ============================================================================
    // Initialization / Shutdown
    // ============================================================================

    void CoverSystem::Initialize()
    {
        m_points.clear();
        m_buckets.clear();
        m_nextID = 1;
    }

    void CoverSystem::Shutdown()
    {
        m_points.clear();
        m_buckets.clear();
        m_nextID = 1;
    }

    // ============================================================================
    // Geometry Analysis
    // ============================================================================

    void CoverSystem::AnalyzeStaticGeometry(const std::vector<XMFLOAT3>& positions,
                                            const std::vector<XMFLOAT3>& normals)
    {
        if (positions.size() != normals.size())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::AI,
                            "CoverSystem: geometry analysis failed — position count (%zu) != normal count (%zu)",
                            positions.size(), normals.size());
            return;
        }

        // Wall-face threshold: normals with a strong horizontal component
        // and weak vertical component indicate vertical surfaces (walls).
        constexpr float WALL_NORMAL_Y_THRESHOLD = 0.3f;
        constexpr float MIN_HORIZONTAL_COMPONENT = 0.7f;

        // Default cover properties for auto-generated points
        constexpr float DEFAULT_COVER_WIDTH = 1.5f;
        constexpr float LOW_COVER_HEIGHT_THRESHOLD = 1.2f;
        constexpr float HIGH_COVER_HEIGHT_THRESHOLD = 1.8f;

        for (size_t i = 0; i < positions.size(); ++i)
        {
            const XMFLOAT3& pos = positions[i];
            const XMFLOAT3& nrm = normals[i];

            // Check if this surface is roughly vertical (a wall)
            float absY = std::abs(nrm.y);
            if (absY > WALL_NORMAL_Y_THRESHOLD)
            {
                continue;
            }

            float horizontalLen = std::sqrt(nrm.x * nrm.x + nrm.z * nrm.z);
            if (horizontalLen < MIN_HORIZONTAL_COMPONENT)
            {
                continue;
            }

            // Create a cover point offset slightly from the wall surface
            CoverPoint cover;
            cover.position.x = pos.x + nrm.x * 0.3f;
            cover.position.y = pos.y;
            cover.position.z = pos.z + nrm.z * 0.3f;
            cover.normal = nrm;
            cover.width = DEFAULT_COVER_WIDTH;

            // Determine cover height from Y position heuristic
            // Low cover: below standing eye height; High cover: above it
            if (pos.y < LOW_COVER_HEIGHT_THRESHOLD)
            {
                cover.height = CoverHeight::Low;
                cover.canFireOver = true;
                cover.canLeanLeft = true;
                cover.canLeanRight = true;
            }
            else if (pos.y < HIGH_COVER_HEIGHT_THRESHOLD)
            {
                cover.height = CoverHeight::High;
                cover.canFireOver = false;
                cover.canLeanLeft = true;
                cover.canLeanRight = true;
            }
            else
            {
                // Very tall wall — high cover, limited lean
                cover.height = CoverHeight::High;
                cover.canFireOver = false;
                cover.canLeanLeft = false;
                cover.canLeanRight = false;
            }

            RegisterCoverPoint(cover);
        }

        SPARK_LOG_INFO(Spark::LogCategory::AI,
                       "CoverSystem: geometry analysis complete — %zu surfaces analyzed, %zu total cover points",
                       positions.size(), m_points.size());
    }

    // ============================================================================
    // Point Management
    // ============================================================================

    uint32_t CoverSystem::RegisterCoverPoint(const CoverPoint& point)
    {
        uint32_t id = m_nextID++;
        CoverPoint stored = point;
        stored.id = id;

        BucketKey key = ToBucket(stored.position);
        m_buckets[key].push_back(id);
        m_points.emplace(id, std::move(stored));

        SPARK_LOG_DEBUG(Spark::LogCategory::AI, "CoverSystem: registered cover point %u at (%.1f, %.1f, %.1f)", id,
                        point.position.x, point.position.y, point.position.z);
        return id;
    }

    // ============================================================================
    // Queries
    // ============================================================================

    const CoverPoint* CoverSystem::FindNearestCover(const XMFLOAT3& position, const XMFLOAT3& threatDir,
                                                    float radius) const
    {
        float radiusSq = radius * radius;
        float bestDistSq = radiusSq + 1.0f;
        const CoverPoint* bestCover = nullptr;

        BucketKey minBucket = ToBucket(XMFLOAT3{position.x - radius, 0.0f, position.z - radius});
        BucketKey maxBucket = ToBucket(XMFLOAT3{position.x + radius, 0.0f, position.z + radius});

        for (int32_t bx = minBucket.x; bx <= maxBucket.x; ++bx)
        {
            for (int32_t bz = minBucket.z; bz <= maxBucket.z; ++bz)
            {
                auto bucketIt = m_buckets.find(BucketKey{bx, bz});
                if (bucketIt == m_buckets.end())
                {
                    continue;
                }

                for (uint32_t coverID : bucketIt->second)
                {
                    auto pointIt = m_points.find(coverID);
                    if (pointIt == m_points.end())
                    {
                        continue;
                    }

                    const CoverPoint& cp = pointIt->second;

                    if (cp.occupied)
                    {
                        continue;
                    }

                    float distSq = DistanceSquared(position, cp.position);
                    if (distSq > radiusSq || distSq >= bestDistSq)
                    {
                        continue;
                    }

                    if (!ProvidesCoverFrom(cp, threatDir))
                    {
                        continue;
                    }

                    bestDistSq = distSq;
                    bestCover = &cp;
                }
            }
        }

        return bestCover;
    }

    std::vector<const CoverPoint*> CoverSystem::FindCoverFromThreat(const XMFLOAT3& agentPos, const XMFLOAT3& threatPos,
                                                                    float radius, uint32_t maxResults) const
    {
        if (maxResults == 0)
        {
            return {};
        }

        // Compute threat direction from agent to threat
        float threatDirX = threatPos.x - agentPos.x;
        float threatDirZ = threatPos.z - agentPos.z;
        float threatLen = std::sqrt(threatDirX * threatDirX + threatDirZ * threatDirZ);
        XMFLOAT3 threatDir{0.0f, 0.0f, 1.0f};
        if (threatLen > 0.001f)
        {
            threatDir.x = threatDirX / threatLen;
            threatDir.z = threatDirZ / threatLen;
        }

        float radiusSq = radius * radius;

        struct ScoredCover
        {
            const CoverPoint* point;
            float distSq;
        };
        std::vector<ScoredCover> candidates;

        BucketKey minBucket = ToBucket(XMFLOAT3{agentPos.x - radius, 0.0f, agentPos.z - radius});
        BucketKey maxBucket = ToBucket(XMFLOAT3{agentPos.x + radius, 0.0f, agentPos.z + radius});

        for (int32_t bx = minBucket.x; bx <= maxBucket.x; ++bx)
        {
            for (int32_t bz = minBucket.z; bz <= maxBucket.z; ++bz)
            {
                auto bucketIt = m_buckets.find(BucketKey{bx, bz});
                if (bucketIt == m_buckets.end())
                {
                    continue;
                }

                for (uint32_t coverID : bucketIt->second)
                {
                    auto pointIt = m_points.find(coverID);
                    if (pointIt == m_points.end())
                    {
                        continue;
                    }

                    const CoverPoint& cp = pointIt->second;

                    if (cp.occupied)
                    {
                        continue;
                    }

                    float distSq = DistanceSquared(agentPos, cp.position);
                    if (distSq > radiusSq)
                    {
                        continue;
                    }

                    if (!ProvidesCoverFrom(cp, threatDir))
                    {
                        continue;
                    }

                    candidates.push_back(ScoredCover{&cp, distSq});
                }
            }
        }

        // Sort by distance (nearest first)
        std::sort(candidates.begin(), candidates.end(),
                  [](const ScoredCover& a, const ScoredCover& b) { return a.distSq < b.distSq; });

        uint32_t resultCount = std::min(maxResults, static_cast<uint32_t>(candidates.size()));
        std::vector<const CoverPoint*> results;
        results.reserve(resultCount);
        for (uint32_t i = 0; i < resultCount; ++i)
        {
            results.push_back(candidates[i].point);
        }

        return results;
    }

    // ============================================================================
    // Occupancy
    // ============================================================================

    void CoverSystem::MarkOccupied(uint32_t coverID, uint32_t entityID)
    {
        auto it = m_points.find(coverID);
        if (it != m_points.end())
        {
            it->second.occupied = true;
            it->second.occupantEntity = entityID;
        }
    }

    void CoverSystem::MarkFree(uint32_t coverID)
    {
        auto it = m_points.find(coverID);
        if (it != m_points.end())
        {
            it->second.occupied = false;
            it->second.occupantEntity = 0;
        }
    }

    // ============================================================================
    // Utilities
    // ============================================================================

    float CoverSystem::DistanceSquared(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    bool CoverSystem::ProvidesCoverFrom(const CoverPoint& cover, const XMFLOAT3& threatDir)
    {
        // The cover's normal points away from the wall. For the cover to protect
        // against a threat, the threat direction should be roughly opposite to the
        // cover normal (i.e., the wall is between the agent and the threat).
        // Negative dot product means the wall faces the threat.
        float dot = cover.normal.x * threatDir.x + cover.normal.z * threatDir.z;
        return dot < 0.0f;
    }

    CoverSystem::BucketKey CoverSystem::ToBucket(const XMFLOAT3& pos)
    {
        return BucketKey{static_cast<int32_t>(std::floor(pos.x / BUCKET_SIZE)),
                         static_cast<int32_t>(std::floor(pos.z / BUCKET_SIZE))};
    }

} // namespace Spark::AI
