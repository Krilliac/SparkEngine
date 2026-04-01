/**
 * @file DDGIProbeSystem.h
 * @brief Dynamic Diffuse Global Illumination using irradiance probes
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a probe-based DDGI system storing L2 spherical harmonics
 * coefficients on a regular 3D grid. Each probe accumulates irradiance
 * from ray-cast samples and blends with hysteresis for temporal stability.
 *
 * Features:
 * - Regular 3D grid placement with configurable spacing
 * - Per-probe ray dispatch with cosine-weighted hemisphere sampling
 * - Hysteresis blending for smooth temporal updates
 * - Border texel copy for seamless trilinear interpolation
 * - Probe relocation away from geometry to prevent light leaking
 * - Trilinear interpolation of 8 nearest probes at any world position
 *
 * @see SHLighting.h, LightProbeSystem.h, VoxelConeTracing.h
 */

#pragma once

#include "../Core/Platform.h"
#include "RHI/RHI.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Spark::Graphics
{

    /// @brief Number of L2 SH coefficients per color channel
    static constexpr int DDGI_SH_COEFFICIENTS = 9;

    /// @brief Total floats per probe (9 coefficients x 3 channels)
    static constexpr int DDGI_FLOATS_PER_PROBE = DDGI_SH_COEFFICIENTS * 3;

    // =========================================================================
    // DDGI Configuration
    // =========================================================================

    struct DDGISettings
    {
        float spacingX = 2.0f;            ///< Grid spacing along X axis (meters)
        float spacingY = 2.0f;            ///< Grid spacing along Y axis (meters)
        float spacingZ = 2.0f;            ///< Grid spacing along Z axis (meters)
        uint32_t countX = 8;              ///< Number of probes along X
        uint32_t countY = 4;              ///< Number of probes along Y
        uint32_t countZ = 8;              ///< Number of probes along Z
        float originX = 0.0f;             ///< Grid origin X
        float originY = 0.0f;             ///< Grid origin Y
        float originZ = 0.0f;             ///< Grid origin Z
        int raysPerProbe = 128;           ///< Rays cast per probe per update
        float hysteresis = 0.97f;         ///< Blend factor for temporal stability [0..1]
        float maxRayDistance = 50.0f;     ///< Maximum ray trace distance
        float normalBias = 0.25f;         ///< Bias along surface normal to prevent self-shadowing
        float relocationThreshold = 0.5f; ///< Min distance from geometry before relocation
        float relocationMaxOffset = 1.0f; ///< Maximum relocation offset
    };

    // =========================================================================
    // DDGI Probe Data
    // =========================================================================

    /// @brief Per-probe SH irradiance stored as 27 floats (9 R + 9 G + 9 B)
    struct DDGIProbe
    {
        std::array<float, DDGI_SH_COEFFICIENTS> r{};
        std::array<float, DDGI_SH_COEFFICIENTS> g{};
        std::array<float, DDGI_SH_COEFFICIENTS> b{};
        float offsetX = 0.0f; ///< Relocation offset from grid position
        float offsetY = 0.0f;
        float offsetZ = 0.0f;
        bool relocated = false;
    };

    /// @brief Result of a single ray cast for probe irradiance gathering
    struct DDGIRayResult
    {
        float hitDistance = -1.0f; ///< Distance to hit, negative if miss
        float radianceR = 0.0f;    ///< Incoming radiance R at hit point
        float radianceG = 0.0f;    ///< Incoming radiance G at hit point
        float radianceB = 0.0f;    ///< Incoming radiance B at hit point
        float hitNormalX = 0.0f;   ///< Surface normal at hit point
        float hitNormalY = 1.0f;
        float hitNormalZ = 0.0f;
    };

    /// @brief Interpolated irradiance result at a world position
    struct DDGIIrradiance
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
    };

    // =========================================================================
    // DDGI Probe System
    // =========================================================================

    /**
     * @brief Dynamic Diffuse Global Illumination probe system
     *
     * CPU reference implementation. Manages a 3D grid of irradiance probes
     * with SH encoding, temporal hysteresis, probe relocation, and
     * trilinear interpolation for querying irradiance at any world position.
     */
    class DDGIProbeSystem
    {
      public:
        DDGIProbeSystem() = default;
        ~DDGIProbeSystem() = default;

        /// @brief Initialize the probe grid with given settings
        bool Initialize(const DDGISettings& settings);

        /// @brief Shutdown and release probe data
        void Shutdown();

        /// @brief Get the world position of a probe at grid index (ix, iy, iz)
        void GetProbePosition(uint32_t ix, uint32_t iy, uint32_t iz, float& outX, float& outY, float& outZ) const;

        /**
         * @brief Update a single probe with new ray results
         *
         * Projects ray radiance into SH basis and blends with existing
         * coefficients using hysteresis for temporal stability.
         *
         * @param ix, iy, iz  Grid indices of the probe
         * @param rays        Array of ray results from tracing
         * @param rayCount    Number of rays in the array
         */
        void UpdateProbe(uint32_t ix, uint32_t iy, uint32_t iz, const DDGIRayResult* rays, int rayCount);

        /**
         * @brief Copy border texels for seamless interpolation
         *
         * In the GPU texture atlas, each probe's SH data occupies a small tile.
         * Border texels must be copied from the opposite edge to enable
         * hardware bilinear filtering across probe boundaries.
         * On CPU, this ensures the trilinear interpolation at grid edges
         * wraps correctly.
         */
        void UpdateProbeBorders();

        /**
         * @brief Relocate a probe away from nearby geometry
         *
         * If a probe is inside or very close to geometry, its irradiance
         * samples will be biased. This nudges the probe along the surface
         * normal of the closest hit to prevent light leaking.
         *
         * @param ix, iy, iz      Grid indices of the probe
         * @param closestHitDist  Distance to closest geometry from probe center
         * @param hitNormalX/Y/Z  Surface normal at the closest hit
         */
        void RelocateProbe(uint32_t ix, uint32_t iy, uint32_t iz, float closestHitDist, float hitNormalX,
                           float hitNormalY, float hitNormalZ);

        /**
         * @brief Query irradiance at an arbitrary world position
         *
         * Performs trilinear interpolation of the 8 nearest probes.
         * Each probe's SH is evaluated at the given surface normal
         * before interpolation.
         *
         * @param worldX, worldY, worldZ  Query position
         * @param normalX, normalY, normalZ  Surface normal for SH evaluation
         * @return Interpolated irradiance (RGB)
         */
        DDGIIrradiance QueryIrradiance(float worldX, float worldY, float worldZ, float normalX, float normalY,
                                       float normalZ) const;

        /// @brief Get total number of probes
        uint32_t GetProbeCount() const { return m_settings.countX * m_settings.countY * m_settings.countZ; }

        /// @brief Access the settings
        const DDGISettings& GetSettings() const { return m_settings; }

        /// @brief Check if initialized
        bool IsInitialized() const { return m_initialized; }

        // ---- GPU Resource Management ----

        /**
         * @brief Create GPU resources for the DDGI probe system
         *
         * Creates a structured buffer for all probe SH data (9 coefficients
         * x 3 channels per probe) and a constant buffer for grid configuration.
         *
         * @param device  RHI device to create resources on
         * @return True if resources were created successfully
         */
        bool CreateGPUResources(Spark::RHI::IRHIDevice* device);

        /**
         * @brief Upload current probe SH data to the GPU structured buffer
         * @param device  RHI device used for the upload
         */
        void UploadProbeDataToGPU(Spark::RHI::IRHIDevice* device);

        /// @brief Get the probe SH data structured buffer
        Spark::RHI::IRHIBuffer* GetProbeBuffer() const { return m_gpuProbeBuffer.get(); }

        /// @brief Check if GPU resources have been created
        bool HasGPUResources() const { return m_gpuProbeBuffer != nullptr; }

      private:
        /// @brief Compute flat index from 3D grid coordinates
        uint32_t ProbeIndex(uint32_t ix, uint32_t iy, uint32_t iz) const;

        /// @brief Copy SH data from source probe to destination probe (border clamping)
        void CopyProbeData(uint32_t dstIdx, uint32_t srcIdx);

        /**
         * @brief Evaluate the 9 real SH basis functions for a direction
         * @param nx, ny, nz  Normalized direction
         * @param outBasis    Output array of 9 basis values
         */
        static void EvaluateSHBasis(float nx, float ny, float nz, float outBasis[DDGI_SH_COEFFICIENTS]);

        /**
         * @brief Generate a quasi-uniform direction using spherical Fibonacci
         * @param index     Ray index
         * @param count     Total number of rays
         * @param outX/Y/Z  Output direction (normalized)
         */
        static void SphericalFibonacci(int index, int count, float& outX, float& outY, float& outZ);

        DDGISettings m_settings;
        std::vector<DDGIProbe> m_probes;
        bool m_initialized = false;

        // GPU resources
        std::unique_ptr<Spark::RHI::IRHIBuffer> m_gpuProbeBuffer;    ///< Structured buffer for probe SH data
        std::unique_ptr<Spark::RHI::IRHIBuffer> m_gpuConstantBuffer; ///< Constant buffer for grid config
    };

} // namespace Spark::Graphics
