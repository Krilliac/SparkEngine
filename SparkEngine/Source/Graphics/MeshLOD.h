/**
 * @file MeshLOD.h
 * @brief Mesh Level-of-Detail system with automatic generation and distance-based switching
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides automatic mesh simplification and runtime LOD selection:
 * - LOD chain generation via edge collapse
 * - Distance-based LOD switching
 * - Smooth LOD transitions (crossfade/dithered)
 * - Per-object LOD bias
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>


namespace Spark::Graphics
{

    // ============================================================================
    // LOD Level Data
    // ============================================================================

    struct LODLevel
    {
        float screenSizeThreshold; ///< Minimum screen percentage to use this LOD
        float maxDistance;         ///< Maximum distance from camera (0 = use screen size)
        uint32_t vertexCount;
        uint32_t indexCount;
        uint32_t vertexOffset; ///< Offset into shared vertex buffer
        uint32_t indexOffset;  ///< Offset into shared index buffer
        float qualityPercent;  ///< 0-100, percentage of original triangle count
    };

    struct LODChain
    {
        std::string meshName;
        std::vector<LODLevel> levels;
        uint32_t totalVertices;
        uint32_t totalIndices;

        int SelectLOD(float distance, float screenSize) const;
        int GetLevelCount() const { return static_cast<int>(levels.size()); }
    };

    // ============================================================================
    // LOD Generation Settings
    // ============================================================================

    struct LODGenerationSettings
    {
        int numLevels = 4; ///< Number of LOD levels to generate
        float reductionFactors[8] = {1.0f, 0.5f, 0.25f, 0.1f, 0.05f, 0.02f, 0.01f, 0.005f};
        float distanceMultipliers[8] = {0.0f, 20.0f, 40.0f, 80.0f, 160.0f, 320.0f, 640.0f, 1280.0f};
        float targetError = 0.01f; ///< Max geometric error
        bool preserveBorderEdges = true;
        bool preserveUVSeams = true;
        bool lockVerticesOnBoundary = true;
    };

    // ============================================================================
    // Mesh Simplification (Edge Collapse)
    // ============================================================================

    struct SimplificationVertex
    {
        XMFLOAT3 position;
        XMFLOAT3 normal;
        XMFLOAT2 texCoord;
    };

    struct SimplificationResult
    {
        std::vector<SimplificationVertex> vertices;
        std::vector<uint32_t> indices;
        float errorMetric;
    };

    class MeshSimplifier
    {
      public:
        /// Simplify a mesh to a target triangle count
        static SimplificationResult Simplify(const SimplificationVertex* vertices, uint32_t vertexCount,
                                             const uint32_t* indices, uint32_t indexCount, uint32_t targetIndexCount,
                                             float maxError = 0.01f);

        /// Simplify by reduction ratio (0.0 - 1.0)
        static SimplificationResult SimplifyByRatio(const SimplificationVertex* vertices, uint32_t vertexCount,
                                                    const uint32_t* indices, uint32_t indexCount, float ratio,
                                                    float maxError = 0.01f);
    };

    // ============================================================================
    // LOD Manager
    // ============================================================================

    /**
     * @class LODManager
     * @brief Registry singleton for generated / pre-built mesh LOD chains.
     *
     * @note **Intentional passive cache** — `LODManager` has no
     *       `Initialize()` / `Update()` / `Shutdown()` lifecycle. The
     *       matching comment in `GameplayLifecycleShared.cpp` reads
     *       "LODManager is a passive cache (no init/update needed; queries
     *       only)". Render code calls `SelectLOD()` on demand, and mesh
     *       importers call `RegisterLODChain()` at asset-load time. The
     *       header lives alongside `MeshSimplifier` (edge-collapse
     *       simplification) and `LODChain` (per-mesh level data), which
     *       are both pure CPU utilities. Exercised by
     *       `Tests/TestMeshLOD.cpp`.
     */
    class LODManager
    {
      public:
        static LODManager& GetInstance();

        /// Generate LOD chain for a mesh
        LODChain GenerateLODChain(const std::string& meshName, const SimplificationVertex* vertices,
                                  uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount,
                                  const LODGenerationSettings& settings = {});

        /// Register a pre-built LOD chain
        void RegisterLODChain(const std::string& meshName, LODChain chain);

        /// Get LOD chain for a mesh
        const LODChain* GetLODChain(const std::string& meshName) const;

        /// Select LOD level for a mesh at a given distance
        int SelectLOD(const std::string& meshName, float distance, float screenSize = -1.0f) const;

        /// Set global LOD bias (positive = higher quality, negative = lower)
        void SetGlobalLODBias(float bias) { m_globalBias = bias; }
        float GetGlobalLODBias() const { return m_globalBias; }

        /// Console integration
        std::string Console_ListLODChains() const;
        std::string Console_GetLODInfo(const std::string& meshName) const;
        void Console_SetLODBias(float bias);
        void Console_ForceLODLevel(int level); ///< -1 = auto

        /// Statistics
        struct LODStats
        {
            uint32_t totalMeshes;
            uint32_t totalLODLevels;
            uint32_t currentLODSwitches;
            float averageLODLevel;
            uint64_t trianglesSaved;
        };
        LODStats GetStats() const;

      private:
        LODManager() = default;
        std::unordered_map<std::string, LODChain> m_lodChains;
        float m_globalBias = 0.0f;
        int m_forcedLODLevel = -1;
        mutable LODStats m_stats{};
    };

} // namespace Spark::Graphics
