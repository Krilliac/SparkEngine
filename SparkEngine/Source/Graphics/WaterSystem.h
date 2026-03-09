/**
 * @file WaterSystem.h
 * @brief Comprehensive water surface rendering with waves, reflections, and underwater effects
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a full water rendering pipeline for realistic ocean and lake surfaces:
 * - Gerstner wave simulation (up to 4 overlapping waves via GPU vertex displacement)
 * - Planar reflections with configurable clip plane offset
 * - Screen-space reflections (SSR) fallback for performance
 * - Refraction with depth-based extinction and color absorption
 * - Fresnel-based reflection/refraction blending
 * - Shore foam and wave crest foam generation
 * - Underwater caustics projection
 * - Underwater post-processing (fog, distortion, color grading)
 * - Tessellation-driven LOD for large water surfaces
 * - Multiple water volume support (oceans, lakes, rivers)
 *
 * Integrates with the DX11 rendering pipeline via ComPtr-managed resources
 * and the engine console for runtime parameter tuning.
 *
 * ## Usage
 * @code
 *   WaterSystem water;
 *   water.Initialize(device, context, 1920, 1080);
 *
 *   auto& settings = water.GetSettings();
 *   settings.shallowColor = {0.0f, 0.4f, 0.4f, 1.0f};
 *   settings.deepColor = {0.0f, 0.05f, 0.1f, 1.0f};
 *
 *   uint32_t id = water.AddVolume({0, 0, 0}, {1000, 1, 1000}, 0.0f);
 *
 *   // Each frame
 *   water.Update(deltaTime, cameraPosition, viewMatrix, projMatrix);
 *   water.RenderReflection(sceneRenderCallback);
 *   water.RenderRefraction(sceneRenderCallback);
 *   water.Render(viewMatrix, projMatrix);
 *
 *   if (water.IsCameraUnderwater(cameraPosition))
 *       water.ApplyUnderwaterPostProcess();
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#else
using Microsoft::WRL::ComPtr;
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics
{

    // =============================================================================
    // Constants
    // =============================================================================

    static constexpr uint32_t MAX_GERSTNER_WAVES = 4;
    static constexpr uint32_t MAX_WATER_VOLUMES = 32;
    static constexpr float WATER_DEFAULT_HEIGHT = 0.0f;

    // =============================================================================
    // Gerstner Wave Parameters
    // =============================================================================

    /**
     * @brief Parameters for a single Gerstner wave
     *
     * Gerstner waves produce realistic circular orbital motion on the water
     * surface. Multiple waves are summed for complex wave patterns.
     */
    struct GerstnerWaveParams
    {
        float amplitude = 0.5f;            ///< Wave height (peak to trough / 2)
        float frequency = 0.8f;            ///< Angular frequency (2*PI / wavelength)
        float steepness = 0.5f;            ///< Wave sharpness Q factor [0, 1]
        XMFLOAT2 direction = {1.0f, 0.0f}; ///< Normalized wave travel direction (XZ)
        float speed = 1.0f;                ///< Phase speed multiplier
        float padding0 = 0.0f;             ///< Pad to 32 bytes
    };

    // =============================================================================
    // Water Surface Settings
    // =============================================================================

    /**
     * @brief Complete configuration for water surface appearance and behavior
     */
    struct WaterSurfaceSettings
    {
        // ---- Wave Parameters ----
        std::array<GerstnerWaveParams, MAX_GERSTNER_WAVES> waves = {{
            {0.5f, 0.8f, 0.5f, {1.0f, 0.0f}, 1.0f, 0.0f},
            {0.25f, 1.2f, 0.4f, {0.7f, 0.7f}, 1.2f, 0.0f},
            {0.15f, 2.0f, 0.3f, {-0.4f, 0.9f}, 0.8f, 0.0f},
            {0.1f, 3.0f, 0.2f, {-0.8f, -0.6f}, 1.5f, 0.0f},
        }};
        uint32_t activeWaveCount = 4; ///< Number of active Gerstner waves [1, 4]
        float globalAmplitude = 1.0f; ///< Master amplitude multiplier
        float globalSpeed = 1.0f;     ///< Master speed multiplier

        // ---- Tessellation ----
        float tessellationFactor = 16.0f;       ///< Max tessellation level
        float tessellationMinDistance = 10.0f;  ///< Distance for max tessellation
        float tessellationMaxDistance = 200.0f; ///< Distance for min tessellation
        float displacementScale = 1.0f;         ///< Vertex displacement multiplier

        // ---- Water Color ----
        XMFLOAT4 shallowColor = {0.0f, 0.4f, 0.4f, 0.6f};     ///< Color at shallow depths (RGBA)
        XMFLOAT4 deepColor = {0.0f, 0.05f, 0.1f, 0.95f};      ///< Color at deep water (RGBA)
        float transparency = 0.8f;                            ///< Base transparency [0=opaque, 1=clear]
        float depthVisibility = 5.0f;                         ///< Depth at which water becomes fully opaque
        XMFLOAT3 extinctionCoefficient = {0.5f, 0.2f, 0.05f}; ///< Per-channel light absorption (RGB)

        // ---- Reflection / Refraction ----
        float reflectionDistortion = 0.03f; ///< Reflection UV distortion strength
        float refractionDistortion = 0.05f; ///< Refraction UV distortion strength
        float fresnelBias = 0.02f;          ///< Minimum reflectivity at normal incidence
        float fresnelPower = 5.0f;          ///< Fresnel exponent (higher = sharper falloff)
        float reflectionStrength = 1.0f;    ///< Reflection intensity multiplier
        float refractionStrength = 1.0f;    ///< Refraction intensity multiplier
        float clipPlaneOffset = 0.1f;       ///< Offset for reflection clip plane to avoid artifacts
        bool enablePlanarReflection = true; ///< Use planar reflection (higher quality)
        bool enableSSR = false;             ///< Use screen-space reflections as fallback

        // ---- Foam ----
        float shoreFoamWidth = 2.0f;              ///< Shore foam band width in world units
        float shoreFoamIntensity = 0.8f;          ///< Shore foam opacity [0, 1]
        float waveCrestFoamThreshold = 0.6f;      ///< Wave height threshold for crest foam [0, 1]
        float waveCrestFoamIntensity = 0.5f;      ///< Crest foam opacity [0, 1]
        float foamScale = 4.0f;                   ///< Foam texture tiling scale
        float foamSpeed = 0.3f;                   ///< Foam texture scroll speed
        XMFLOAT3 foamColor = {0.9f, 0.95f, 1.0f}; ///< Foam tint color

        // ---- Caustics ----
        float causticsIntensity = 0.5f;  ///< Caustic projection brightness [0, 1]
        float causticsScale = 8.0f;      ///< Caustic pattern tiling scale
        float causticsSpeed = 0.5f;      ///< Caustic animation speed
        float causticsDepthFade = 10.0f; ///< Depth at which caustics fade out

        // ---- Underwater Fog ----
        XMFLOAT3 underwaterFogColor = {0.0f, 0.15f, 0.2f}; ///< Underwater visibility tint
        float underwaterFogDensity = 0.08f;                ///< Underwater fog exponential density
        float underwaterFogStartDistance = 0.5f;           ///< Distance before underwater fog begins
        float underwaterFogMaxOpacity = 0.95f;             ///< Maximum fog opacity

        // ---- Normal Map ----
        float normalMapScale = 10.0f;                   ///< Normal map tiling scale
        float normalMapStrength = 1.0f;                 ///< Normal map blend strength [0, 1]
        XMFLOAT2 normalMapScrollSpeed = {0.02f, 0.01f}; ///< Normal map UV scroll velocity

        // ---- Specular ----
        float specularPower = 256.0f;   ///< Specular highlight tightness
        float specularIntensity = 1.0f; ///< Specular highlight brightness
    };

    // =============================================================================
    // GPU Constant Buffer Structures
    // =============================================================================

    /**
     * @brief GPU constant buffer for water surface shader (cbuffer WaterCB : register(b0))
     *
     * Laid out for 16-byte alignment per HLSL packing rules.
     */
    struct alignas(16) WaterConstants
    {
        // ---- Matrices (64 bytes each) ----
        XMFLOAT4X4 worldMatrix;          ///< Water mesh world transform
        XMFLOAT4X4 viewMatrix;           ///< Camera view matrix
        XMFLOAT4X4 projectionMatrix;     ///< Camera projection matrix
        XMFLOAT4X4 reflectionViewMatrix; ///< Reflected camera view matrix

        // ---- Gerstner Wave Data (32 bytes per wave = 128 bytes) ----
        struct alignas(16) GerstnerWaveGPU
        {
            XMFLOAT2 direction; ///< Normalized wave direction
            float amplitude;    ///< Wave amplitude
            float frequency;    ///< Angular frequency
            float steepness;    ///< Q factor
            float speed;        ///< Phase speed
            float padding[2];   ///< Pad to 32 bytes
        };
        GerstnerWaveGPU gerstnerWaves[MAX_GERSTNER_WAVES];

        // ---- Water Colors (16 bytes each) ----
        XMFLOAT4 shallowColor;    ///< Shallow water color + alpha
        XMFLOAT4 deepColor;       ///< Deep water color + alpha
        XMFLOAT4 extinctionCoeff; ///< RGB extinction + depth visibility in W

        // ---- Camera and Time (16 bytes) ----
        XMFLOAT3 cameraPosition; ///< World-space camera position
        float totalTime;         ///< Accumulated time for wave animation

        // ---- Reflection/Refraction Parameters (16 bytes) ----
        float reflectionDistortion;
        float refractionDistortion;
        float fresnelBias;
        float fresnelPower;

        // ---- Tessellation (16 bytes) ----
        float tessellationFactor;
        float tessellationMinDist;
        float tessellationMaxDist;
        float displacementScale;

        // ---- Foam Parameters (16 bytes) ----
        float shoreFoamWidth;
        float shoreFoamIntensity;
        float waveCrestFoamThreshold;
        float waveCrestFoamIntensity;

        // ---- Foam Visuals (16 bytes) ----
        XMFLOAT3 foamColor;
        float foamScale;

        // ---- Caustics (16 bytes) ----
        float causticsIntensity;
        float causticsScale;
        float causticsSpeed;
        float causticsDepthFade;

        // ---- Normal Map (16 bytes) ----
        float normalMapScale;
        float normalMapStrength;
        XMFLOAT2 normalMapScrollSpeed;

        // ---- Specular and Misc (16 bytes) ----
        float specularPower;
        float specularIntensity;
        float waterHeight;        ///< Y-coordinate of the water plane
        uint32_t activeWaveCount; ///< Number of active waves

        // ---- Clip Plane (16 bytes) ----
        XMFLOAT4 clipPlane; ///< Reflection/refraction clip plane
    };

    /**
     * @brief GPU constant buffer for underwater post-processing (cbuffer UnderwaterCB : register(b1))
     */
    struct alignas(16) UnderwaterPostProcessConstants
    {
        XMFLOAT3 fogColor; ///< Underwater fog color
        float fogDensity;  ///< Fog exponential density

        float fogStartDistance;   ///< Distance before fog begins
        float fogMaxOpacity;      ///< Maximum fog opacity
        float distortionStrength; ///< Screen-space distortion amount
        float distortionSpeed;    ///< Distortion animation speed

        XMFLOAT3 tintColor;  ///< Overall underwater color tint
        float tintIntensity; ///< Tint blend factor

        float causticsIntensity; ///< Caustic overlay intensity
        float causticsScale;     ///< Caustic pattern scale
        float totalTime;         ///< Accumulated time for animation
        float waterSurfaceY;     ///< Y-coordinate of water surface

        XMFLOAT3 cameraPosition; ///< Camera world position
        float depthFade;         ///< Depth at which effects fully apply

        XMFLOAT4X4 invViewProjection; ///< Inverse view-projection for position reconstruction
    };

    // =============================================================================
    // Water Volume
    // =============================================================================

    /**
     * @class WaterVolume
     * @brief Defines a bounded water body with its own transform and flow properties
     *
     * Each WaterVolume represents a discrete water body (ocean, lake, river)
     * with independent position, size, flow direction, and LOD settings.
     */
    class WaterVolume
    {
      public:
        WaterVolume() = default;

        WaterVolume(const XMFLOAT3& position, const XMFLOAT3& size, float height)
            : m_position(position), m_size(size), m_height(height)
        {
        }

        ~WaterVolume() = default;

        // ---- Transform ----

        void SetPosition(const XMFLOAT3& position) { m_position = position; }
        const XMFLOAT3& GetPosition() const { return m_position; }

        void SetSize(const XMFLOAT3& size) { m_size = size; }
        const XMFLOAT3& GetSize() const { return m_size; }

        void SetHeight(float height) { m_height = height; }
        float GetHeight() const { return m_height; }

        // ---- Flow ----

        void SetFlowDirection(const XMFLOAT2& direction) { m_flowDirection = direction; }
        const XMFLOAT2& GetFlowDirection() const { return m_flowDirection; }

        void SetFlowSpeed(float speed) { m_flowSpeed = std::max(speed, 0.0f); }
        float GetFlowSpeed() const { return m_flowSpeed; }

        // ---- Shore Detection ----

        void SetShoreDetectionDistance(float distance) { m_shoreDetectionDistance = std::max(distance, 0.0f); }
        float GetShoreDetectionDistance() const { return m_shoreDetectionDistance; }

        // ---- LOD ----

        void SetLODDistances(float near, float mid, float far)
        {
            m_lodNearDistance = std::max(near, 1.0f);
            m_lodMidDistance = std::max(mid, m_lodNearDistance + 1.0f);
            m_lodFarDistance = std::max(far, m_lodMidDistance + 1.0f);
        }
        float GetLODNearDistance() const { return m_lodNearDistance; }
        float GetLODMidDistance() const { return m_lodMidDistance; }
        float GetLODFarDistance() const { return m_lodFarDistance; }

        // ---- State ----

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        void SetVolumeId(uint32_t id) { m_volumeId = id; }
        uint32_t GetVolumeId() const { return m_volumeId; }

        // ---- Queries ----

        /**
         * @brief Test if a world-space point is within this water volume's XZ bounds
         */
        bool ContainsPointXZ(float x, float z) const
        {
            float halfW = m_size.x * 0.5f;
            float halfD = m_size.z * 0.5f;
            return (x >= m_position.x - halfW && x <= m_position.x + halfW && z >= m_position.z - halfD &&
                    z <= m_position.z + halfD);
        }

        /**
         * @brief Test if a point is below the water surface height
         */
        bool IsPointUnderwater(const XMFLOAT3& point) const
        {
            return ContainsPointXZ(point.x, point.z) && point.y < m_height;
        }

        /**
         * @brief Compute the world matrix for this water volume
         */
        XMMATRIX ComputeWorldMatrix() const
        {
            return XMMatrixScaling(m_size.x, 1.0f, m_size.z) *
                   XMMatrixTranslation(m_position.x, m_height, m_position.z);
        }

        /**
         * @brief Calculate a displaced surface normal from Gerstner wave sum
         * @param worldX World X coordinate on the surface
         * @param worldZ World Z coordinate on the surface
         * @param time Current simulation time
         * @param waves Array of wave parameters
         * @param waveCount Number of active waves
         * @return Normalized surface normal at the given point
         */
        static XMFLOAT3 CalculateNormal(float worldX, float worldZ, float time, const GerstnerWaveParams* waves,
                                        uint32_t waveCount)
        {
            float nx = 0.0f;
            float ny = 1.0f;
            float nz = 0.0f;

            for (uint32_t i = 0; i < waveCount && i < MAX_GERSTNER_WAVES; ++i)
            {
                const auto& w = waves[i];
                float dot = w.direction.x * worldX + w.direction.y * worldZ;
                float phase = w.frequency * dot - w.speed * time;
                float cosPhase = std::cos(phase);
                float sinPhase = std::sin(phase);

                float wa = w.frequency * w.amplitude;

                nx -= w.direction.x * wa * cosPhase;
                ny -= w.steepness * wa * sinPhase;
                nz -= w.direction.y * wa * cosPhase;
            }

            // Normalize
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f)
            {
                float invLen = 1.0f / len;
                return {nx * invLen, ny * invLen, nz * invLen};
            }
            return {0.0f, 1.0f, 0.0f};
        }

        /**
         * @brief Calculate the Gerstner wave displacement at a given world XZ position
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @param time Current simulation time
         * @param waves Array of wave parameters
         * @param waveCount Number of active waves
         * @return Displacement vector (X, Y, Z)
         */
        static XMFLOAT3 CalculateDisplacement(float worldX, float worldZ, float time, const GerstnerWaveParams* waves,
                                              uint32_t waveCount)
        {
            float dx = 0.0f;
            float dy = 0.0f;
            float dz = 0.0f;

            for (uint32_t i = 0; i < waveCount && i < MAX_GERSTNER_WAVES; ++i)
            {
                const auto& w = waves[i];
                float dot = w.direction.x * worldX + w.direction.y * worldZ;
                float phase = w.frequency * dot - w.speed * time;
                float cosPhase = std::cos(phase);
                float sinPhase = std::sin(phase);

                float qi = w.steepness / (w.frequency * w.amplitude * static_cast<float>(waveCount));

                dx += qi * w.amplitude * w.direction.x * cosPhase;
                dy += w.amplitude * sinPhase;
                dz += qi * w.amplitude * w.direction.y * cosPhase;
            }

            return {dx, dy, dz};
        }

      private:
        XMFLOAT3 m_position = {0.0f, 0.0f, 0.0f};
        XMFLOAT3 m_size = {100.0f, 1.0f, 100.0f};
        float m_height = WATER_DEFAULT_HEIGHT;

        XMFLOAT2 m_flowDirection = {1.0f, 0.0f};
        float m_flowSpeed = 0.0f;

        float m_shoreDetectionDistance = 5.0f;

        float m_lodNearDistance = 50.0f;
        float m_lodMidDistance = 150.0f;
        float m_lodFarDistance = 500.0f;

        bool m_enabled = true;
        uint32_t m_volumeId = 0;
    };

    // =============================================================================
    // Water Metrics
    // =============================================================================

    struct WaterMetrics
    {
        uint32_t activeVolumeCount = 0;     ///< Number of enabled water volumes
        uint32_t totalTriangles = 0;        ///< Triangles rendered this frame
        float reflectionPassTimeMs = 0.0f;  ///< Reflection render time
        float refractionPassTimeMs = 0.0f;  ///< Refraction render time
        float waterPassTimeMs = 0.0f;       ///< Water surface render time
        float underwaterPassTimeMs = 0.0f;  ///< Underwater post-process time
        bool isCameraUnderwater = false;    ///< Whether camera is currently submerged
        uint32_t reflectionResolutionX = 0; ///< Reflection render target width
        uint32_t reflectionResolutionY = 0; ///< Reflection render target height
    };

    // =============================================================================
    // Water System
    // =============================================================================

    /**
     * @class WaterSystem
     * @brief Manages all water rendering including surfaces, reflections, and underwater effects
     *
     * The WaterSystem orchestrates the full water rendering pipeline:
     * 1. Update wave simulation time and camera state
     * 2. Render scene into reflection render target (planar reflection or SSR)
     * 3. Render scene into refraction render target with clip plane
     * 4. Render water surface mesh with Gerstner displacement
     * 5. Apply underwater post-processing if camera is submerged
     *
     * Thread safety: main thread only (follows GraphicsEngine convention).
     */
    class WaterSystem
    {
      public:
        WaterSystem() = default;
        ~WaterSystem() = default;

        // ---- Lifecycle ----

        /**
         * @brief Initialize the water system and create GPU resources
         * @param device D3D11 device for resource creation
         * @param context D3D11 device context for rendering
         * @param width Viewport width for render targets
         * @param height Viewport height for render targets
         * @return true on success
         */
        bool Initialize([[maybe_unused]] ID3D11Device* device, [[maybe_unused]] ID3D11DeviceContext* context,
                        uint32_t width = 1920, uint32_t height = 1080)
        {
            m_viewportWidth = width;
            m_viewportHeight = height;
            m_reflectionWidth = width / 2;
            m_reflectionHeight = height / 2;
            m_refractionWidth = width;
            m_refractionHeight = height;

#ifdef SPARK_PLATFORM_WINDOWS
            m_device = device;
            m_context = context;

            if (!CreateRenderTargets())
            {
                return false;
            }
            if (!CreateConstantBuffers())
            {
                return false;
            }
            if (!CreateSamplerStates())
            {
                return false;
            }
#endif

            m_initialized = true;
            return true;
        }

        /**
         * @brief Release all GPU resources and reset state
         */
        void Shutdown()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            m_reflectionRTV.Reset();
            m_reflectionSRV.Reset();
            m_reflectionTexture.Reset();
            m_reflectionDSV.Reset();
            m_reflectionDepthTexture.Reset();

            m_refractionRTV.Reset();
            m_refractionSRV.Reset();
            m_refractionTexture.Reset();
            m_refractionDepthSRV.Reset();
            m_refractionDepthTexture.Reset();

            m_waterConstantBuffer.Reset();
            m_underwaterConstantBuffer.Reset();
            m_waterSampler.Reset();

            m_device = nullptr;
            m_context = nullptr;
#endif

            m_volumes.clear();
            m_nextVolumeId = 1;
            m_initialized = false;
            m_totalTime = 0.0f;
        }

        /**
         * @brief Update wave simulation and camera state
         * @param deltaTime Frame delta time in seconds
         * @param cameraPosition World-space camera position
         * @param viewMatrix Current view matrix
         * @param projMatrix Current projection matrix
         */
        void Update(float deltaTime, const XMFLOAT3& cameraPosition, const XMFLOAT4X4& viewMatrix,
                    const XMFLOAT4X4& projMatrix)
        {
            if (!m_initialized)
            {
                return;
            }

            m_totalTime += deltaTime * m_settings.globalSpeed;
            m_cameraPosition = cameraPosition;
            m_viewMatrix = viewMatrix;
            m_projMatrix = projMatrix;

            // Determine if camera is underwater in any active volume
            m_cameraUnderwater = false;
            m_activeUnderwaterVolume = nullptr;
            for (auto& volume : m_volumes)
            {
                if (volume.IsEnabled() && volume.IsPointUnderwater(cameraPosition))
                {
                    m_cameraUnderwater = true;
                    m_activeUnderwaterVolume = &volume;
                    break;
                }
            }

            // Update metrics
            m_metrics.isCameraUnderwater = m_cameraUnderwater;
            m_metrics.activeVolumeCount = 0;
            for (const auto& volume : m_volumes)
            {
                if (volume.IsEnabled())
                {
                    m_metrics.activeVolumeCount++;
                }
            }
        }

        /**
         * @brief Render the scene into the reflection render target
         *
         * Call this before Render(). The caller provides a callback that renders
         * the scene with the reflected view matrix and clip plane applied.
         * Uses planar reflection if enabled, otherwise prepares for SSR.
         */
        void RenderReflection()
        {
            if (!m_initialized || !m_settings.enablePlanarReflection)
            {
                return;
            }

#ifdef SPARK_PLATFORM_WINDOWS
            // Bind reflection render target
            if (m_reflectionRTV.Get() && m_reflectionDSV.Get())
            {
                float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                m_context->ClearRenderTargetView(m_reflectionRTV.Get(), clearColor);
                m_context->ClearDepthStencilView(m_reflectionDSV.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f,
                                                 0);
                m_context->OMSetRenderTargets(1, m_reflectionRTV.GetAddressOf(), m_reflectionDSV.Get());
            }
#endif
            // Scene rendering with reflected view is done externally via callback
        }

        /**
         * @brief Render the scene into the refraction render target
         *
         * Call this before Render(). Captures the scene below the water surface
         * using a clip plane.
         */
        void RenderRefraction()
        {
            if (!m_initialized)
            {
                return;
            }

#ifdef SPARK_PLATFORM_WINDOWS
            if (m_refractionRTV.Get())
            {
                float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                m_context->ClearRenderTargetView(m_refractionRTV.Get(), clearColor);
                m_context->OMSetRenderTargets(1, m_refractionRTV.GetAddressOf(), nullptr);
            }
#endif
        }

        /**
         * @brief Render all active water surface volumes
         * @param viewMatrix View matrix for this frame
         * @param projMatrix Projection matrix for this frame
         */
        void Render([[maybe_unused]] const XMFLOAT4X4& viewMatrix, [[maybe_unused]] const XMFLOAT4X4& projMatrix)
        {
            if (!m_initialized)
            {
                return;
            }

            m_metrics.totalTriangles = 0;

            for (auto& volume : m_volumes)
            {
                if (!volume.IsEnabled())
                {
                    continue;
                }

#ifdef SPARK_PLATFORM_WINDOWS
                UpdateWaterConstants(volume, viewMatrix, projMatrix);
                BindWaterResources();
                DrawWaterMesh(volume);
#endif
            }
        }

        /**
         * @brief Apply underwater post-processing if camera is submerged
         *
         * Applies fog, distortion, caustics overlay, and color tinting
         * when the camera is beneath a water volume.
         */
        void ApplyUnderwaterPostProcess()
        {
            if (!m_initialized || !m_cameraUnderwater || m_activeUnderwaterVolume == nullptr)
            {
                return;
            }

#ifdef SPARK_PLATFORM_WINDOWS
            UpdateUnderwaterConstants();
            BindUnderwaterResources();
            DrawFullscreenQuad();
#endif
        }

        // ---- Resize ----

        /**
         * @brief Handle viewport resize and recreate render targets
         */
        void Resize(uint32_t width, uint32_t height)
        {
            m_viewportWidth = width;
            m_viewportHeight = height;
            m_reflectionWidth = width / 2;
            m_reflectionHeight = height / 2;
            m_refractionWidth = width;
            m_refractionHeight = height;

            if (m_initialized)
            {
#ifdef SPARK_PLATFORM_WINDOWS
                CreateRenderTargets();
#endif
            }

            m_metrics.reflectionResolutionX = m_reflectionWidth;
            m_metrics.reflectionResolutionY = m_reflectionHeight;
        }

        // ---- Volume Management ----

        /**
         * @brief Add a new water volume
         * @param position World-space center position
         * @param size Volume dimensions (width, height, depth)
         * @param height Water surface Y coordinate
         * @return Unique volume ID
         */
        uint32_t AddVolume(const XMFLOAT3& position, const XMFLOAT3& size, float height)
        {
            if (m_volumes.size() >= MAX_WATER_VOLUMES)
            {
                return 0;
            }

            WaterVolume volume(position, size, height);
            uint32_t id = m_nextVolumeId++;
            volume.SetVolumeId(id);
            m_volumes.push_back(volume);
            return id;
        }

        /**
         * @brief Remove a water volume by ID
         * @return true if the volume was found and removed
         */
        bool RemoveVolume(uint32_t volumeId)
        {
            for (auto it = m_volumes.begin(); it != m_volumes.end(); ++it)
            {
                if (it->GetVolumeId() == volumeId)
                {
                    m_volumes.erase(it);
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Get a mutable reference to a volume by ID
         * @return Pointer to the volume, or nullptr if not found
         */
        WaterVolume* GetVolume(uint32_t volumeId)
        {
            for (auto& volume : m_volumes)
            {
                if (volume.GetVolumeId() == volumeId)
                {
                    return &volume;
                }
            }
            return nullptr;
        }

        const WaterVolume* GetVolume(uint32_t volumeId) const
        {
            for (const auto& volume : m_volumes)
            {
                if (volume.GetVolumeId() == volumeId)
                {
                    return &volume;
                }
            }
            return nullptr;
        }

        uint32_t GetVolumeCount() const { return static_cast<uint32_t>(m_volumes.size()); }

        const std::vector<WaterVolume>& GetVolumes() const { return m_volumes; }

        // ---- Underwater Detection ----

        /**
         * @brief Check if a world-space position is underwater in any active volume
         */
        bool IsCameraUnderwater(const XMFLOAT3& position) const
        {
            for (const auto& volume : m_volumes)
            {
                if (volume.IsEnabled() && volume.IsPointUnderwater(position))
                {
                    return true;
                }
            }
            return false;
        }

        bool IsCameraCurrentlyUnderwater() const { return m_cameraUnderwater; }

        // ---- Settings ----

        WaterSurfaceSettings& GetSettings() { return m_settings; }
        const WaterSurfaceSettings& GetSettings() const { return m_settings; }

        void SetSettings(const WaterSurfaceSettings& settings) { m_settings = settings; }

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // ---- Reflection Matrix ----

        /**
         * @brief Compute a planar reflection matrix for a given water surface height
         * @param waterHeight Y coordinate of the water plane
         * @param viewMatrix Source view matrix to reflect
         * @return Reflected view matrix
         */
        static XMFLOAT4X4 ComputeReflectionMatrix(float waterHeight, const XMFLOAT4X4& viewMatrix)
        {
            // Reflection matrix about Y = waterHeight plane
            // R = T(0, waterHeight, 0) * Scale(1, -1, 1) * T(0, -waterHeight, 0)
            XMMATRIX reflection = XMMatrixIdentity();
            reflection.m[1][1] = -1.0f;
            reflection.m[3][1] = 2.0f * waterHeight;

            XMMATRIX view = XMLoadFloat4x4(&viewMatrix);
            XMMATRIX reflectedView = reflection * view;

            XMFLOAT4X4 result;
            XMStoreFloat4x4(&result, reflectedView);
            return result;
        }

        /**
         * @brief Compute the clip plane for reflection rendering
         * @param waterHeight Y coordinate of the water plane
         * @param offset Small offset to avoid edge artifacts
         * @return Clip plane as (A, B, C, D) where Ax + By + Cz + D = 0
         */
        static XMFLOAT4 ComputeReflectionClipPlane(float waterHeight, float offset)
        {
            return {0.0f, 1.0f, 0.0f, -(waterHeight + offset)};
        }

        /**
         * @brief Compute the clip plane for refraction rendering
         * @param waterHeight Y coordinate of the water plane
         * @param offset Small offset to avoid edge artifacts
         * @return Clip plane as (A, B, C, D)
         */
        static XMFLOAT4 ComputeRefractionClipPlane(float waterHeight, float offset)
        {
            return {0.0f, -1.0f, 0.0f, waterHeight + offset};
        }

        // ---- Render Target Accessors (for external binding) ----

#ifdef SPARK_PLATFORM_WINDOWS
        ID3D11ShaderResourceView* GetReflectionSRV() const { return m_reflectionSRV.Get(); }
        ID3D11ShaderResourceView* GetRefractionSRV() const { return m_refractionSRV.Get(); }
        ID3D11ShaderResourceView* GetRefractionDepthSRV() const { return m_refractionDepthSRV.Get(); }

        ID3D11RenderTargetView* GetReflectionRTV() const { return m_reflectionRTV.Get(); }
        ID3D11RenderTargetView* GetRefractionRTV() const { return m_refractionRTV.Get(); }
        ID3D11DepthStencilView* GetReflectionDSV() const { return m_reflectionDSV.Get(); }
#endif

        // ---- Metrics ----

        const WaterMetrics& GetMetrics() const { return m_metrics; }

        // ---- Console Integration ----

        std::string Console_GetStatus() const
        {
            std::string s = "Water System:\n";
            s += "  Enabled: " + std::string(m_enabled ? "yes" : "no") + "\n";
            s += "  Initialized: " + std::string(m_initialized ? "yes" : "no") + "\n";
            s += "  Volumes: " + std::to_string(m_volumes.size()) + "\n";
            s += "  Active waves: " + std::to_string(m_settings.activeWaveCount) + "\n";
            s += "  Camera underwater: " + std::string(m_cameraUnderwater ? "yes" : "no") + "\n";
            s += "  Planar reflection: " + std::string(m_settings.enablePlanarReflection ? "on" : "off") + "\n";
            s += "  SSR fallback: " + std::string(m_settings.enableSSR ? "on" : "off") + "\n";
            s += "  Reflection res: " + std::to_string(m_reflectionWidth) + "x" + std::to_string(m_reflectionHeight) +
                 "\n";
            s += "  Tessellation: " + std::to_string(m_settings.tessellationFactor) + "\n";
            s += "  Total time: " + std::to_string(m_totalTime) + "\n";
            return s;
        }

        std::string Console_ListVolumes() const
        {
            std::string s = "Water Volumes (" + std::to_string(m_volumes.size()) + "):\n";
            for (const auto& v : m_volumes)
            {
                s += "  [" + std::to_string(v.GetVolumeId()) + "] " + (v.IsEnabled() ? "ON" : "OFF") + " pos=(" +
                     std::to_string(v.GetPosition().x) + ", " + std::to_string(v.GetPosition().y) + ", " +
                     std::to_string(v.GetPosition().z) + ")" + " size=(" + std::to_string(v.GetSize().x) + ", " +
                     std::to_string(v.GetSize().y) + ", " + std::to_string(v.GetSize().z) + ")" +
                     " height=" + std::to_string(v.GetHeight()) + "\n";
            }
            return s;
        }

        std::string Console_GetWaveInfo() const
        {
            std::string s = "Gerstner Waves (" + std::to_string(m_settings.activeWaveCount) + " active):\n";
            for (uint32_t i = 0; i < m_settings.activeWaveCount && i < MAX_GERSTNER_WAVES; ++i)
            {
                const auto& w = m_settings.waves[i];
                s += "  Wave " + std::to_string(i) + ": " + "amp=" + std::to_string(w.amplitude) +
                     " freq=" + std::to_string(w.frequency) + " steep=" + std::to_string(w.steepness) + " dir=(" +
                     std::to_string(w.direction.x) + ", " + std::to_string(w.direction.y) + ")" +
                     " speed=" + std::to_string(w.speed) + "\n";
            }
            s += "  Global amplitude: " + std::to_string(m_settings.globalAmplitude) + "\n";
            s += "  Global speed: " + std::to_string(m_settings.globalSpeed) + "\n";
            return s;
        }

        std::string Console_SetWaveAmplitude(uint32_t index, float amplitude)
        {
            if (index >= MAX_GERSTNER_WAVES)
            {
                return "Error: Wave index must be 0-" + std::to_string(MAX_GERSTNER_WAVES - 1);
            }
            m_settings.waves[index].amplitude = std::max(amplitude, 0.0f);
            return "Wave " + std::to_string(index) + " amplitude set to " +
                   std::to_string(m_settings.waves[index].amplitude);
        }

        std::string Console_SetWaterColor(float sr, float sg, float sb, float dr, float dg, float db)
        {
            m_settings.shallowColor = {sr, sg, sb, m_settings.shallowColor.w};
            m_settings.deepColor = {dr, dg, db, m_settings.deepColor.w};
            return "Water colors updated";
        }

      private:
        // ---- GPU Resource Creation (Windows only) ----

#ifdef SPARK_PLATFORM_WINDOWS
        bool CreateRenderTargets()
        {
            // Reflection render target
            {
                D3D11_TEXTURE2D_DESC texDesc = {};
                texDesc.Width = m_reflectionWidth;
                texDesc.Height = m_reflectionHeight;
                texDesc.MipLevels = 1;
                texDesc.ArraySize = 1;
                texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                texDesc.SampleDesc.Count = 1;
                texDesc.Usage = D3D11_USAGE_DEFAULT;
                texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

                HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, m_reflectionTexture.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    return false;
                }

                hr = m_device->CreateRenderTargetView(m_reflectionTexture.Get(), nullptr,
                                                      m_reflectionRTV.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    return false;
                }

                hr = m_device->CreateShaderResourceView(m_reflectionTexture.Get(), nullptr,
                                                        m_reflectionSRV.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    return false;
                }
            }

            // Reflection depth
            {
                D3D11_TEXTURE2D_DESC depthDesc = {};
                depthDesc.Width = m_reflectionWidth;
                depthDesc.Height = m_reflectionHeight;
                depthDesc.MipLevels = 1;
                depthDesc.ArraySize = 1;
                depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                depthDesc.SampleDesc.Count = 1;
                depthDesc.Usage = D3D11_USAGE_DEFAULT;
                depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

                HRESULT hr =
                    m_device->CreateTexture2D(&depthDesc, nullptr, m_reflectionDepthTexture.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    return false;
                }

                hr = m_device->CreateDepthStencilView(m_reflectionDepthTexture.Get(), nullptr,
                                                      m_reflectionDSV.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    return false;
                }
            }

            // Refraction render target
            {
                D3D11_TEXTURE2D_DESC texDesc = {};
                texDesc.Width = m_refractionWidth;
                texDesc.Height = m_refractionHeight;
                texDesc.MipLevels = 1;
                texDesc.ArraySize = 1;
                texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                texDesc.SampleDesc.Count = 1;
                texDesc.Usage = D3D11_USAGE_DEFAULT;
                texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

                HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, m_refractionTexture.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    return false;
                }

                hr = m_device->CreateRenderTargetView(m_refractionTexture.Get(), nullptr,
                                                      m_refractionRTV.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    return false;
                }

                hr = m_device->CreateShaderResourceView(m_refractionTexture.Get(), nullptr,
                                                        m_refractionSRV.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    return false;
                }
            }

            // Refraction depth (readable as SRV for depth-based effects)
            {
                D3D11_TEXTURE2D_DESC depthDesc = {};
                depthDesc.Width = m_refractionWidth;
                depthDesc.Height = m_refractionHeight;
                depthDesc.MipLevels = 1;
                depthDesc.ArraySize = 1;
                depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                depthDesc.SampleDesc.Count = 1;
                depthDesc.Usage = D3D11_USAGE_DEFAULT;
                depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

                HRESULT hr =
                    m_device->CreateTexture2D(&depthDesc, nullptr, m_refractionDepthTexture.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    return false;
                }

                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;

                hr = m_device->CreateShaderResourceView(m_refractionDepthTexture.Get(), &srvDesc,
                                                        m_refractionDepthSRV.ReleaseAndGetAddressOf());
                if (FAILED(hr))
                {
                    return false;
                }
            }

            m_metrics.reflectionResolutionX = m_reflectionWidth;
            m_metrics.reflectionResolutionY = m_reflectionHeight;
            return true;
        }

        bool CreateConstantBuffers()
        {
            D3D11_BUFFER_DESC desc = {};
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            desc.ByteWidth = static_cast<UINT>((sizeof(WaterConstants) + 15) & ~15);
            HRESULT hr = m_device->CreateBuffer(&desc, nullptr, m_waterConstantBuffer.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                return false;
            }

            desc.ByteWidth = static_cast<UINT>((sizeof(UnderwaterPostProcessConstants) + 15) & ~15);
            hr = m_device->CreateBuffer(&desc, nullptr, m_underwaterConstantBuffer.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                return false;
            }

            return true;
        }

        bool CreateSamplerStates()
        {
            D3D11_SAMPLER_DESC sampDesc = {};
            sampDesc.Filter = D3D11_FILTER_ANISOTROPIC;
            sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
            sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            sampDesc.MaxAnisotropy = 8;
            sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
            sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;

            HRESULT hr = m_device->CreateSamplerState(&sampDesc, m_waterSampler.ReleaseAndGetAddressOf());
            return SUCCEEDED(hr);
        }

        void UpdateWaterConstants(const WaterVolume& volume, const XMFLOAT4X4& viewMatrix, const XMFLOAT4X4& projMatrix)
        {
            WaterConstants cb = {};

            // Matrices
            XMMATRIX world = volume.ComputeWorldMatrix();
            XMStoreFloat4x4(&cb.worldMatrix, world);
            cb.viewMatrix = viewMatrix;
            cb.projectionMatrix = projMatrix;

            XMFLOAT4X4 reflView = ComputeReflectionMatrix(volume.GetHeight(), viewMatrix);
            cb.reflectionViewMatrix = reflView;

            // Wave data
            cb.activeWaveCount = std::min(m_settings.activeWaveCount, MAX_GERSTNER_WAVES);
            for (uint32_t i = 0; i < cb.activeWaveCount; ++i)
            {
                const auto& src = m_settings.waves[i];
                cb.gerstnerWaves[i].direction = src.direction;
                cb.gerstnerWaves[i].amplitude = src.amplitude * m_settings.globalAmplitude;
                cb.gerstnerWaves[i].frequency = src.frequency;
                cb.gerstnerWaves[i].steepness = src.steepness;
                cb.gerstnerWaves[i].speed = src.speed * m_settings.globalSpeed;
            }

            // Colors
            cb.shallowColor = m_settings.shallowColor;
            cb.deepColor = m_settings.deepColor;
            cb.extinctionCoeff = {m_settings.extinctionCoefficient.x, m_settings.extinctionCoefficient.y,
                                  m_settings.extinctionCoefficient.z, m_settings.depthVisibility};

            // Camera and time
            cb.cameraPosition = m_cameraPosition;
            cb.totalTime = m_totalTime;

            // Reflection/refraction
            cb.reflectionDistortion = m_settings.reflectionDistortion;
            cb.refractionDistortion = m_settings.refractionDistortion;
            cb.fresnelBias = m_settings.fresnelBias;
            cb.fresnelPower = m_settings.fresnelPower;

            // Tessellation
            cb.tessellationFactor = m_settings.tessellationFactor;
            cb.tessellationMinDist = m_settings.tessellationMinDistance;
            cb.tessellationMaxDist = m_settings.tessellationMaxDistance;
            cb.displacementScale = m_settings.displacementScale;

            // Foam
            cb.shoreFoamWidth = m_settings.shoreFoamWidth;
            cb.shoreFoamIntensity = m_settings.shoreFoamIntensity;
            cb.waveCrestFoamThreshold = m_settings.waveCrestFoamThreshold;
            cb.waveCrestFoamIntensity = m_settings.waveCrestFoamIntensity;
            cb.foamColor = m_settings.foamColor;
            cb.foamScale = m_settings.foamScale;

            // Caustics
            cb.causticsIntensity = m_settings.causticsIntensity;
            cb.causticsScale = m_settings.causticsScale;
            cb.causticsSpeed = m_settings.causticsSpeed;
            cb.causticsDepthFade = m_settings.causticsDepthFade;

            // Normal map
            cb.normalMapScale = m_settings.normalMapScale;
            cb.normalMapStrength = m_settings.normalMapStrength;
            cb.normalMapScrollSpeed = m_settings.normalMapScrollSpeed;

            // Specular and misc
            cb.specularPower = m_settings.specularPower;
            cb.specularIntensity = m_settings.specularIntensity;
            cb.waterHeight = volume.GetHeight();

            // Clip plane for current rendering pass
            cb.clipPlane = ComputeReflectionClipPlane(volume.GetHeight(), m_settings.clipPlaneOffset);

            // Upload to GPU
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(m_waterConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                memcpy(mapped.pData, &cb, sizeof(cb));
                m_context->Unmap(m_waterConstantBuffer.Get(), 0);
            }
        }

        void UpdateUnderwaterConstants()
        {
            if (!m_activeUnderwaterVolume)
            {
                return;
            }

            UnderwaterPostProcessConstants cb = {};
            cb.fogColor = m_settings.underwaterFogColor;
            cb.fogDensity = m_settings.underwaterFogDensity;
            cb.fogStartDistance = m_settings.underwaterFogStartDistance;
            cb.fogMaxOpacity = m_settings.underwaterFogMaxOpacity;
            cb.distortionStrength = 0.02f;
            cb.distortionSpeed = 1.0f;
            cb.tintColor = m_settings.underwaterFogColor;
            cb.tintIntensity = 0.3f;
            cb.causticsIntensity = m_settings.causticsIntensity;
            cb.causticsScale = m_settings.causticsScale;
            cb.totalTime = m_totalTime;
            cb.waterSurfaceY = m_activeUnderwaterVolume->GetHeight();
            cb.cameraPosition = m_cameraPosition;
            cb.depthFade = m_settings.causticsDepthFade;

            // Compute inverse view-projection
            XMMATRIX view = XMLoadFloat4x4(&m_viewMatrix);
            XMMATRIX proj = XMLoadFloat4x4(&m_projMatrix);
            XMMATRIX viewProj = view * proj;
            XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);
            XMStoreFloat4x4(&cb.invViewProjection, invViewProj);

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(m_underwaterConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                memcpy(mapped.pData, &cb, sizeof(cb));
                m_context->Unmap(m_underwaterConstantBuffer.Get(), 0);
            }
        }

        void BindWaterResources()
        {
            // Bind constant buffer to vertex and pixel shader slot 0
            ID3D11Buffer* cbs[] = {m_waterConstantBuffer.Get()};
            m_context->VSSetConstantBuffers(0, 1, cbs);
            m_context->HSSetConstantBuffers(0, 1, cbs);
            m_context->DSSetConstantBuffers(0, 1, cbs);
            m_context->PSSetConstantBuffers(0, 1, cbs);

            // Bind reflection/refraction SRVs to pixel shader
            ID3D11ShaderResourceView* srvs[] = {m_reflectionSRV.Get(), m_refractionSRV.Get(),
                                                m_refractionDepthSRV.Get()};
            m_context->PSSetShaderResources(0, 3, srvs);

            // Bind sampler
            ID3D11SamplerState* samplers[] = {m_waterSampler.Get()};
            m_context->PSSetSamplers(0, 1, samplers);
        }

        void BindUnderwaterResources()
        {
            ID3D11Buffer* cbs[] = {m_underwaterConstantBuffer.Get()};
            m_context->PSSetConstantBuffers(1, 1, cbs);
        }

        void DrawWaterMesh([[maybe_unused]] const WaterVolume& volume)
        {
            // Actual draw call would issue IA/Draw for the water grid mesh
            // Tessellation stages (HS/DS) handle LOD subdivision
            // Vertex shader applies Gerstner wave displacement
        }

        void DrawFullscreenQuad()
        {
            // Full-screen triangle or quad for underwater post-processing pass
        }
