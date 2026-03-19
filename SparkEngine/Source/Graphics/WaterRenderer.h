/**
 * @file WaterRenderer.h
 * @brief Water surface rendering with Gerstner wave simulation
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides CPU-side water plane management and Gerstner wave vertex animation.
 * Each water plane is a tessellated grid whose vertices are displaced each frame
 * by a sum of Gerstner wave components, producing realistic ocean-like surfaces.
 *
 * ## Usage
 * @code
 *   auto& water = Spark::Graphics::WaterRenderer::GetInstance();
 *   water.Initialize();
 *
 *   WaterSettings settings;
 *   settings.waveAmplitude = 0.5f;
 *   settings.waveCount = 4;
 *   water.SetSettings(settings);
 *
 *   uint32_t id = water.AddWaterPlane({0, 0, 0}, {100, 100});
 *   water.Update(deltaTime);
 *
 *   float height = water.GetWaterHeight(10.0f, 20.0f);
 * @endcode
 *
 * @see SkyAtmosphereSystem, FogSystem
 */

#pragma once

#include "../Core/Platform.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    // =========================================================================
    // Water Settings
    // =========================================================================

    /**
     * @brief Configuration for water surface rendering and simulation.
     */
    struct WaterSettings
    {
        XMFLOAT3 waterColor = {0.0f, 0.3f, 0.5f}; ///< Base water surface color (linear RGB)
        float opacity = 0.8f;                     ///< Water opacity (0 = transparent, 1 = opaque)
        float waveAmplitude = 0.3f;               ///< Global amplitude scale for waves
        float waveFrequency = 1.0f;               ///< Global frequency scale for waves
        float waveSpeed = 1.0f;                   ///< Global speed multiplier
        int waveCount = 4;                        ///< Number of Gerstner wave components
        float fresnelPower = 5.0f;                ///< Fresnel reflection power exponent
        float reflectionStrength = 0.5f;          ///< Planar reflection blend factor
        float refractionStrength = 0.3f;          ///< Refraction distortion strength
        float specularPower = 64.0f;              ///< Specular highlight sharpness
        float rippleSpeed = 1.0f;                 ///< Detail ripple animation speed
        bool enabled = true;                      ///< Master enable/disable
    };

    // =========================================================================
    // Gerstner Wave
    // =========================================================================

    /**
     * @brief A single Gerstner wave component in the wave summation.
     *
     * Gerstner waves produce the characteristic peaked crests and flat troughs
     * of ocean waves. Each component defines a travelling wave with independent
     * direction, amplitude, wavelength, speed, and steepness.
     */
    struct GerstnerWave
    {
        XMFLOAT2 direction = {1.0f, 0.0f}; ///< Normalized wave travel direction (XZ plane)
        float amplitude = 0.2f;            ///< Vertical displacement magnitude
        float wavelength = 10.0f;          ///< Distance between wave crests
        float speed = 1.0f;                ///< Phase velocity
        float steepness = 0.5f;            ///< Gerstner steepness Q (0 = sine, 1 = sharp peak)
    };

    // =========================================================================
    // Water Vertex
    // =========================================================================

    /**
     * @brief A single vertex of a water mesh with position, normal, and UV.
     */
    struct WaterVertex
    {
        XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
        XMFLOAT3 normal = {0.0f, 1.0f, 0.0f};
        XMFLOAT2 texCoord = {0.0f, 0.0f};
    };

    // =========================================================================
    // Water Plane (internal)
    // =========================================================================

    /**
     * @brief Internal representation of a single water body.
     */
    struct WaterPlane
    {
        XMFLOAT3 center = {0.0f, 0.0f, 0.0f};
        XMFLOAT2 size = {100.0f, 100.0f};
        std::vector<WaterVertex> vertices;
        std::vector<XMFLOAT3> basePositions; ///< Undisplaced grid positions
        int gridResolution = 32;             ///< Vertices per side
    };

    // =========================================================================
    // WaterRenderer
    // =========================================================================

    /**
     * @class WaterRenderer
     * @brief Manages water planes and animates them using Gerstner wave summation.
     *
     * Creates tessellated grid meshes for each water plane and displaces vertices
     * each frame using a sum of Gerstner wave components. Provides CPU-side height
     * queries for gameplay (e.g. buoyancy, splash effects).
     */
    class WaterRenderer
    {
      public:
        /** @brief Singleton access. */
        static WaterRenderer& GetInstance()
        {
            static WaterRenderer s;
            return s;
        }

        /**
         * @brief Initialize the water renderer and generate default wave components.
         * @return True on success.
         */
        bool Initialize();

        /** @brief Release all water planes and resources. */
        void Shutdown();

        // ---- Settings ----

        void SetSettings(const WaterSettings& settings);
        const WaterSettings& GetSettings() const { return m_settings; }

        // ---- Water Plane Management ----

        /**
         * @brief Add a new water plane to the scene.
         * @param center  World-space center of the water surface.
         * @param size    Width (X) and depth (Z) of the water plane.
         * @return        Unique identifier for the water plane.
         */
        uint32_t AddWaterPlane(const XMFLOAT3& center, const XMFLOAT2& size);

        /**
         * @brief Remove a water plane by ID.
         * @param waterID  ID returned by AddWaterPlane.
         */
        void RemoveWaterPlane(uint32_t waterID);

        // ---- Simulation ----

        /**
         * @brief Advance wave simulation and update all water plane vertex positions.
         * @param deltaTime  Frame time in seconds.
         */
        void Update(float deltaTime);

        // ---- Queries ----

        /**
         * @brief Sample the animated water height at a world position.
         * @param worldX  World X coordinate.
         * @param worldZ  World Z coordinate.
         * @return        Water surface height (Y), or 0 if no water at that position.
         */
        float GetWaterHeight(float worldX, float worldZ) const;

        /**
         * @brief Get the animated vertex data for a water plane.
         * @param waterID  ID of the water plane.
         * @return         Span of vertices, or empty span if ID is invalid.
         */
        std::span<const WaterVertex> GetMeshVertices(uint32_t waterID) const;

        /** @brief Get the number of active water planes. */
        size_t GetWaterPlaneCount() const { return m_planes.size(); }

      private:
        WaterRenderer() = default;
        ~WaterRenderer() = default;
        WaterRenderer(const WaterRenderer&) = delete;
        WaterRenderer& operator=(const WaterRenderer&) = delete;

        /** @brief Generate a flat tessellated grid for a water plane. */
        void GenerateGrid(WaterPlane& plane) const;

        /** @brief Generate default Gerstner wave components from settings. */
        void GenerateDefaultWaves();

        /**
         * @brief Compute Gerstner wave displacement at a position and time.
         * @param baseX  Undisplaced X position.
         * @param baseZ  Undisplaced Z position.
         * @param time   Current simulation time.
         * @return       Displaced position (XYZ).
         */
        XMFLOAT3 ComputeGerstnerDisplacement(float baseX, float baseZ, float time) const;

        /**
         * @brief Compute the surface normal at a position via finite differences.
         * @param baseX  Undisplaced X position.
         * @param baseZ  Undisplaced Z position.
         * @param time   Current simulation time.
         * @return       Approximate surface normal (normalized).
         */
        XMFLOAT3 ComputeGerstnerNormal(float baseX, float baseZ, float time) const;

        bool m_initialized = false;
        WaterSettings m_settings;
        float m_time = 0.0f;
        uint32_t m_nextID = 1;

        std::unordered_map<uint32_t, WaterPlane> m_planes;
        std::vector<GerstnerWave> m_waves;
    };

} // namespace Spark::Graphics
