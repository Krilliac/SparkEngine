/**
 * @file TacticalPointSystem.h
 * @brief Tactical point evaluation for AI combat positioning
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a spatial database of tactical points (cover, vantage, ambush, etc.)
 * that AI agents can query to find optimal combat positions. Points are scored
 * by quality, distance, and threat direction, then returned in priority order.
 *
 * Uses spatial bucketing (cell-based grid) for efficient radius queries so that
 * searches scale with the query area, not the total point count.
 *
 * @threadsafety Main thread only.
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Spark::AI
{

    // ============================================================================
    // Tactical Point Types
    // ============================================================================

    /**
     * @brief Classification of a tactical position's strategic purpose.
     */
    enum class TacticalPointType : uint8_t
    {
        Cover,   ///< Position provides protection from incoming fire.
        Vantage, ///< Elevated position with good line-of-sight.
        Ambush,  ///< Concealed position suitable for surprising enemies.
        Flank,   ///< Position enabling attack from the side or rear.
        Retreat, ///< Safe fallback position for withdrawing agents.
        Rally    ///< Gathering point for group coordination.
    };

    // ============================================================================
    // TacticalPoint — a single scored position in the world
    // ============================================================================

    /**
     * @brief A world-space position with tactical metadata and occupancy tracking.
     */
    struct TacticalPoint
    {
        uint32_t id = 0;                     ///< Unique identifier for this point.
        XMFLOAT3 position{0.0f, 0.0f, 0.0f}; ///< World-space location.
        TacticalPointType type = TacticalPointType::Cover;
        float quality = 0.5f;              ///< Base quality score (0.0–1.0).
        XMFLOAT3 normal{0.0f, 0.0f, 1.0f}; ///< Surface normal (cover direction).
        bool occupied = false;             ///< Whether an agent currently claims this point.
        uint32_t occupantEntity = 0;       ///< Entity ID of the occupying agent.
    };

    // ============================================================================
    // TacticalQuery — parameters for finding tactical points
    // ============================================================================

    /**
     * @brief Describes what kind of tactical point an agent is looking for.
     */
    struct TacticalQuery
    {
        XMFLOAT3 queryOrigin{0.0f, 0.0f, 0.0f};    ///< Agent's current position.
        XMFLOAT3 threatPosition{0.0f, 0.0f, 0.0f}; ///< Primary threat location.
        float searchRadius = 20.0f;                ///< Maximum distance to search.
        TacticalPointType preferredType = TacticalPointType::Cover;
        float minQuality = 0.3f;       ///< Minimum acceptable quality score.
        bool requireUnoccupied = true; ///< Skip points claimed by other agents.
    };

    // ============================================================================
    // TacticalPointSystem — singleton spatial query system
    // ============================================================================

    /**
     * @brief Manages a database of tactical points with spatial bucketing.
     *
     * Points are stored in a flat vector and indexed into spatial buckets
     * (grid cells) for fast radius queries. AI agents query the system each
     * frame to find optimal combat positions based on threat direction,
     * point quality, and occupancy.
     */
    class TacticalPointSystem
    {
      public:
        static TacticalPointSystem& GetInstance()
        {
            static TacticalPointSystem s;
            return s;
        }

        /** @brief Initialize the system, clearing all points and buckets. */
        void Initialize();

        /** @brief Shut down and release all data. */
        void Shutdown();

        // -- Point management --

        /**
         * @brief Register a new tactical point.
         * @param point The tactical point to add (id is assigned automatically).
         * @return The assigned point ID.
         */
        uint32_t RegisterPoint(const TacticalPoint& point);

        /**
         * @brief Remove a tactical point by ID.
         * @param pointID The point to remove.
         */
        void RemovePoint(uint32_t pointID);

        /**
         * @brief Update the quality score of an existing point.
         * @param pointID The point to update.
         * @param newQuality New quality value (clamped to 0.0–1.0).
         */
        void UpdatePointQuality(uint32_t pointID, float newQuality);

        // -- Queries --

        /**
         * @brief Find the single best tactical point matching a query.
         * @param query Search parameters.
         * @return Pointer to the best point, or nullptr if none qualifies.
         */
        const TacticalPoint* FindBestPoint(const TacticalQuery& query) const;

        /**
         * @brief Find multiple tactical points matching a query, sorted by score.
         * @param query      Search parameters.
         * @param maxResults Maximum number of results to return.
         * @return Vector of pointers to qualifying points, best first.
         */
        std::vector<const TacticalPoint*> FindPoints(const TacticalQuery& query, uint32_t maxResults) const;

        // -- Occupancy --

        /**
         * @brief Mark a point as occupied by an entity.
         * @param pointID The point to claim.
         * @param entityID The claiming entity.
         */
        void MarkOccupied(uint32_t pointID, uint32_t entityID);

        /**
         * @brief Release a point so other agents can claim it.
         * @param pointID The point to free.
         */
        void MarkFree(uint32_t pointID);

        /** @brief Get the total number of registered points. */
        uint32_t GetPointCount() const { return static_cast<uint32_t>(m_points.size()); }

      private:
        TacticalPointSystem() = default;
        ~TacticalPointSystem() = default;

        TacticalPointSystem(const TacticalPointSystem&) = delete;
        TacticalPointSystem& operator=(const TacticalPointSystem&) = delete;

        /**
         * @brief Score a tactical point relative to a query.
         *
         * Combines base quality, distance penalty, type preference, and
         * threat-direction alignment into a single score.
         */
        float ScorePoint(const TacticalPoint& point, const TacticalQuery& query) const;

        /** @brief Compute squared distance between two XMFLOAT3 positions. */
        static float DistanceSquared(const XMFLOAT3& a, const XMFLOAT3& b);

        /// All registered tactical points, keyed by ID.
        std::unordered_map<uint32_t, TacticalPoint> m_points;

        /// Next auto-assigned point ID.
        uint32_t m_nextID = 1;

        /// Spatial bucket cell size in world units.
        static constexpr float BUCKET_SIZE = 16.0f;

        /** @brief Bucket key from world XZ. */
        struct BucketKey
        {
            int32_t x = 0;
            int32_t z = 0;
            bool operator==(const BucketKey& other) const { return x == other.x && z == other.z; }
        };

        struct BucketHash
        {
            size_t operator()(const BucketKey& k) const
            {
                size_t h = std::hash<int32_t>{}(k.x);
                h ^= std::hash<int32_t>{}(k.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        /** @brief Convert a world position to a bucket key. */
        static BucketKey ToBucket(const XMFLOAT3& pos);

        /** @brief Rebuild the spatial index from m_points. */
        void RebuildBuckets();

        /// Spatial index: bucket → list of point IDs in that bucket.
        std::unordered_map<BucketKey, std::vector<uint32_t>, BucketHash> m_buckets;
    };

} // namespace Spark::AI
