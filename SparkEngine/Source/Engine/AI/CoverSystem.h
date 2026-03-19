/**
 * @file CoverSystem.h
 * @brief CryEngine-inspired cover analysis and querying for AI agents
 * @author Spark Engine Team
 * @date 2026
 *
 * Analyzes static level geometry to identify cover positions and provides
 * fast queries for AI agents seeking protection from threats. Each cover point
 * records height (crouch/stand), width, lean capabilities, and fire-over ability.
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
    // Cover Types
    // ============================================================================

    /**
     * @brief Whether a cover point requires crouching or allows standing.
     */
    enum class CoverHeight : uint8_t
    {
        Low, ///< Agent must crouch to stay protected.
        High ///< Agent can stand behind this cover.
    };

    // ============================================================================
    // CoverPoint — a single analyzed cover position
    // ============================================================================

    /**
     * @brief A cover position with protection metadata and occupancy state.
     */
    struct CoverPoint
    {
        uint32_t id = 0;                     ///< Unique identifier.
        XMFLOAT3 position{0.0f, 0.0f, 0.0f}; ///< World-space position at the base of cover.
        XMFLOAT3 normal{0.0f, 0.0f, 1.0f};   ///< Direction the cover faces (away from wall).
        CoverHeight height = CoverHeight::High;
        float width = 1.0f;          ///< Approximate cover width in world units.
        bool canLeanLeft = false;    ///< Agent can lean left to fire around cover.
        bool canLeanRight = false;   ///< Agent can lean right to fire around cover.
        bool canFireOver = false;    ///< Agent can fire over the top (low cover only).
        bool occupied = false;       ///< Whether an agent currently claims this point.
        uint32_t occupantEntity = 0; ///< Entity ID of the occupying agent.
    };

    // ============================================================================
    // CoverSystem — singleton cover query system
    // ============================================================================

    /**
     * @brief Manages cover analysis and provides fast cover queries for AI.
     *
     * Cover points can be registered manually or generated from static geometry
     * via AnalyzeStaticGeometry(). Queries use spatial bucketing for efficient
     * radius searches.
     */
    class CoverSystem
    {
      public:
        static CoverSystem& GetInstance()
        {
            static CoverSystem s;
            return s;
        }

        /** @brief Initialize the system, clearing all cover data. */
        void Initialize();

        /** @brief Shut down and release all cover data. */
        void Shutdown();

        // -- Geometry analysis --

        /**
         * @brief Analyze static geometry to automatically generate cover points.
         *
         * Processes a set of positions and normals representing level surfaces.
         * Identifies positions where the surface normal indicates a wall face
         * and creates cover points at the base of those walls.
         *
         * @param positions  World-space surface sample positions.
         * @param normals    Surface normals corresponding to each position.
         */
        void AnalyzeStaticGeometry(const std::vector<XMFLOAT3>& positions, const std::vector<XMFLOAT3>& normals);

        // -- Point management --

        /**
         * @brief Manually register a cover point.
         * @param point The cover point to add (id is assigned automatically).
         * @return The assigned cover point ID.
         */
        uint32_t RegisterCoverPoint(const CoverPoint& point);

        // -- Queries --

        /**
         * @brief Find the nearest cover point to a position.
         * @param position  Agent's current world position.
         * @param threatDir Normalized direction toward the threat.
         * @param radius    Maximum search distance.
         * @return Pointer to the nearest qualifying cover point, or nullptr.
         */
        const CoverPoint* FindNearestCover(const XMFLOAT3& position, const XMFLOAT3& threatDir, float radius) const;

        /**
         * @brief Find cover points that protect from a specific threat position.
         * @param agentPos   Agent's current world position.
         * @param threatPos  Threat's world position.
         * @param radius     Maximum search distance from the agent.
         * @param maxResults Maximum number of results to return.
         * @return Vector of pointers to qualifying cover points, nearest first.
         */
        std::vector<const CoverPoint*> FindCoverFromThreat(const XMFLOAT3& agentPos, const XMFLOAT3& threatPos,
                                                           float radius, uint32_t maxResults) const;

        // -- Occupancy --

        /** @brief Mark a cover point as occupied by an entity. */
        void MarkOccupied(uint32_t coverID, uint32_t entityID);

        /** @brief Release a cover point for use by other agents. */
        void MarkFree(uint32_t coverID);

        /** @brief Get the total number of registered cover points. */
        uint32_t GetCoverCount() const { return static_cast<uint32_t>(m_points.size()); }

      private:
        CoverSystem() = default;
        ~CoverSystem() = default;

        CoverSystem(const CoverSystem&) = delete;
        CoverSystem& operator=(const CoverSystem&) = delete;

        /** @brief Compute squared distance between two positions. */
        static float DistanceSquared(const XMFLOAT3& a, const XMFLOAT3& b);

        /**
         * @brief Check if a cover point's normal faces away from the threat.
         * @return true if the cover provides protection from the threat direction.
         */
        static bool ProvidesCoverFrom(const CoverPoint& cover, const XMFLOAT3& threatDir);

        /// All registered cover points, keyed by ID.
        std::unordered_map<uint32_t, CoverPoint> m_points;

        /// Next auto-assigned cover point ID.
        uint32_t m_nextID = 1;

        /// Spatial bucket cell size.
        static constexpr float BUCKET_SIZE = 16.0f;

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

        static BucketKey ToBucket(const XMFLOAT3& pos);

        /// Spatial index: bucket -> list of cover point IDs.
        std::unordered_map<BucketKey, std::vector<uint32_t>, BucketHash> m_buckets;
    };

} // namespace Spark::AI
