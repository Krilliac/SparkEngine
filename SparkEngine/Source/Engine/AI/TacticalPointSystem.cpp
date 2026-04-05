/**
 * @file TacticalPointSystem.cpp
 * @brief Tactical point evaluation implementation
 */

#include "TacticalPointSystem.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>

namespace Spark::AI
{

    // ============================================================================
    // Initialization / Shutdown
    // ============================================================================

    void TacticalPointSystem::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::AI, "TacticalPointSystem::Initialize");
        m_points.clear();
        m_buckets.clear();
        m_nextID = 1;
    }

    void TacticalPointSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::AI, "TacticalPointSystem::Shutdown — %zu points", m_points.size());
        m_points.clear();
        m_buckets.clear();
        m_nextID = 1;
    }

    // ============================================================================
    // Point Management
    // ============================================================================

    uint32_t TacticalPointSystem::RegisterPoint(const TacticalPoint& point)
    {
        uint32_t id = m_nextID++;
        TacticalPoint stored = point;
        stored.id = id;

        BucketKey key = ToBucket(stored.position);
        m_buckets[key].push_back(id);
        m_points.emplace(id, std::move(stored));

        SPARK_LOG_DEBUG(Spark::LogCategory::AI, "TacticalPointSystem: registered point %u at (%.1f, %.1f, %.1f)", id,
                        point.position.x, point.position.y, point.position.z);
        return id;
    }

    void TacticalPointSystem::RemovePoint(uint32_t pointID)
    {
        auto it = m_points.find(pointID);
        if (it == m_points.end())
        {
            return;
        }

        // Remove from spatial bucket
        BucketKey key = ToBucket(it->second.position);
        auto bucketIt = m_buckets.find(key);
        if (bucketIt != m_buckets.end())
        {
            auto& ids = bucketIt->second;
            std::erase(ids, pointID);
            if (ids.empty())
            {
                m_buckets.erase(bucketIt);
            }
        }

        SPARK_LOG_DEBUG(Spark::LogCategory::AI, "TacticalPointSystem: removed point %u", pointID);
        m_points.erase(it);
    }

    void TacticalPointSystem::UpdatePointQuality(uint32_t pointID, float newQuality)
    {
        auto it = m_points.find(pointID);
        if (it != m_points.end())
        {
            it->second.quality = std::clamp(newQuality, 0.0f, 1.0f);
        }
    }

    // ============================================================================
    // Queries
    // ============================================================================

    const TacticalPoint* TacticalPointSystem::FindBestPoint(const TacticalQuery& query) const
    {
        auto results = FindPoints(query, 1);
        return results.empty() ? nullptr : results[0];
    }

    std::vector<const TacticalPoint*> TacticalPointSystem::FindPoints(const TacticalQuery& query,
                                                                      uint32_t maxResults) const
    {
        if (maxResults == 0)
        {
            return {};
        }

        float radiusSq = query.searchRadius * query.searchRadius;

        // Determine which spatial buckets overlap the search radius
        BucketKey minBucket = ToBucket(
            XMFLOAT3{query.queryOrigin.x - query.searchRadius, 0.0f, query.queryOrigin.z - query.searchRadius});
        BucketKey maxBucket = ToBucket(
            XMFLOAT3{query.queryOrigin.x + query.searchRadius, 0.0f, query.queryOrigin.z + query.searchRadius});

        // Collect candidate points with scores
        struct ScoredPoint
        {
            const TacticalPoint* point;
            float score;
        };
        std::vector<ScoredPoint> candidates;

        for (int32_t bx = minBucket.x; bx <= maxBucket.x; ++bx)
        {
            for (int32_t bz = minBucket.z; bz <= maxBucket.z; ++bz)
            {
                auto bucketIt = m_buckets.find(BucketKey{bx, bz});
                if (bucketIt == m_buckets.end())
                {
                    continue;
                }

                for (uint32_t pointID : bucketIt->second)
                {
                    auto pointIt = m_points.find(pointID);
                    if (pointIt == m_points.end())
                    {
                        continue;
                    }

                    const TacticalPoint& tp = pointIt->second;

                    // Distance check
                    if (DistanceSquared(query.queryOrigin, tp.position) > radiusSq)
                    {
                        continue;
                    }

                    // Quality filter
                    if (tp.quality < query.minQuality)
                    {
                        continue;
                    }

                    // Occupancy filter
                    if (query.requireUnoccupied && tp.occupied)
                    {
                        continue;
                    }

                    float score = ScorePoint(tp, query);
                    candidates.push_back(ScoredPoint{&tp, score});
                }
            }
        }

        // Sort by score descending (best first)
        std::sort(candidates.begin(), candidates.end(),
                  [](const ScoredPoint& a, const ScoredPoint& b) { return a.score > b.score; });

        // Build result vector
        uint32_t resultCount = std::min(maxResults, static_cast<uint32_t>(candidates.size()));
        std::vector<const TacticalPoint*> results;
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

    void TacticalPointSystem::MarkOccupied(uint32_t pointID, uint32_t entityID)
    {
        auto it = m_points.find(pointID);
        if (it != m_points.end())
        {
            it->second.occupied = true;
            it->second.occupantEntity = entityID;
        }
    }

    void TacticalPointSystem::MarkFree(uint32_t pointID)
    {
        auto it = m_points.find(pointID);
        if (it != m_points.end())
        {
            it->second.occupied = false;
            it->second.occupantEntity = 0;
        }
    }

    // ============================================================================
    // Scoring
    // ============================================================================

    float TacticalPointSystem::ScorePoint(const TacticalPoint& point, const TacticalQuery& query) const
    {
        float score = point.quality;

        // Distance penalty: closer points score higher
        float distSq = DistanceSquared(query.queryOrigin, point.position);
        float maxDistSq = query.searchRadius * query.searchRadius;
        if (maxDistSq > 0.0f)
        {
            float distFactor = 1.0f - (distSq / maxDistSq);
            score *= (0.5f + 0.5f * distFactor); // 50% base + 50% proximity bonus
        }

        // Type preference: matching the preferred type gets a bonus
        if (point.type == query.preferredType)
        {
            score *= 1.5f;
        }

        // Threat direction alignment: for cover, the point's normal should face
        // away from the threat (i.e., the point is between the agent and the threat)
        if (point.type == TacticalPointType::Cover)
        {
            float toThreatX = query.threatPosition.x - point.position.x;
            float toThreatZ = query.threatPosition.z - point.position.z;
            float threatLen = std::sqrt(toThreatX * toThreatX + toThreatZ * toThreatZ);

            if (threatLen > 0.001f)
            {
                toThreatX /= threatLen;
                toThreatZ /= threatLen;

                // Dot product: normal should point away from threat (negative dot = good cover)
                float dot = point.normal.x * toThreatX + point.normal.z * toThreatZ;
                float coverFactor = std::clamp(-dot, 0.0f, 1.0f);
                score *= (0.5f + 0.5f * coverFactor);
            }
        }

        return score;
    }

    // ============================================================================
    // Spatial Utilities
    // ============================================================================

    float TacticalPointSystem::DistanceSquared(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    TacticalPointSystem::BucketKey TacticalPointSystem::ToBucket(const XMFLOAT3& pos)
    {
        return BucketKey{static_cast<int32_t>(std::floor(pos.x / BUCKET_SIZE)),
                         static_cast<int32_t>(std::floor(pos.z / BUCKET_SIZE))};
    }

    void TacticalPointSystem::RebuildBuckets()
    {
        m_buckets.clear();
        for (const auto& [id, point] : m_points)
        {
            BucketKey key = ToBucket(point.position);
            m_buckets[key].push_back(id);
        }
    }

} // namespace Spark::AI
