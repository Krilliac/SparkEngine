#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file MeshLOD.cpp
 * @brief Mesh LOD implementation — simplification, chain generation, runtime selection
 */

#include "MeshLOD.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

#ifdef SPARK_PLATFORM_WINDOWS

namespace Spark::Graphics {

// ============================================================================
// LODChain
// ============================================================================

int LODChain::SelectLOD(float distance, float screenSize) const {
    if (levels.empty()) return 0;

    // Use distance-based selection
    for (int i = static_cast<int>(levels.size()) - 1; i >= 0; --i) {
        if (distance >= levels[i].maxDistance && levels[i].maxDistance > 0.0f) {
            return i;
        }
    }

    // Use screen-size-based selection as fallback
    if (screenSize >= 0.0f) {
        for (int i = static_cast<int>(levels.size()) - 1; i >= 0; --i) {
            if (screenSize <= levels[i].screenSizeThreshold) {
                return i;
            }
        }
    }

    return 0;  // Highest quality
}

// ============================================================================
// MeshSimplifier — Edge Collapse Implementation
// ============================================================================

SimplificationResult MeshSimplifier::Simplify(
    const SimplificationVertex* vertices, uint32_t vertexCount,
    const uint32_t* indices, uint32_t indexCount,
    uint32_t targetIndexCount, float maxError) {

    SimplificationResult result;

    // Copy input data
    result.vertices.assign(vertices, vertices + vertexCount);
    result.indices.assign(indices, indices + indexCount);

    if (targetIndexCount >= indexCount) {
        result.errorMetric = 0.0f;
        return result;
    }

    // Build edge list
    struct Edge {
        uint32_t v0, v1;
        float cost;
        bool operator>(const Edge& other) const { return cost > other.cost; }
    };

    auto computeEdgeCost = [&](uint32_t v0, uint32_t v1) -> float {
        const auto& p0 = result.vertices[v0].position;
        const auto& p1 = result.vertices[v1].position;
        float dx = p1.x - p0.x, dy = p1.y - p0.y, dz = p1.z - p0.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        // Normal deviation penalty
        const auto& n0 = result.vertices[v0].normal;
        const auto& n1 = result.vertices[v1].normal;
        float normalDot = n0.x * n1.x + n0.y * n1.y + n0.z * n1.z;
        float normalPenalty = 1.0f - (std::max)(0.0f, normalDot);

        return dist + normalPenalty * 2.0f;
    };

    // Iterative edge collapse
    float bestCost = 1e30f;
    while (result.indices.size() > targetIndexCount && result.indices.size() >= 3) {
        // Find cheapest edge
        bestCost = 1e30f;
        uint32_t bestV0 = 0, bestV1 = 0;

        std::set<uint64_t> processedEdges;
        for (size_t i = 0; i + 2 < result.indices.size(); i += 3) {
            uint32_t tri[3] = {result.indices[i], result.indices[i+1], result.indices[i+2]};
            for (int e = 0; e < 3; ++e) {
                uint32_t a = tri[e], b = tri[(e + 1) % 3];
                uint64_t edgeKey = (static_cast<uint64_t>((std::min)(a, b)) << 32) |
                                    static_cast<uint64_t>((std::max)(a, b));
                if (processedEdges.count(edgeKey)) continue;
                processedEdges.insert(edgeKey);

                float cost = computeEdgeCost(a, b);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestV0 = a;
                    bestV1 = b;
                }
            }
        }

        if (bestCost > maxError * 100.0f) break;  // Error threshold reached

        // Collapse bestV1 into bestV0
        // Average the position
        auto& v0 = result.vertices[bestV0];
        const auto& v1 = result.vertices[bestV1];
        v0.position.x = (v0.position.x + v1.position.x) * 0.5f;
        v0.position.y = (v0.position.y + v1.position.y) * 0.5f;
        v0.position.z = (v0.position.z + v1.position.z) * 0.5f;
        v0.normal.x = (v0.normal.x + v1.normal.x) * 0.5f;
        v0.normal.y = (v0.normal.y + v1.normal.y) * 0.5f;
        v0.normal.z = (v0.normal.z + v1.normal.z) * 0.5f;
        // Normalize
        float len = std::sqrt(v0.normal.x * v0.normal.x + v0.normal.y * v0.normal.y + v0.normal.z * v0.normal.z);
        if (len > 0.0001f) {
            v0.normal.x /= len; v0.normal.y /= len; v0.normal.z /= len;
        }

        // Remap all references from bestV1 to bestV0
        for (auto& idx : result.indices) {
            if (idx == bestV1) idx = bestV0;
        }

        // Remove degenerate triangles
        std::vector<uint32_t> newIndices;
        for (size_t i = 0; i + 2 < result.indices.size(); i += 3) {
            uint32_t a = result.indices[i], b = result.indices[i+1], c = result.indices[i+2];
            if (a != b && b != c && a != c) {
                newIndices.push_back(a);
                newIndices.push_back(b);
                newIndices.push_back(c);
            }
        }
        result.indices = std::move(newIndices);
    }

    result.errorMetric = bestCost;
    return result;
}

