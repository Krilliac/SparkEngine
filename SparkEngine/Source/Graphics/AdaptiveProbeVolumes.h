/**
 * @file AdaptiveProbeVolumes.h
 * @brief Brick-based hierarchical probe system with adaptive subdivision
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements an adaptive probe volume system inspired by Unity's APV.
 * Probes are organized in bricks (4x4x4 = 64 probes each) with multiple
 * LOD levels for varying spatial density. Bricks near geometry are subdivided
 * to finer spacing while open areas use coarser spacing.
 *
 * Features:
 * - Brick-based hierarchy with 3 LOD levels (1m, 2m, 4m spacing)
 * - Adaptive subdivision based on geometry proximity
 * - Per-pixel SH sampling via trilinear interpolation within bricks
 * - Virtual offsets to prevent light leaking near surfaces
 * - Camera-distance-based streaming of brick data
 * - L2 SH storage (9 coefficients x 3 channels = 27 floats per probe)
 *
 * @see DDGIProbeSystem.h, SHLighting.h, LightProbeSystem.h
 */

#pragma once

#include "../Core/Platform.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    static constexpr int APV_SH_COEFFICIENTS = 9;
    static constexpr int APV_PROBES_PER_BRICK_AXIS = 4;
    static constexpr int APV_PROBES_PER_BRICK =
        APV_PROBES_PER_BRICK_AXIS * APV_PROBES_PER_BRICK_AXIS * APV_PROBES_PER_BRICK_AXIS;

    // =========================================================================
    // LOD Levels
    // =========================================================================

    enum class BrickLOD : uint8_t
    {
        Fine = 0,   ///< 1m spacing between probes
        Medium = 1, ///< 2m spacing
        Coarse = 2, ///< 4m spacing
        Count = 3
    };

    /// @brief Base spacing (meters) for each LOD level
    inline float BrickLODSpacing(BrickLOD lod)
    {
        static constexpr float spacings[] = {1.0f, 2.0f, 4.0f};
        return spacings[static_cast<int>(lod)];
    }

    // =========================================================================
    // Probe Data
    // =========================================================================

    /// @brief SH coefficients for a single probe (L2, 3 channels)
    struct APVProbeData
    {
        std::array<float, APV_SH_COEFFICIENTS> r{};
        std::array<float, APV_SH_COEFFICIENTS> g{};
        std::array<float, APV_SH_COEFFICIENTS> b{};

        /// @brief Virtual offset to push probe away from surfaces
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float offsetZ = 0.0f;
        bool valid = true; ///< False if probe is inside geometry
    };

    // =========================================================================
    // Brick
    // =========================================================================

    /// @brief Unique brick identifier based on grid position and LOD
    struct BrickID
    {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        BrickLOD lod = BrickLOD::Coarse;

        bool operator==(const BrickID& other) const
        {
            return x == other.x && y == other.y && z == other.z && lod == other.lod;
        }
    };

    struct BrickIDHash
    {
        size_t operator()(const BrickID& id) const
        {
            size_t h = std::hash<int32_t>()(id.x);
            h ^= std::hash<int32_t>()(id.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int32_t>()(id.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>()(static_cast<uint8_t>(id.lod)) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    /// @brief A brick containing 4x4x4 = 64 probes
    struct APVBrick
    {
        BrickID id;
        std::array<APVProbeData, APV_PROBES_PER_BRICK> probes;
        float originX = 0.0f; ///< World origin of brick corner (0,0,0)
        float originY = 0.0f;
        float originZ = 0.0f;
        float spacing = 4.0f; ///< Probe spacing within this brick
        bool loaded = false;
        bool subdivided = false; ///< True if this brick has finer children
    };

    // =========================================================================
    // Irradiance Result
    // =========================================================================

    struct APVIrradiance
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
    };

    // =========================================================================
    // Adaptive Probe Volume Settings
    // =========================================================================

    struct APVSettings
    {
        float streamingDistance = 100.0f;          ///< Max distance from camera to load bricks
        float subdivisionGeometryThreshold = 2.0f; ///< Distance to geometry to trigger subdivision
        float virtualOffsetDistance = 0.25f;       ///< Push distance for virtual offsets
        float virtualOffsetSearchRadius = 1.0f;    ///< Search radius for offset raycast
        BrickLOD defaultLOD = BrickLOD::Coarse;
    };

    // =========================================================================
    // Adaptive Probe Volume System
    // =========================================================================

    /**
     * @brief Hierarchical adaptive probe volume with brick-based streaming
     *
     * CPU reference implementation. Manages a sparse set of bricks that
     * can be loaded/unloaded based on camera proximity and adaptively
     * subdivided near geometry.
     */
    class AdaptiveProbeVolumes
    {
      public:
        AdaptiveProbeVolumes() = default;
        ~AdaptiveProbeVolumes() = default;

        /// @brief Initialize the system
        bool Initialize(const APVSettings& settings);

        /// @brief Shutdown and release all data
        void Shutdown();

        /**
         * @brief Create a brick at the given grid position and LOD
         *
         * Initializes 64 probes at the appropriate spacing.
         *
         * @param gridX, gridY, gridZ  Brick grid coordinates
         * @param lod  LOD level determining probe spacing
         * @return True if brick was created successfully
         */
        bool CreateBrick(int32_t gridX, int32_t gridY, int32_t gridZ, BrickLOD lod);

        /**
         * @brief Subdivide a brick into finer LOD children
         *
         * Replaces a coarse brick with 8 finer bricks. The parent
         * is marked as subdivided and its probe data is used to
         * initialize children via interpolation.
         *
         * @param gridX, gridY, gridZ  Parent brick coordinates
         * @param parentLOD  LOD of the parent brick
         * @return True if subdivision succeeded
         */
        bool SubdivideBrick(int32_t gridX, int32_t gridY, int32_t gridZ, BrickLOD parentLOD);

        /**
         * @brief Apply virtual offsets to probes in a brick
         *
         * For each probe, checks if geometry is within the search radius.
         * If so, pushes the probe away from the surface to prevent
         * light leaking.
         *
         * @param id                  Brick identifier
         * @param geometryDistances   Array of 64 distances to nearest geometry
         *                            (negative if inside geometry)
         * @param geometryNormals     Array of 64 surface normals (nx,ny,nz triples)
         */
        void ApplyVirtualOffsets(const BrickID& id, const float* geometryDistances, const float* geometryNormals);

        /**
         * @brief Update streaming: load/unload bricks based on camera distance
         *
         * @param cameraPosX/Y/Z  Camera world position
         */
        void UpdateStreaming(float cameraPosX, float cameraPosY, float cameraPosZ);

        /**
         * @brief Store baked SH data into a specific probe within a brick
         *
         * @param id     Brick identifier
         * @param probeX, probeY, probeZ  Local probe indices [0, 3]
         * @param shR, shG, shB  Arrays of 9 SH coefficients per channel
         */
        void SetProbeData(const BrickID& id, int probeX, int probeY, int probeZ, const float shR[APV_SH_COEFFICIENTS],
                          const float shG[APV_SH_COEFFICIENTS], const float shB[APV_SH_COEFFICIENTS]);

        /**
         * @brief Sample irradiance at a world position and surface normal
         *
         * Finds the finest loaded brick containing the position and
         * performs trilinear interpolation of SH data within the brick.
         *
         * @param worldX/Y/Z    Query position
         * @param normalX/Y/Z   Surface normal for SH evaluation
         * @return Interpolated irradiance
         */
        APVIrradiance SampleIrradiance(float worldX, float worldY, float worldZ, float normalX, float normalY,
                                       float normalZ) const;

        /// @brief Get total number of loaded bricks
        uint32_t GetLoadedBrickCount() const { return static_cast<uint32_t>(m_bricks.size()); }

        /// @brief Get total number of probes across all loaded bricks
        uint32_t GetTotalProbeCount() const { return static_cast<uint32_t>(m_bricks.size()) * APV_PROBES_PER_BRICK; }

        /// @brief Check if initialized
        bool IsInitialized() const { return m_initialized; }

        /// @brief Access settings
        const APVSettings& GetSettings() const { return m_settings; }

      private:
        /// @brief Compute local probe index within a brick
        static int LocalProbeIndex(int px, int py, int pz);

        /// @brief Evaluate L2 SH basis functions
        static void EvaluateSHBasis(float nx, float ny, float nz, float outBasis[APV_SH_COEFFICIENTS]);

        /// @brief Initialize child brick probes from parent via interpolation
        void InterpolateBrickFromParent(APVBrick& child, const APVBrick& parent) const;

        APVSettings m_settings;
        std::unordered_map<BrickID, APVBrick, BrickIDHash> m_bricks;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
