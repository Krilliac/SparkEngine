/**
 * @file EnvironmentQuery.h
 * @brief Environment Query System (EQS) for tactical AI positioning
 *
 * Provides a generator/test/scorer pipeline for evaluating spatial positions.
 * Agents use EQS to find cover, flanking positions, vantage points, and other
 * tactically advantageous locations.
 *
 * ## Usage
 * @code
 *   EQSQuery query;
 *   query.AddGenerator(std::make_unique<GridGenerator>(center, 15.0f, 2.0f));
 *   query.AddTest(std::make_unique<LineOfSightTest>(threatPos, false));  // prefer NOT visible
 *   query.AddTest(std::make_unique<DistanceTest>(agentPos, 5.0f, 15.0f));
 *   query.AddScorer(std::make_unique<DistanceScorer>(goalPos, 1.0f));
 *
 *   EQSResult result = query.Execute();
 *   if (result.HasValidItem())
 *       agent.MoveTo(result.GetBestItem().position);
 * @endcode
 */

#pragma once

#include "../../Core/Platform.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Spark::AI
{

    // ============================================================================
    // EQS Item — a candidate position to evaluate
    // ============================================================================

    struct EQSItem
    {
        XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
        float score = 0.0f;          ///< Accumulated weighted score
        bool filtered = false;       ///< If true, excluded by a test
        uint32_t sourceEntityId = 0; ///< Optional entity associated with this point
    };

    // ============================================================================
    // Generators — produce candidate positions
    // ============================================================================

    /**
 * @brief Base class for EQS generators that produce candidate positions
 */
    class EQSGenerator
    {
      public:
        virtual ~EQSGenerator() = default;
        virtual std::vector<EQSItem> Generate() const = 0;
        virtual std::string GetName() const = 0;
    };

    /**
 * @brief Generates a grid of points around a center position
 */
    class GridGenerator : public EQSGenerator
    {
      public:
        GridGenerator(XMFLOAT3 center, float radius, float spacing)
            : m_center(center), m_radius(radius), m_spacing(spacing)
        {
        }

        std::vector<EQSItem> Generate() const override
        {
            std::vector<EQSItem> items;
            float radiusSq = m_radius * m_radius;

            int steps = static_cast<int>(m_radius / m_spacing);
            for (int x = -steps; x <= steps; ++x)
            {
                for (int z = -steps; z <= steps; ++z)
                {
                    float px = m_center.x + static_cast<float>(x) * m_spacing;
                    float pz = m_center.z + static_cast<float>(z) * m_spacing;

                    float dx = px - m_center.x;
                    float dz = pz - m_center.z;
                    if (dx * dx + dz * dz <= radiusSq)
                    {
                        EQSItem item;
                        item.position = {px, m_center.y, pz};
                        items.push_back(item);
                    }
                }
            }
            return items;
        }

        std::string GetName() const override { return "Grid"; }

      private:
        XMFLOAT3 m_center;
        float m_radius;
        float m_spacing;
    };

    /**
 * @brief Generates points in a ring (donut) around a center position
 */
    class RingGenerator : public EQSGenerator
    {
      public:
        RingGenerator(XMFLOAT3 center, float innerRadius, float outerRadius, int numPoints)
            : m_center(center), m_innerRadius(innerRadius), m_outerRadius(outerRadius), m_numPoints(numPoints)
        {
        }

        std::vector<EQSItem> Generate() const override
        {
            std::vector<EQSItem> items;
            const float pi2 = 6.28318530718f;

            // Generate points at multiple radii within the ring
            int numRings = std::max(1, static_cast<int>((m_outerRadius - m_innerRadius) / 2.0f));
            int pointsPerRing = m_numPoints / numRings;

            for (int r = 0; r < numRings; ++r)
            {
                float t = numRings > 1 ? static_cast<float>(r) / static_cast<float>(numRings - 1) : 0.5f;
                float radius = m_innerRadius + t * (m_outerRadius - m_innerRadius);

                for (int i = 0; i < pointsPerRing; ++i)
                {
                    float angle = pi2 * static_cast<float>(i) / static_cast<float>(pointsPerRing);
                    EQSItem item;
                    item.position = {m_center.x + radius * std::cos(angle), m_center.y,
                                     m_center.z + radius * std::sin(angle)};
                    items.push_back(item);
                }
            }
            return items;
        }

        std::string GetName() const override { return "Ring"; }

      private:
        XMFLOAT3 m_center;
        float m_innerRadius;
        float m_outerRadius;
        int m_numPoints;
    };

    /**
 * @brief Generates points along a cone (arc) from agent toward a direction
 */
    class ConeGenerator : public EQSGenerator
    {
      public:
        ConeGenerator(XMFLOAT3 origin, XMFLOAT3 direction, float distance, float halfAngleDeg, int numPoints)
            : m_origin(origin), m_direction(direction), m_distance(distance),
              m_halfAngleRad(halfAngleDeg * 3.14159265f / 180.0f), m_numPoints(numPoints)
        {
        }

        std::vector<EQSItem> Generate() const override
        {
            std::vector<EQSItem> items;

            // Normalize direction
            float len = std::sqrt(m_direction.x * m_direction.x + m_direction.z * m_direction.z);
            if (len < 0.001f)
                return items;

            float baseAngle = std::atan2(m_direction.z, m_direction.x);

            int numDist = std::max(2, m_numPoints / 4);
            int numAngles = m_numPoints / numDist;

            for (int d = 1; d <= numDist; ++d)
            {
                float dist = m_distance * static_cast<float>(d) / static_cast<float>(numDist);
                for (int a = 0; a < numAngles; ++a)
                {
                    float t = numAngles > 1 ? static_cast<float>(a) / static_cast<float>(numAngles - 1) : 0.5f;
                    float angle = baseAngle + m_halfAngleRad * (2.0f * t - 1.0f);

                    EQSItem item;
                    item.position = {m_origin.x + dist * std::cos(angle), m_origin.y,
                                     m_origin.z + dist * std::sin(angle)};
                    items.push_back(item);
                }
            }
            return items;
        }

        std::string GetName() const override { return "Cone"; }

      private:
        XMFLOAT3 m_origin;
        XMFLOAT3 m_direction;
        float m_distance;
        float m_halfAngleRad;
        int m_numPoints;
    };

    // ============================================================================
    // Tests — filter out invalid positions
    // ============================================================================

    /**
 * @brief Base class for EQS tests that filter candidate positions
 */
    class EQSTest
    {
      public:
        virtual ~EQSTest() = default;

        /** @return true if the item passes the test (should be kept) */
        virtual bool Test(const EQSItem& item) const = 0;
        virtual std::string GetName() const = 0;
    };

    /**
 * @brief Filters items by distance to a reference point
 */
    class DistanceTest : public EQSTest
    {
      public:
        DistanceTest(XMFLOAT3 reference, float minDist, float maxDist)
            : m_reference(reference), m_minDist(minDist), m_maxDist(maxDist)
        {
        }

        bool Test(const EQSItem& item) const override
        {
            float dx = item.position.x - m_reference.x;
            float dy = item.position.y - m_reference.y;
            float dz = item.position.z - m_reference.z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            return dist >= m_minDist && dist <= m_maxDist;
        }

        std::string GetName() const override { return "Distance"; }

      private:
        XMFLOAT3 m_reference;
        float m_minDist;
        float m_maxDist;
    };

    /**
 * @brief Geometric line-of-sight test (no raycasting — uses distance + direction)
 *
 * Approximates whether a point is "behind cover" relative to a threat.
 * For actual raycasting, use NavMesh::Raycast or physics raycasts.
 */
    class CoverTest : public EQSTest
    {
      public:
        /**
     * @param threatPos    Position of the threat
     * @param coverRadius  Minimum distance between item and threat's line to agent
     * @param wantCover    true = keep points IN cover, false = keep exposed points
     */
        CoverTest(XMFLOAT3 threatPos, XMFLOAT3 agentPos, float coverRadius, bool wantCover = true)
            : m_threatPos(threatPos), m_agentPos(agentPos), m_coverRadius(coverRadius), m_wantCover(wantCover)
        {
        }

        bool Test(const EQSItem& item) const override
        {
            // Check if the item is behind cover: item should be farther from the threat
            // than the agent's current position, and off to the side of the threat-agent line
            float threatToItemX = item.position.x - m_threatPos.x;
            float threatToItemZ = item.position.z - m_threatPos.z;
            float threatToAgentX = m_agentPos.x - m_threatPos.x;
            float threatToAgentZ = m_agentPos.z - m_threatPos.z;

            // Project item onto the threat-agent line to find perpendicular distance
            float lineLen = std::sqrt(threatToAgentX * threatToAgentX + threatToAgentZ * threatToAgentZ);
            if (lineLen < 0.001f)
                return !m_wantCover;

            float dot = (threatToItemX * threatToAgentX + threatToItemZ * threatToAgentZ) / lineLen;
            float projX = m_threatPos.x + (threatToAgentX / lineLen) * dot;
            float projZ = m_threatPos.z + (threatToAgentZ / lineLen) * dot;

            float perpDistX = item.position.x - projX;
            float perpDistZ = item.position.z - projZ;
            float perpDist = std::sqrt(perpDistX * perpDistX + perpDistZ * perpDistZ);

            bool isCovered = perpDist >= m_coverRadius;
            return m_wantCover ? isCovered : !isCovered;
        }

        std::string GetName() const override { return "Cover"; }

      private:
        XMFLOAT3 m_threatPos;
        XMFLOAT3 m_agentPos;
        float m_coverRadius;
        bool m_wantCover;
    };

    /**
 * @brief Custom predicate test
 */
    class PredicateTest : public EQSTest
    {
      public:
        PredicateTest(std::string name, std::function<bool(const EQSItem&)> predicate)
            : m_name(std::move(name)), m_predicate(std::move(predicate))
        {
        }

        bool Test(const EQSItem& item) const override { return m_predicate(item); }
        std::string GetName() const override { return m_name; }

      private:
        std::string m_name;
        std::function<bool(const EQSItem&)> m_predicate;
    };

    // ============================================================================
    // Scorers — assign weighted scores to remaining items
    // ============================================================================

    /**
 * @brief Base class for EQS scorers that assign weighted scores
 */
    class EQSScorer
    {
      public:
        virtual ~EQSScorer() = default;

        /** @return Score in [0, 1] range — higher is better */
        virtual float Score(const EQSItem& item) const = 0;
        virtual std::string GetName() const = 0;

        float GetWeight() const { return m_weight; }
        void SetWeight(float w) { m_weight = w; }

      protected:
        float m_weight = 1.0f;
    };

    /**
 * @brief Scores by distance to a reference point (closer = higher score)
 */
    class DistanceScorer : public EQSScorer
    {
      public:
        DistanceScorer(XMFLOAT3 reference, float maxDistance, bool preferCloser = true)
            : m_reference(reference), m_maxDistance(maxDistance), m_preferCloser(preferCloser)
        {
        }

        float Score(const EQSItem& item) const override
        {
            float dx = item.position.x - m_reference.x;
            float dy = item.position.y - m_reference.y;
            float dz = item.position.z - m_reference.z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            float normalized = std::min(dist / m_maxDistance, 1.0f);
            return m_preferCloser ? (1.0f - normalized) : normalized;
        }

        std::string GetName() const override { return "Distance"; }

      private:
        XMFLOAT3 m_reference;
        float m_maxDistance;
        bool m_preferCloser;
    };

    /**
 * @brief Scores by dot product with a preferred direction (alignment)
 */
    class DirectionScorer : public EQSScorer
    {
      public:
        DirectionScorer(XMFLOAT3 origin, XMFLOAT3 preferredDir) : m_origin(origin), m_preferredDir(preferredDir)
        {
            // Normalize preferred direction
            float len = std::sqrt(m_preferredDir.x * m_preferredDir.x + m_preferredDir.y * m_preferredDir.y +
                                  m_preferredDir.z * m_preferredDir.z);
            if (len > 0.001f)
            {
                m_preferredDir.x /= len;
                m_preferredDir.y /= len;
                m_preferredDir.z /= len;
            }
        }

        float Score(const EQSItem& item) const override
        {
            float dx = item.position.x - m_origin.x;
            float dy = item.position.y - m_origin.y;
            float dz = item.position.z - m_origin.z;
            float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len < 0.001f)
                return 0.0f;

            float dot = (dx * m_preferredDir.x + dy * m_preferredDir.y + dz * m_preferredDir.z) / len;
            return (dot + 1.0f) * 0.5f; // Map [-1, 1] to [0, 1]
        }

        std::string GetName() const override { return "Direction"; }

      private:
        XMFLOAT3 m_origin;
        XMFLOAT3 m_preferredDir;
    };

    /**
 * @brief Custom scoring function
 */
    class PredicateScorer : public EQSScorer
    {
      public:
        PredicateScorer(std::string name, float weight, std::function<float(const EQSItem&)> scoreFn)
            : m_name(std::move(name)), m_scoreFn(std::move(scoreFn))
        {
            m_weight = weight;
        }

        float Score(const EQSItem& item) const override { return std::clamp(m_scoreFn(item), 0.0f, 1.0f); }
        std::string GetName() const override { return m_name; }

      private:
        std::string m_name;
        std::function<float(const EQSItem&)> m_scoreFn;
    };

    // ============================================================================
    // Query Result
    // ============================================================================

    struct EQSResult
    {
        std::vector<EQSItem> items; ///< All items (including filtered)
        int totalGenerated = 0;     ///< Items generated before filtering
        int totalFiltered = 0;      ///< Items that passed all tests
        float queryTimeMs = 0.0f;   ///< Execution time

        bool HasValidItem() const
        {
            return std::any_of(items.begin(), items.end(), [](const EQSItem& i) { return !i.filtered; });
        }

        EQSItem GetBestItem() const
        {
            EQSItem best;
            best.filtered = true;
            best.score = -1.0f;
            for (const auto& item : items)
            {
                if (!item.filtered && item.score > best.score)
                {
                    best = item;
                }
            }
            return best;
        }

        std::vector<EQSItem> GetTopItems(int count) const
        {
            std::vector<EQSItem> valid;
            for (const auto& item : items)
            {
                if (!item.filtered)
                {
                    valid.push_back(item);
                }
            }
            std::sort(valid.begin(), valid.end(), [](const EQSItem& a, const EQSItem& b) { return a.score > b.score; });
            if (static_cast<int>(valid.size()) > count)
            {
                valid.resize(static_cast<size_t>(count));
            }
            return valid;
        }
    };

    // ============================================================================
    // EQS Query — the main pipeline
    // ============================================================================

    /**
 * @brief Environment Query: generates candidates, filters, scores, and ranks
 */
    class EQSQuery
    {
      public:
        void AddGenerator(std::unique_ptr<EQSGenerator> gen) { m_generators.push_back(std::move(gen)); }

        void AddTest(std::unique_ptr<EQSTest> test) { m_tests.push_back(std::move(test)); }

        void AddScorer(std::unique_ptr<EQSScorer> scorer) { m_scorers.push_back(std::move(scorer)); }

        /**
     * @brief Execute the query pipeline: generate → test → score → sort
     */
        EQSResult Execute() const
        {
            auto startTime = std::chrono::high_resolution_clock::now();

            EQSResult result;

            // 1. Generate candidate positions
            for (const auto& gen : m_generators)
            {
                auto items = gen->Generate();
                result.items.insert(result.items.end(), items.begin(), items.end());
            }
            result.totalGenerated = static_cast<int>(result.items.size());

            // 2. Run tests — filter out invalid positions
            for (auto& item : result.items)
            {
                if (item.filtered)
                    continue;

                for (const auto& test : m_tests)
                {
                    if (!test->Test(item))
                    {
                        item.filtered = true;
                        break; // Early-out on first failed test
                    }
                }
            }

            // 3. Score remaining items
            float totalWeight = 0.0f;
            for (const auto& scorer : m_scorers)
            {
                totalWeight += scorer->GetWeight();
            }

            for (auto& item : result.items)
            {
                if (item.filtered)
                    continue;

                float weightedScore = 0.0f;
                for (const auto& scorer : m_scorers)
                {
                    float rawScore = scorer->Score(item);
                    weightedScore += rawScore * scorer->GetWeight();
                }

                item.score = totalWeight > 0.0f ? weightedScore / totalWeight : 0.0f;
            }

            // Count filtered
            result.totalFiltered = 0;
            for (const auto& item : result.items)
            {
                if (!item.filtered)
                {
                    result.totalFiltered++;
                }
            }

            auto endTime = std::chrono::high_resolution_clock::now();
            result.queryTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

            return result;
        }

      private:
        std::vector<std::unique_ptr<EQSGenerator>> m_generators;
        std::vector<std::unique_ptr<EQSTest>> m_tests;
        std::vector<std::unique_ptr<EQSScorer>> m_scorers;
    };

} // namespace Spark::AI
