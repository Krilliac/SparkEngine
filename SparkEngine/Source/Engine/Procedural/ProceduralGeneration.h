/**
 * @file ProceduralGeneration.h
 * @brief Procedural generation framework for levels, terrain, and meshes
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides tools for procedural content generation:
 * - Noise functions (Perlin, Simplex, Worley, FBM)
 * - Procedural meshes (primitives, terrain patches)
 * - Rule-based object placement
 * - Wave Function Collapse for room/dungeon layouts
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <random>
#include <cstdint>


namespace Spark::Procedural
{

    // ============================================================================
    // Noise Functions
    // ============================================================================

    class NoiseGenerator
    {
      public:
        explicit NoiseGenerator(uint32_t seed = 42);
        void SetSeed(uint32_t seed);

        /// 2D Perlin noise (-1 to 1)
        float Perlin2D(float x, float y) const;

        /// 3D Perlin noise (-1 to 1)
        float Perlin3D(float x, float y, float z) const;

        /// 2D Simplex noise (-1 to 1)
        float Simplex2D(float x, float y) const;

        /// 3D Simplex noise (-1 to 1)
        float Simplex3D(float x, float y, float z) const;

        /// Worley (cellular) noise — returns distance to nearest feature point
        float Worley2D(float x, float y, int cellCount = 8) const;

        /// Fractal Brownian Motion — layered noise
        float FBM(float x, float y, int octaves = 6, float lacunarity = 2.0f, float persistence = 0.5f) const;

        /// Ridged multifractal noise
        float RidgedMultifractal(float x, float y, int octaves = 6, float lacunarity = 2.0f, float gain = 2.0f) const;

        /// Domain warping
        float WarpedNoise(float x, float y, float warpStrength = 0.5f) const;

      private:
        std::vector<int> m_permutation;
        mutable std::mt19937 m_rng;

        float Fade(float t) const;
        float Lerp(float a, float b, float t) const;
        float Grad2D(int hash, float x, float y) const;
        float Grad3D(int hash, float x, float y, float z) const;
    };

    // ============================================================================
    // Heightmap Generator
    // ============================================================================

    struct HeightmapSettings
    {
        int width = 256;
        int height = 256;
        float scale = 50.0f; ///< Noise scale (larger = more zoomed out)
        int octaves = 6;
        float lacunarity = 2.0f;
        float persistence = 0.5f;
        float heightMultiplier = 50.0f;
        float seaLevel = 0.3f;
        uint32_t seed = 42;
        bool applyErosion = false;
        int erosionIterations = 10000;
    };

    class HeightmapGenerator
    {
      public:
        /// Generate a heightmap using FBM noise
        static std::vector<float> Generate(const HeightmapSettings& settings);

        /// Apply thermal erosion
        static void ApplyThermalErosion(std::vector<float>& heightmap, int width, int height, int iterations = 100,
                                        float talus = 0.01f);

        /// Apply hydraulic erosion
        static void ApplyHydraulicErosion(std::vector<float>& heightmap, int width, int height, int iterations = 10000,
                                          uint32_t seed = 42);

        /// Normalize heightmap to 0-1 range
        static void Normalize(std::vector<float>& heightmap);
    };

    // ============================================================================
    // Procedural Mesh Generation
    // ============================================================================

    struct ProceduralVertex
    {
        XMFLOAT3 position;
        XMFLOAT3 normal;
        XMFLOAT2 texCoord;
    };

    struct ProceduralMeshData
    {
        std::vector<ProceduralVertex> vertices;
        std::vector<uint32_t> indices;
    };

    class ProceduralMesh
    {
      public:
        static ProceduralMeshData CreatePlane(float width, float depth, int subdivX = 1, int subdivZ = 1);
        static ProceduralMeshData CreateBox(float width, float height, float depth, int subdivisions = 1);
        static ProceduralMeshData CreateSphere(float radius, int slices = 16, int stacks = 16);
        static ProceduralMeshData CreateCylinder(float radius, float height, int slices = 16);
        static ProceduralMeshData CreateCone(float radius, float height, int slices = 16);
        static ProceduralMeshData CreateTorus(float majorRadius, float minorRadius, int slices = 24, int stacks = 12);

        /// Create terrain mesh from heightmap
        static ProceduralMeshData CreateTerrainFromHeightmap(const float* heightmap, int width, int height,
                                                             float cellSize, float heightScale);

        /// Create procedural rock mesh
        static ProceduralMeshData CreateRock(float radius, int detail, uint32_t seed = 42);

        /// Create procedural tree (very simplified L-system)
        static ProceduralMeshData CreateTree(float height, int branches, uint32_t seed = 42);
    };

    // ============================================================================
    // Object Placement Rules
    // ============================================================================

    struct PlacementRule
    {
        std::string objectType;
        float density = 0.1f;            ///< Objects per square meter
        float minDistance = 2.0f;        ///< Minimum distance between objects
        float minHeight = 0.0f;          ///< Minimum terrain height
        float maxHeight = 1.0f;          ///< Maximum terrain height
        float minSlope = 0.0f;           ///< Minimum terrain slope (degrees)
        float maxSlope = 30.0f;          ///< Maximum terrain slope (degrees)
        XMFLOAT2 scaleRange{0.8f, 1.2f}; ///< Random scale range
        float rotationVariance = 360.0f; ///< Random Y rotation range
        bool alignToSurface = true;      ///< Align object to terrain normal

        /// Optional noise mask (only place where noise > threshold)
        float noiseMaskScale = 20.0f;
        float noiseMaskThreshold = 0.0f;
    };

    struct PlacementResult
    {
        std::string objectType;
        XMFLOAT3 position;
        XMFLOAT3 rotation;
        XMFLOAT3 scale;
    };

    class ObjectPlacer
    {
      public:
        /// Place objects on a heightmap according to rules
        static std::vector<PlacementResult> PlaceObjects(const float* heightmap, int width, int height, float cellSize,
                                                         const XMFLOAT3& origin,
                                                         const std::vector<PlacementRule>& rules, uint32_t seed = 42);
    };

    // ============================================================================
    // Wave Function Collapse (WFC) for Room Layouts
    // ============================================================================

    struct WFCTile
    {
        std::string name;
        int id;
        /// Socket compatibility: North, East, South, West
        std::string sockets[4];
        float weight = 1.0f;
        bool rotatable = true;
    };

    struct WFCGrid
    {
        int width, height;
        std::vector<std::vector<std::vector<int>>> cells; ///< [y][x] -> possible tile IDs

        bool IsCollapsed() const;
        bool HasContradiction() const;
    };

    class WaveFunctionCollapse
    {
      public:
        void AddTile(const WFCTile& tile);
        void SetGridSize(int width, int height);

        /// Run the WFC algorithm
        bool Collapse(uint32_t seed = 42);

        /// Get result
        const WFCGrid& GetResult() const { return m_grid; }
        const WFCTile* GetTile(int id) const;

        /// Console integration
        std::string Console_GetStatus() const;

      private:
        std::vector<WFCTile> m_tiles;
        WFCGrid m_grid;

        /// Find the cell with minimum entropy (fewest possibilities)
        std::pair<int, int> FindMinEntropyCell() const;

        /// Propagate constraints from a collapsed cell
        void Propagate(int x, int y);

        /// Check if two sockets are compatible
        bool AreSocketsCompatible(const std::string& a, const std::string& b) const;
    };

} // namespace Spark::Procedural