SimplificationResult MeshSimplifier::SimplifyByRatio(
    const SimplificationVertex* vertices, uint32_t vertexCount,
    const uint32_t* indices, uint32_t indexCount,
    float ratio, float maxError) {

    uint32_t targetIndexCount = static_cast<uint32_t>(indexCount * ratio);
    targetIndexCount = (std::max)(targetIndexCount, 3u);  // At least one triangle
    targetIndexCount = (targetIndexCount / 3) * 3;  // Round to triangle boundary
    return Simplify(vertices, vertexCount, indices, indexCount, targetIndexCount, maxError);
}

// ============================================================================
// LODManager
// ============================================================================

LODManager& LODManager::GetInstance() {
    static LODManager instance;
    return instance;
}

LODChain LODManager::GenerateLODChain(const std::string& meshName,
                                       const SimplificationVertex* vertices, uint32_t vertexCount,
                                       const uint32_t* indices, uint32_t indexCount,
                                       const LODGenerationSettings& settings) {
    LODChain chain;
    chain.meshName = meshName;
    chain.totalVertices = 0;
    chain.totalIndices = 0;

    int numLevels = (std::min)(settings.numLevels, 8);

    for (int i = 0; i < numLevels; ++i) {
        LODLevel level{};
        level.maxDistance = settings.distanceMultipliers[i];
        level.screenSizeThreshold = 1.0f / (1.0f + settings.distanceMultipliers[i] * 0.01f);
        level.vertexOffset = chain.totalVertices;
        level.indexOffset = chain.totalIndices;

        if (i == 0) {
            // LOD 0 = original mesh
            level.vertexCount = vertexCount;
            level.indexCount = indexCount;
            level.qualityPercent = 100.0f;
        } else {
            // Generate simplified mesh
            auto simplified = MeshSimplifier::SimplifyByRatio(
                vertices, vertexCount, indices, indexCount,
                settings.reductionFactors[i], settings.targetError);

            level.vertexCount = static_cast<uint32_t>(simplified.vertices.size());
            level.indexCount = static_cast<uint32_t>(simplified.indices.size());
            level.qualityPercent = (indexCount > 0)
                ? (static_cast<float>(level.indexCount) / indexCount * 100.0f) : 0.0f;
        }

        chain.totalVertices += level.vertexCount;
        chain.totalIndices += level.indexCount;
        chain.levels.push_back(level);
    }

    m_lodChains[meshName] = chain;
    return chain;
}

void LODManager::RegisterLODChain(const std::string& meshName, LODChain chain) {
    m_lodChains[meshName] = std::move(chain);
}

const LODChain* LODManager::GetLODChain(const std::string& meshName) const {
    auto it = m_lodChains.find(meshName);
    return (it != m_lodChains.end()) ? &it->second : nullptr;
}

int LODManager::SelectLOD(const std::string& meshName, float distance, float screenSize) const {
    if (m_forcedLODLevel >= 0) return m_forcedLODLevel;

    const auto* chain = GetLODChain(meshName);
    if (!chain) return 0;

    // Apply global bias (positive = closer threshold = higher quality)
    float adjustedDistance = distance - m_globalBias * 10.0f;
    adjustedDistance = (std::max)(0.0f, adjustedDistance);

    return chain->SelectLOD(adjustedDistance, screenSize);
}

std::string LODManager::Console_ListLODChains() const {
    std::ostringstream ss;
    ss << "=== LOD Chains (" << m_lodChains.size() << ") ===\n";
    for (const auto& [name, chain] : m_lodChains) {
        ss << "  " << name << " [" << chain.levels.size() << " levels, "
           << chain.totalVertices << " total verts]\n";
    }
    ss << "Global LOD Bias: " << m_globalBias << "\n";
    if (m_forcedLODLevel >= 0)
        ss << "Forced LOD Level: " << m_forcedLODLevel << "\n";
    return ss.str();
}

std::string LODManager::Console_GetLODInfo(const std::string& meshName) const {
    auto it = m_lodChains.find(meshName);
    if (it == m_lodChains.end()) return "LOD chain '" + meshName + "' not found\n";

    const auto& chain = it->second;
    std::ostringstream ss;
    ss << "=== LOD: " << meshName << " ===\n";
    for (size_t i = 0; i < chain.levels.size(); ++i) {
        const auto& level = chain.levels[i];
        ss << "  LOD " << i << ": " << level.indexCount / 3 << " tris ("
           << level.qualityPercent << "%), dist: " << level.maxDistance << "\n";
    }
    return ss.str();
}

void LODManager::Console_SetLODBias(float bias) {
    m_globalBias = bias;
}

void LODManager::Console_ForceLODLevel(int level) {
    m_forcedLODLevel = level;
}

LODManager::LODStats LODManager::GetStats() const {
    m_stats.totalMeshes = static_cast<uint32_t>(m_lodChains.size());
    m_stats.totalLODLevels = 0;
    for (const auto& [_, chain] : m_lodChains)
        m_stats.totalLODLevels += static_cast<uint32_t>(chain.levels.size());
    return m_stats;
}

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS


#endif // SPARK_PLATFORM_WINDOWS
