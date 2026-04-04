/**
 * @file LODGenerator.h
 * @brief Automatic LOD chain generation using quadric error metric simplification
 * @author Spark Engine Team
 * @date 2026
 *
 * Generates LOD chains from input meshes using edge-collapse mesh simplification
 * with the Quadric Error Metric (QEM) algorithm. Integrates with MeshLOD.h for
 * runtime LOD selection.
 *
 * @code
 *   auto& gen = LODGenerator::GetInstance();
 *   LODGenerationOptions opts;
 *   opts.lodCount = 4;
 *   opts.reductionPerLevel = 0.5f;
 *   auto result = gen.Generate(vertices, vertCount, indices, idxCount, opts);
 *   // result.levels[0] = original, result.levels[3] = most simplified
 * @endcode
 */

#pragma once

#include <cstdint>
#include <vector>

namespace Spark::Graphics
{

    // ============================================================================
    // Generation Options
    // ============================================================================

    /**
     * @brief Options controlling LOD chain generation
     */
    struct LODGenerationOptions
    {
        uint32_t lodCount = 4;          ///< Number of LOD levels including LOD0 (original)
        float reductionPerLevel = 0.5f; ///< Triangle reduction ratio per level (0.5 = 50%)
        float maxError = 0.01f;         ///< Maximum geometric error tolerance
        bool lockBorders = true;        ///< Preserve mesh border edges during simplification
        bool preserveAttributes = true; ///< Keep UVs/normals smooth across simplification

        /// Screen-size thresholds for each LOD level (index 0 = LOD0)
        std::vector<float> screenSizeThresholds = {1.0f, 0.5f, 0.25f, 0.1f};
    };

    // ============================================================================
    // Generated LOD Data
    // ============================================================================

    /**
     * @brief A single simplified mesh level
     */
    struct GeneratedLOD
    {
        std::vector<float> vertices; ///< x,y,z interleaved positions
        std::vector<float> normals;  ///< x,y,z interleaved normals
        std::vector<float> uvs;      ///< u,v interleaved texture coords
        std::vector<uint32_t> indices;
        uint32_t triangleCount = 0;
        float geometricError = 0.0f; ///< Max deviation from original mesh
    };

    /**
     * @brief Result of LOD chain generation
     */
    struct LODChainResult
    {
        std::vector<GeneratedLOD> levels;
        std::vector<float> screenSizeThresholds;
        float totalReduction = 0.0f; ///< e.g. 0.875 for 87.5% reduction at lowest LOD
    };

    // ============================================================================
    // LOD Generator
    // ============================================================================

    /**
     * @brief Generates LOD chains from input meshes via QEM simplification
     *
     * Uses the Quadric Error Metric (QEM) algorithm to iteratively collapse
     * edges in order of increasing geometric error. Produces multiple LOD
     * levels with progressively fewer triangles.
     */
    class LODGenerator
    {
      public:
        /**
         * @brief Get the singleton instance
         * @return Reference to the global LODGenerator
         */
        static LODGenerator& GetInstance();

        /**
         * @brief Generate a complete LOD chain from an input mesh
         * @param vertices    Interleaved x,y,z vertex positions
         * @param vertexCount Number of vertices (positions array has vertexCount*3 floats)
         * @param indices     Triangle index array
         * @param indexCount  Number of indices (must be multiple of 3)
         * @param options     Generation parameters
         * @return LODChainResult with all generated levels
         */
        LODChainResult Generate(const float* vertices, uint32_t vertexCount, const uint32_t* indices,
                                uint32_t indexCount, const LODGenerationOptions& options = {});

        /**
         * @brief Simplify a single mesh to a target triangle count
         * @param vertices        Interleaved x,y,z vertex positions
         * @param vertexCount     Number of vertices
         * @param indices         Triangle index array
         * @param indexCount      Number of indices
         * @param targetTriangles Desired triangle count after simplification
         * @param maxError        Maximum geometric error tolerance
         * @return GeneratedLOD with the simplified mesh
         */
        GeneratedLOD Simplify(const float* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount,
                              uint32_t targetTriangles, float maxError = 0.01f);

      private:
        LODGenerator() = default;

        /**
         * @brief Symmetric 4x4 matrix stored as upper triangle (10 elements)
         *
         * Used for accumulating per-vertex error quadrics in QEM simplification.
         */
        struct Quadric
        {
            double data[10] = {};

            Quadric& operator+=(const Quadric& other)
            {
                for (int i = 0; i < 10; ++i)
                    data[i] += other.data[i];
                return *this;
            }
        };

        /**
         * @brief Compute a face quadric from three vertex positions
         * @param p0 First vertex position (x,y,z)
         * @param p1 Second vertex position
         * @param p2 Third vertex position
         * @return Quadric for the face plane
         */
        static Quadric ComputeFaceQuadric(const float* p0, const float* p1, const float* p2);

        /**
         * @brief Evaluate the quadric error at a point
         * @param q   Quadric to evaluate
         * @param pos Point position (x,y,z)
         * @return Error value (smaller = better)
         */
        static double EvaluateQuadric(const Quadric& q, const float* pos);

        /**
         * @brief Recompute face-area-weighted normals after simplification
         * @param vertices    Interleaved positions (vertexCount*3)
         * @param vertexCount Number of vertices
         * @param indices     Index array
         * @param indexCount  Number of indices
         * @return Vector of interleaved normal values (vertexCount*3)
         */
        static std::vector<float> RecomputeNormals(const float* vertices, uint32_t vertexCount, const uint32_t* indices,
                                                   uint32_t indexCount);
    };

} // namespace Spark::Graphics