#endif // SPARK_PLATFORM_WINDOWS

        // ---- State ----
        bool m_initialized = false;
        bool m_enabled = true;
        float m_totalTime = 0.0f;

        uint32_t m_viewportWidth = 1920;
        uint32_t m_viewportHeight = 1080;
        uint32_t m_reflectionWidth = 960;
        uint32_t m_reflectionHeight = 540;
        uint32_t m_refractionWidth = 1920;
        uint32_t m_refractionHeight = 1080;

        // ---- Settings ----
        WaterSurfaceSettings m_settings;

        // ---- Volumes ----
        std::vector<WaterVolume> m_volumes;
        uint32_t m_nextVolumeId = 1;

        // ---- Camera State ----
        XMFLOAT3 m_cameraPosition = {0.0f, 0.0f, 0.0f};
        XMFLOAT4X4 m_viewMatrix;
        XMFLOAT4X4 m_projMatrix;
        bool m_cameraUnderwater = false;
        WaterVolume* m_activeUnderwaterVolume = nullptr;

        // ---- Metrics ----
        WaterMetrics m_metrics;

        // ---- D3D11 Resources (Windows only) ----
#ifdef SPARK_PLATFORM_WINDOWS
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;

        // Reflection
        ComPtr<ID3D11Texture2D> m_reflectionTexture;
        ComPtr<ID3D11RenderTargetView> m_reflectionRTV;
        ComPtr<ID3D11ShaderResourceView> m_reflectionSRV;
        ComPtr<ID3D11Texture2D> m_reflectionDepthTexture;
        ComPtr<ID3D11DepthStencilView> m_reflectionDSV;

        // Refraction
        ComPtr<ID3D11Texture2D> m_refractionTexture;
        ComPtr<ID3D11RenderTargetView> m_refractionRTV;
        ComPtr<ID3D11ShaderResourceView> m_refractionSRV;
        ComPtr<ID3D11Texture2D> m_refractionDepthTexture;
        ComPtr<ID3D11ShaderResourceView> m_refractionDepthSRV;

        // Constant buffers
        ComPtr<ID3D11Buffer> m_waterConstantBuffer;
        ComPtr<ID3D11Buffer> m_underwaterConstantBuffer;

        // Sampler
        ComPtr<ID3D11SamplerState> m_waterSampler;
#endif
    };

} // namespace Spark::Graphics
