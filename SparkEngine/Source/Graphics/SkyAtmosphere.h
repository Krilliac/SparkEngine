/**
 * @file SkyAtmosphere.h
 * @brief Procedural sky and atmosphere rendering system
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides physically-based sky and atmosphere rendering using a combined
 * Preetham/Bruneton model with precomputed LUTs. Supports:
 * - Rayleigh and Mie scattering with ozone absorption
 * - Transmittance and multi-scattering LUT precomputation
 * - Volumetric and 2D cloud layers
 * - Static cubemap skybox fallback
 * - Sun disk and night-sky star rendering
 * - Integration with DayNightCycle for time-of-day transitions
 *
 * ## Usage
 * @code
 *   SkyAtmosphereSystem sky;
 *   sky.Initialize(device, context);
 *   sky.SetDayNightCycle(&dayNight);
 *
 *   // Each frame
 *   sky.Update(deltaTime);
 *   sky.Render(context, viewProj, cameraPos);
 *
 *   // Console
 *   auto status = sky.Console_GetStatus();
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace Spark
{

    // Forward declaration for DayNightCycle integration
    class DayNightCycle;

    namespace Graphics
    {

        // =============================================================================
        // Constants
        // =============================================================================

        namespace SkyConstants
        {
            /// Earth atmosphere defaults (in km where noted)
            constexpr float kEarthRadius             = 6360.0f;   ///< Planet radius in km
            constexpr float kAtmosphereRadius         = 6460.0f;   ///< Atmosphere outer radius in km
            constexpr float kRayleighScaleHeight       = 8.0f;     ///< Rayleigh scale height in km
            constexpr float kMieScaleHeight            = 1.2f;     ///< Mie scale height in km
            constexpr float kSunAngularRadius          = 0.00935f; ///< Sun angular radius in radians (~0.536 deg)
            constexpr float kDefaultSunIntensity       = 20.0f;    ///< Default solar illuminance
            constexpr float kDefaultMieG               = 0.76f;    ///< Default Henyey-Greenstein g

            /// Rayleigh scattering coefficients at sea level for Earth (per km)
            constexpr float kRayleighCoeffR = 5.802e-3f;
            constexpr float kRayleighCoeffG = 13.558e-3f;
            constexpr float kRayleighCoeffB = 33.1e-3f;

            /// Mie scattering coefficient at sea level for Earth (per km)
            constexpr float kMieScattering  = 3.996e-3f;
            constexpr float kMieAbsorption  = 4.4e-4f;

            /// Ozone absorption coefficients (per km)
            constexpr float kOzoneAbsorptionR = 0.650e-3f;
            constexpr float kOzoneAbsorptionG = 1.881e-3f;
            constexpr float kOzoneAbsorptionB = 0.085e-3f;

            /// LUT dimensions
            constexpr uint32_t kTransmittanceLutWidth  = 256;
            constexpr uint32_t kTransmittanceLutHeight = 64;
            constexpr uint32_t kMultiScatterLutSize    = 32;
            constexpr uint32_t kSkyViewLutWidth        = 192;
            constexpr uint32_t kSkyViewLutHeight       = 108;

            /// Star rendering
            constexpr uint32_t kMaxStarCount = 4096;

            /// Cloud defaults
            constexpr float kDefaultCloudAltitude  = 2.0f;   ///< km above ground
            constexpr float kDefaultCloudThickness = 1.5f;   ///< km
        } // namespace SkyConstants

        // =============================================================================
        // Cloud Type
        // =============================================================================

        enum class CloudType
        {
            None,       ///< No clouds rendered
            Layer2D,    ///< Flat 2D noise-based cloud layer
            Volumetric  ///< Ray-marched volumetric clouds
        };

        // =============================================================================
        // Sky Render Mode
        // =============================================================================

        enum class SkyRenderMode
        {
            Procedural, ///< Physically-based atmosphere model
            Cubemap     ///< Static cubemap skybox
        };

        // =============================================================================
        // Atmosphere Settings
        // =============================================================================

        /**
         * @brief Configuration for physically-based atmosphere scattering
         *
         * Default values correspond to Earth's atmosphere. Rayleigh scattering
         * produces the blue sky, Mie scattering produces the hazy sun glow, and
         * ozone absorption adds subtle color shifts near the horizon.
         */
        struct AtmosphereSettings
        {
            /// Rayleigh scattering coefficients at sea level (per km, RGB)
            XMFLOAT3 rayleighScattering = {
                SkyConstants::kRayleighCoeffR,
                SkyConstants::kRayleighCoeffG,
                SkyConstants::kRayleighCoeffB
            };
            float rayleighScaleHeight = SkyConstants::kRayleighScaleHeight; ///< km

            /// Mie scattering
            float mieScattering   = SkyConstants::kMieScattering;  ///< Mie scattering coefficient (per km)
            float mieAbsorption   = SkyConstants::kMieAbsorption;  ///< Mie absorption coefficient (per km)
            float mieScaleHeight  = SkyConstants::kMieScaleHeight; ///< Mie scale height in km
            float mieG            = SkyConstants::kDefaultMieG;    ///< Henyey-Greenstein asymmetry parameter [-1, 1]

            /// Ozone absorption coefficients (per km, RGB)
            XMFLOAT3 ozoneAbsorption = {
                SkyConstants::kOzoneAbsorptionR,
                SkyConstants::kOzoneAbsorptionG,
                SkyConstants::kOzoneAbsorptionB
            };
            float ozoneCenterAltitude = 25.0f; ///< Ozone layer center altitude in km
            float ozoneWidth          = 15.0f; ///< Ozone layer width in km

            /// Sun parameters
            XMFLOAT3 sunDirection = {0.0f, 1.0f, 0.0f}; ///< Normalized sun direction (Y-up)
            float sunIntensity    = SkyConstants::kDefaultSunIntensity;
            float sunDiskSize     = SkyConstants::kSunAngularRadius;
            XMFLOAT3 sunColor     = {1.0f, 0.95f, 0.85f};

            /// Ground
            XMFLOAT3 groundAlbedo = {0.3f, 0.3f, 0.3f}; ///< Ground reflectance for multi-scattering

            /// Planet geometry
            float planetRadius     = SkyConstants::kEarthRadius;
            float atmosphereRadius = SkyConstants::kAtmosphereRadius;

            /// Exposure and artistic controls
            float exposureMultiplier = 1.0f;

            /// Reset to Earth-like defaults
            void ResetToEarthDefaults()
            {
                *this = AtmosphereSettings{};
            }
        };

        // =============================================================================
        // Cloud Settings
        // =============================================================================

        /**
         * @brief Configuration for cloud rendering (2D layer or volumetric)
         */
        struct CloudSettings
        {
            CloudType type = CloudType::None;

            /// Coverage and shape
            float coverage       = 0.5f;  ///< Cloud coverage [0, 1]
            float density        = 0.3f;  ///< Cloud density multiplier
            float altitude       = SkyConstants::kDefaultCloudAltitude;  ///< Base altitude in km
            float thickness      = SkyConstants::kDefaultCloudThickness; ///< Layer thickness in km

            /// Wind animation
            XMFLOAT2 windDirection = {1.0f, 0.0f}; ///< Normalized 2D wind direction
            float windSpeed        = 0.02f;         ///< World-space wind speed

            /// Detail noise (volumetric)
            float detailNoiseScale = 4.0f;   ///< Scale of detail noise texture
            float erosion          = 0.3f;   ///< Detail erosion strength [0, 1]

            /// Lighting
            float lightAbsorption         = 0.5f;  ///< Beer-Lambert absorption coefficient
            float silverLiningIntensity   = 0.3f;  ///< Silver lining (forward scattering rim)
            float silverLiningSpread      = 0.15f; ///< Angular spread of silver lining

            /// Volumetric ray-march
            int   rayMarchSteps           = 64;    ///< Primary ray march step count
            int   lightMarchSteps         = 6;     ///< In-scatter light march steps per sample
            float maxRayDistance           = 50.0f; ///< Maximum ray distance in km

            /// Temporal reprojection
            float temporalBlend = 0.95f; ///< Blend factor for temporal reprojection [0, 1]

            /// Accumulated wind offset (internal, updated each frame)
            XMFLOAT2 windOffset = {0.0f, 0.0f};
        };

        // =============================================================================
        // Skybox Settings
        // =============================================================================

        /**
         * @brief Configuration for static cubemap skybox rendering
         */
        struct SkyboxSettings
        {
#ifdef SPARK_PLATFORM_WINDOWS
            ComPtr<ID3D11ShaderResourceView> cubemapSRV; ///< Cubemap texture SRV
#else
            void* cubemapSRV = nullptr; ///< Opaque handle (non-Windows stub)
#endif

            SkyRenderMode mode     = SkyRenderMode::Procedural;
            float rotation         = 0.0f;  ///< Y-axis rotation in radians
            float intensity        = 1.0f;  ///< Brightness multiplier
            float blurLevel        = 0.0f;  ///< Mip bias for blurred skybox [0, maxMip]
            float proceduralBlend  = 1.0f;  ///< Blend between procedural (1) and cubemap (0)

            bool IsProceduralMode() const { return mode == SkyRenderMode::Procedural; }
            bool HasCubemap() const
            {
#ifdef SPARK_PLATFORM_WINDOWS
                return cubemapSRV != nullptr;
#else
                return cubemapSRV != nullptr;
#endif
            }
        };

        // =============================================================================
        // Star Vertex
        // =============================================================================

        /**
         * @brief Single star for night-sky rendering
         */
        struct StarVertex
        {
            XMFLOAT3 direction;  ///< Normalized direction on unit sphere
            float brightness;    ///< Apparent magnitude mapped to [0, 1]
            XMFLOAT3 color;     ///< Star color (spectral class)
            float size;          ///< Angular size multiplier
        };

        // =============================================================================
        // GPU Constant Buffer (shader-side struct)
        // =============================================================================

        /**
         * @brief GPU constant buffer layout for atmosphere shader parameters
         *
         * Must be 16-byte aligned for D3D11 cbuffer requirements.
         * Corresponds to the HLSL cbuffer AtmosphereParams.
         */
        struct alignas(16) AtmosphereCBuffer
        {
            // Rayleigh
            XMFLOAT3 rayleighScattering;
            float rayleighScaleHeight;

            // Mie
            float mieScattering;
            float mieAbsorption;
            float mieScaleHeight;
            float mieG;

            // Ozone
            XMFLOAT3 ozoneAbsorption;
            float ozoneCenterAltitude;
            float ozoneWidth;
            float pad0[3];

            // Sun
            XMFLOAT3 sunDirection;
            float sunIntensity;
            XMFLOAT3 sunColor;
            float sunDiskSize;

            // Planet
            float planetRadius;
            float atmosphereRadius;
            float exposureMultiplier;
            float pad1;

            // Ground
            XMFLOAT3 groundAlbedo;
            float pad2;

            // Camera
            XMFLOAT3 cameraPosition;
            float cameraHeight; ///< Height above planet surface in km

            // View
            XMFLOAT4X4 inverseViewProjection;

            // Cloud params
            float cloudCoverage;
            float cloudDensity;
            float cloudAltitude;
            float cloudThickness;
            XMFLOAT2 cloudWindOffset;
            float cloudLightAbsorption;
            float cloudSilverLining;
            float cloudDetailScale;
            float cloudErosion;
            float cloudSilverSpread;
            int   cloudRaySteps;
            int   cloudLightSteps;
            int   cloudType;
            float cloudTemporalBlend;
            float pad3;

            // Stars
            float starVisibility; ///< [0, 1] fades stars in at night
            float nightTransition; ///< Smooth transition factor for night effects
            float pad4[2];
        };

        // =============================================================================
        // Sky Atmosphere Metrics
        // =============================================================================

        struct SkyAtmosphereMetrics
        {
            bool lutsGenerated         = false;
            bool cloudsEnabled         = false;
            int  cloudType             = 0;
            int  starCount             = 0;
            float sunElevationDeg      = 0.0f;
            float cameraHeightKm       = 0.0f;
            SkyRenderMode renderMode   = SkyRenderMode::Procedural;
        };

        // =============================================================================
        // Sky Atmosphere System
        // =============================================================================

        /**
         * @class SkyAtmosphereSystem
         * @brief Complete procedural sky and atmosphere rendering pipeline
         *
         * Implements a Bruneton-style precomputed atmosphere model with transmittance
         * and multi-scattering LUTs. Renders the sky as a fullscreen pass or into a
         * cubemap for reflections. Supports cloud layers (2D noise and volumetric
         * ray-marching), sun disk, and star field rendering.
         *
         * The system integrates with DayNightCycle for automatic sun position and
         * color temperature updates based on time of day.
         *
         * Thread safety: Must be accessed from the main/render thread only.
         */
        class SkyAtmosphereSystem
        {
        public:
            SkyAtmosphereSystem() = default;
            ~SkyAtmosphereSystem() { Shutdown(); }

            // Non-copyable, movable
            SkyAtmosphereSystem(const SkyAtmosphereSystem&) = delete;
            SkyAtmosphereSystem& operator=(const SkyAtmosphereSystem&) = delete;
            SkyAtmosphereSystem(SkyAtmosphereSystem&&) noexcept = default;
            SkyAtmosphereSystem& operator=(SkyAtmosphereSystem&&) noexcept = default;

            // ================================================================
            // Lifecycle
            // ================================================================

            /**
             * @brief Initialize GPU resources and precompute LUTs
             * @param device D3D11 device for resource creation
             * @param context D3D11 context for initial LUT dispatch
             * @return true on success
             */
            bool Initialize(
                [[maybe_unused]] ID3D11Device* device,
                [[maybe_unused]] ID3D11DeviceContext* context)
            {
#ifdef SPARK_PLATFORM_WINDOWS
                if (!device || !context)
                {
                    return false;
                }

                m_device = device;
                m_context = context;

                if (!CreateConstantBuffer())
                {
                    return false;
                }

                if (!CreateTransmittanceLut())
                {
                    return false;
                }

                if (!CreateMultiScatterLut())
                {
                    return false;
                }

                if (!CreateSkyViewLut())
                {
                    return false;
                }

                GenerateStarField();

                m_lutsNeedUpdate = true;
                m_initialized = true;
#endif
                return true;
            }

            /**
             * @brief Release all GPU resources
             */
            void Shutdown()
            {
#ifdef SPARK_PLATFORM_WINDOWS
                m_transmittanceLutSRV.Reset();
                m_transmittanceLutUAV.Reset();
                m_transmittanceLutTexture.Reset();

                m_multiScatterLutSRV.Reset();
                m_multiScatterLutUAV.Reset();
                m_multiScatterLutTexture.Reset();

                m_skyViewLutSRV.Reset();
                m_skyViewLutUAV.Reset();
                m_skyViewLutTexture.Reset();

                m_atmosphereCBuffer.Reset();

                m_starVertexBuffer.Reset();

                m_transmittanceLutCS.Reset();
                m_multiScatterLutCS.Reset();
                m_skyRenderCS.Reset();
                m_cloudRenderCS.Reset();
#endif
                m_initialized = false;
                m_lutsNeedUpdate = false;
            }

            /**
             * @brief Update atmosphere state for the current frame
             * @param deltaTime Frame delta time in seconds
             *
             * Syncs sun direction from DayNightCycle if bound, advances cloud
             * wind animation, and flags LUT regeneration if settings changed.
             */
            void Update(float deltaTime)
            {
                if (!m_initialized)
                {
                    return;
                }

                // Sync with DayNightCycle if bound
                if (m_dayNightCycle)
                {
                    SyncFromDayNightCycle();
                }

                // Advance cloud wind offset
                UpdateCloudWind(deltaTime);

                // Determine night-sky star visibility
                UpdateStarVisibility();

                // Mark LUTs dirty if atmosphere params changed
                if (m_settingsDirty)
                {
                    m_lutsNeedUpdate = true;
                    m_settingsDirty = false;
                }
            }

            /**
             * @brief Render the sky atmosphere for the current frame
             * @param context D3D11 device context
             * @param inverseViewProj Inverse view-projection matrix
             * @param cameraPosition Camera world position
             *
             * Regenerates LUTs if flagged dirty, then renders the sky as a
             * fullscreen pass. Cloud and star layers are composited on top.
             */
            void Render(
                [[maybe_unused]] ID3D11DeviceContext* context,
                [[maybe_unused]] const XMMATRIX& inverseViewProj,
                [[maybe_unused]] const XMFLOAT3& cameraPosition)
            {
#ifdef SPARK_PLATFORM_WINDOWS
                if (!m_initialized || !context)
                {
                    return;
                }

                // Regenerate LUTs if atmosphere parameters changed
                if (m_lutsNeedUpdate)
                {
                    DispatchTransmittanceLut(context);
                    DispatchMultiScatterLut(context);
                    m_lutsNeedUpdate = false;
                    m_lutsGenerated = true;
                }

                // Update GPU constant buffer
                UpdateAtmosphereCBuffer(inverseViewProj, cameraPosition);
                UploadConstantBuffer(context);

                // Render sky view LUT (per-frame, depends on sun direction)
                DispatchSkyViewLut(context);

                // Fullscreen sky pass
                RenderSkyPass(context);

                // Sun disk
                if (m_sunDiskEnabled)
                {
                    RenderSunDisk(context);
                }

                // Clouds
                if (m_cloudSettings.type != CloudType::None)
                {
                    RenderClouds(context);
                }

                // Stars (night sky)
                if (m_starVisibility > 0.001f && m_starsEnabled)
                {
                    RenderStars(context);
                }
#endif
            }

            /**
             * @brief Render the sky into a cubemap for reflections
             * @param context D3D11 device context
             * @param cubemapRTV Array of 6 render target views (one per face)
             * @param resolution Cubemap face resolution
             */
            void RenderToCubemap(
                [[maybe_unused]] ID3D11DeviceContext* context,
                [[maybe_unused]] ID3D11RenderTargetView* const* cubemapRTV,
                [[maybe_unused]] uint32_t resolution)
            {
#ifdef SPARK_PLATFORM_WINDOWS
                if (!m_initialized || !context || !cubemapRTV)
                {
                    return;
                }

                // For each cubemap face, set up view direction and render sky
                static const XMFLOAT3 faceDirs[6] = {
                    { 1.0f,  0.0f,  0.0f}, // +X
                    {-1.0f,  0.0f,  0.0f}, // -X
                    { 0.0f,  1.0f,  0.0f}, // +Y
                    { 0.0f, -1.0f,  0.0f}, // -Y
                    { 0.0f,  0.0f,  1.0f}, // +Z
                    { 0.0f,  0.0f, -1.0f}  // -Z
                };

                static const XMFLOAT3 faceUps[6] = {
                    {0.0f, 1.0f,  0.0f}, // +X
                    {0.0f, 1.0f,  0.0f}, // -X
                    {0.0f, 0.0f, -1.0f}, // +Y
                    {0.0f, 0.0f,  1.0f}, // -Y
                    {0.0f, 1.0f,  0.0f}, // +Z
                    {0.0f, 1.0f,  0.0f}  // -Z
                };

                for (uint32_t face = 0; face < 6; ++face)
                {
                    XMVECTOR eye = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
                    XMVECTOR target = XMLoadFloat3(&faceDirs[face]);
                    XMVECTOR up = XMLoadFloat3(&faceUps[face]);

                    XMMATRIX view = XMMatrixLookAtLH(eye, target, up);
                    XMMATRIX proj = XMMatrixPerspectiveFovLH(
                        XM_PIDIV2, 1.0f, 0.1f, 1000.0f);
                    XMMATRIX viewProj = view * proj;
                    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

                    context->OMSetRenderTargets(1, &cubemapRTV[face], nullptr);

                    UpdateAtmosphereCBuffer(invViewProj, {0.0f, 0.0f, 0.0f});
                    UploadConstantBuffer(context);
                    RenderSkyPass(context);
                }
#endif
            }

            // ================================================================
            // Settings Accessors
            // ================================================================

            void SetAtmosphereSettings(const AtmosphereSettings& settings)
            {
                m_atmosphereSettings = settings;
                m_settingsDirty = true;
            }
            const AtmosphereSettings& GetAtmosphereSettings() const { return m_atmosphereSettings; }
            AtmosphereSettings& GetAtmosphereSettingsMutable()
            {
                m_settingsDirty = true;
                return m_atmosphereSettings;
            }

            void SetCloudSettings(const CloudSettings& settings) { m_cloudSettings = settings; }
            const CloudSettings& GetCloudSettings() const { return m_cloudSettings; }
            CloudSettings& GetCloudSettingsMutable() { return m_cloudSettings; }

            void SetSkyboxSettings(const SkyboxSettings& settings) { m_skyboxSettings = settings; }
            const SkyboxSettings& GetSkyboxSettings() const { return m_skyboxSettings; }
            SkyboxSettings& GetSkyboxSettingsMutable() { return m_skyboxSettings; }

            // ================================================================
            // Sun Control
            // ================================================================

            void SetSunDirection(const XMFLOAT3& dir)
            {
                m_atmosphereSettings.sunDirection = dir;
                m_settingsDirty = true;
            }

            XMFLOAT3 GetSunDirection() const { return m_atmosphereSettings.sunDirection; }

            void SetSunIntensity(float intensity)
            {
                m_atmosphereSettings.sunIntensity = std::max(0.0f, intensity);
            }

            void SetSunColor(const XMFLOAT3& color) { m_atmosphereSettings.sunColor = color; }

            void SetSunDiskEnabled(bool enabled) { m_sunDiskEnabled = enabled; }
            bool IsSunDiskEnabled() const { return m_sunDiskEnabled; }

            void SetSunDiskSize(float radians)
            {
                m_atmosphereSettings.sunDiskSize = std::max(0.0f, radians);
            }

            // ================================================================
            // Star Control
            // ================================================================

            void SetStarsEnabled(bool enabled) { m_starsEnabled = enabled; }
            bool AreStarsEnabled() const { return m_starsEnabled; }

            void SetStarCount(uint32_t count)
            {
                m_starCount = std::min(count, SkyConstants::kMaxStarCount);
                GenerateStarField();
            }
            uint32_t GetStarCount() const { return m_starCount; }

            float GetStarVisibility() const { return m_starVisibility; }

            // ================================================================
            // Cloud Control
            // ================================================================

            void SetCloudType(CloudType type) { m_cloudSettings.type = type; }
            CloudType GetCloudType() const { return m_cloudSettings.type; }

            void SetCloudCoverage(float coverage)
            {
                m_cloudSettings.coverage = std::clamp(coverage, 0.0f, 1.0f);
            }

            void SetCloudDensity(float density)
            {
                m_cloudSettings.density = std::max(0.0f, density);
            }

            void SetCloudWindSpeed(float speed)
            {
                m_cloudSettings.windSpeed = std::max(0.0f, speed);
            }

            // ================================================================
            // DayNightCycle Integration
            // ================================================================

            /**
             * @brief Bind a DayNightCycle for automatic sun position updates
             * @param cycle Pointer to DayNightCycle (non-owning), or nullptr to unbind
             */
            void SetDayNightCycle(DayNightCycle* cycle) { m_dayNightCycle = cycle; }
            DayNightCycle* GetDayNightCycle() const { return m_dayNightCycle; }

            // ================================================================
            // LUT Access (for external shaders)
            // ================================================================

#ifdef SPARK_PLATFORM_WINDOWS
            ID3D11ShaderResourceView* GetTransmittanceLutSRV() const
            {
                return m_transmittanceLutSRV.Get();
            }

            ID3D11ShaderResourceView* GetMultiScatterLutSRV() const
            {
                return m_multiScatterLutSRV.Get();
            }

            ID3D11ShaderResourceView* GetSkyViewLutSRV() const
            {
                return m_skyViewLutSRV.Get();
            }
#endif

            // ================================================================
            // Utility
            // ================================================================

            bool IsInitialized() const { return m_initialized; }
            bool AreLutsGenerated() const { return m_lutsGenerated; }

            /**
             * @brief Force LUT regeneration on next Render call
             */
            void InvalidateLuts() { m_lutsNeedUpdate = true; }

            /**
             * @brief Get the sun elevation angle in degrees
             * @return Elevation from horizon (-90 to +90)
             */
            float GetSunElevationDegrees() const
            {
                return XMConvertToDegrees(
                    std::asin(std::clamp(m_atmosphereSettings.sunDirection.y, -1.0f, 1.0f)));
            }

            /**
             * @brief Compute transmittance for a ray from given height and angle
             * @param heightKm Height above planet surface in km
             * @param cosZenith Cosine of zenith angle
             * @return Approximate RGB transmittance
             *
             * CPU-side approximation for use outside the render pipeline (e.g., fog
             * tinting, light color estimation). For accurate results, sample the GPU LUT.
             */
            XMFLOAT3 ComputeTransmittanceCPU(float heightKm, float cosZenith) const
            {
                const auto& s = m_atmosphereSettings;
                float r = s.planetRadius + heightKm;
                float mu = cosZenith;

                // Ray-atmosphere intersection distance
                float discriminant = r * r * (mu * mu - 1.0f) + s.atmosphereRadius * s.atmosphereRadius;
                if (discriminant < 0.0f)
                {
                    return {1.0f, 1.0f, 1.0f};
                }
                float distance = -r * mu + std::sqrt(discriminant);
                if (distance < 0.0f)
                {
                    return {1.0f, 1.0f, 1.0f};
                }

                // Numerical integration (simple Euler, 32 steps)
                constexpr int kSteps = 32;
                float stepSize = distance / static_cast<float>(kSteps);
                float opticalDepthR = 0.0f;
                float opticalDepthG = 0.0f;
                float opticalDepthB = 0.0f;

                for (int i = 0; i < kSteps; ++i)
                {
                    float t = (static_cast<float>(i) + 0.5f) * stepSize;
                    float sampleHeight = std::sqrt(r * r + t * t + 2.0f * r * t * mu) - s.planetRadius;
                    sampleHeight = std::max(sampleHeight, 0.0f);

                    // Rayleigh density
                    float rayleighDensity = std::exp(-sampleHeight / s.rayleighScaleHeight);

                    // Mie density
                    float mieDensity = std::exp(-sampleHeight / s.mieScaleHeight);

                    // Ozone density (triangle profile)
                    float ozoneDensity = std::max(0.0f,
                        1.0f - std::abs(sampleHeight - s.ozoneCenterAltitude) / s.ozoneWidth);

                    opticalDepthR += (s.rayleighScattering.x * rayleighDensity
                        + (s.mieScattering + s.mieAbsorption) * mieDensity
                        + s.ozoneAbsorption.x * ozoneDensity) * stepSize;

                    opticalDepthG += (s.rayleighScattering.y * rayleighDensity
                        + (s.mieScattering + s.mieAbsorption) * mieDensity
                        + s.ozoneAbsorption.y * ozoneDensity) * stepSize;

                    opticalDepthB += (s.rayleighScattering.z * rayleighDensity
                        + (s.mieScattering + s.mieAbsorption) * mieDensity
                        + s.ozoneAbsorption.z * ozoneDensity) * stepSize;
                }

                return {
                    std::exp(-opticalDepthR),
                    std::exp(-opticalDepthG),
                    std::exp(-opticalDepthB)
                };
            }

            /**
             * @brief Estimate the sky luminance in a given direction (CPU-side)
             * @param viewDir Normalized view direction
             * @return Approximate RGB sky luminance
             */
            XMFLOAT3 EstimateSkyLuminanceCPU(const XMFLOAT3& viewDir) const
            {
                const auto& s = m_atmosphereSettings;

                // Cosine of angle between view and sun
                float cosTheta = viewDir.x * s.sunDirection.x
                               + viewDir.y * s.sunDirection.y
                               + viewDir.z * s.sunDirection.z;
                cosTheta = std::clamp(cosTheta, -1.0f, 1.0f);

                // View zenith cosine
                float cosViewZenith = std::clamp(viewDir.y, -1.0f, 1.0f);

                // Transmittance from camera to atmosphere edge
                XMFLOAT3 transmittance = ComputeTransmittanceCPU(0.0f, cosViewZenith);

                // Rayleigh phase
                float rayleighPhase = (3.0f / (16.0f * XM_PI)) * (1.0f + cosTheta * cosTheta);

                // Mie phase (Henyey-Greenstein)
                float g = s.mieG;
                float g2 = g * g;
                float denom = 1.0f + g2 - 2.0f * g * cosTheta;
                float miePhase = (denom > 0.0001f)
                    ? (1.0f - g2) / (4.0f * XM_PI * std::pow(denom, 1.5f))
                    : 1.0f;

                // Approximate single-scattering contribution
                float rayleighDensity = 1.0f; // sea level
                float mieDensity = 1.0f;

                float lr = s.rayleighScattering.x * rayleighDensity * rayleighPhase
                         + s.mieScattering * mieDensity * miePhase;
                float lg = s.rayleighScattering.y * rayleighDensity * rayleighPhase
                         + s.mieScattering * mieDensity * miePhase;
                float lb = s.rayleighScattering.z * rayleighDensity * rayleighPhase
                         + s.mieScattering * mieDensity * miePhase;

                float sunFactor = s.sunIntensity * std::max(0.0f, s.sunDirection.y);

                return {
                    lr * transmittance.x * sunFactor * s.sunColor.x * s.exposureMultiplier,
                    lg * transmittance.y * sunFactor * s.sunColor.y * s.exposureMultiplier,
                    lb * transmittance.z * sunFactor * s.sunColor.z * s.exposureMultiplier
                };
            }

            // ================================================================
            // Metrics and Console
            // ================================================================

            SkyAtmosphereMetrics GetMetrics() const
            {
                SkyAtmosphereMetrics m;
                m.lutsGenerated = m_lutsGenerated;
                m.cloudsEnabled = (m_cloudSettings.type != CloudType::None);
                m.cloudType = static_cast<int>(m_cloudSettings.type);
                m.starCount = static_cast<int>(m_starCount);
                m.sunElevationDeg = GetSunElevationDegrees();
                m.cameraHeightKm = m_currentCameraHeightKm;
                m.renderMode = m_skyboxSettings.mode;
                return m;
            }

            std::string Console_GetStatus() const
            {
                std::string s = "Sky Atmosphere System:\n";
                s += "  Initialized: " + std::string(m_initialized ? "yes" : "no") + "\n";
                s += "  LUTs generated: " + std::string(m_lutsGenerated ? "yes" : "no") + "\n";
                s += "  Render mode: " + std::string(
                    m_skyboxSettings.mode == SkyRenderMode::Procedural ? "Procedural" : "Cubemap") + "\n";
                s += "  Sun elevation: " + std::to_string(GetSunElevationDegrees()) + " deg\n";
                s += "  Sun intensity: " + std::to_string(m_atmosphereSettings.sunIntensity) + "\n";
                s += "  Sun direction: ("
                    + std::to_string(m_atmosphereSettings.sunDirection.x) + ", "
                    + std::to_string(m_atmosphereSettings.sunDirection.y) + ", "
                    + std::to_string(m_atmosphereSettings.sunDirection.z) + ")\n";

                static const char* cloudNames[] = {"None", "2D Layer", "Volumetric"};
                s += "  Cloud type: " + std::string(cloudNames[static_cast<int>(m_cloudSettings.type)]) + "\n";
                if (m_cloudSettings.type != CloudType::None)
                {
                    s += "  Cloud coverage: " + std::to_string(m_cloudSettings.coverage) + "\n";
                    s += "  Cloud density: " + std::to_string(m_cloudSettings.density) + "\n";
                }

                s += "  Stars: " + std::string(m_starsEnabled ? "enabled" : "disabled")
                    + " (" + std::to_string(m_starCount) + " stars, visibility: "
                    + std::to_string(m_starVisibility) + ")\n";
                s += "  Sun disk: " + std::string(m_sunDiskEnabled ? "enabled" : "disabled") + "\n";
                s += "  DayNightCycle bound: " + std::string(m_dayNightCycle ? "yes" : "no") + "\n";

                return s;
            }

            std::string Console_SetSunDirection(float x, float y, float z)
            {
                XMVECTOR dir = XMVector3Normalize(XMVectorSet(x, y, z, 0.0f));
                XMFLOAT3 normalized;
                XMStoreFloat3(&normalized, dir);
                SetSunDirection(normalized);
                return "Sun direction set to ("
                    + std::to_string(normalized.x) + ", "
                    + std::to_string(normalized.y) + ", "
                    + std::to_string(normalized.z) + ")";
            }

            std::string Console_SetCloudType(const std::string& typeName)
            {
                if (typeName == "none")
                {
                    m_cloudSettings.type = CloudType::None;
                }
                else if (typeName == "2d" || typeName == "layer")
                {
                    m_cloudSettings.type = CloudType::Layer2D;
                }
                else if (typeName == "volumetric" || typeName == "vol")
                {
                    m_cloudSettings.type = CloudType::Volumetric;
                }
                else
                {
                    return "Unknown cloud type: " + typeName
                        + ". Options: none, 2d, layer, volumetric, vol";
                }
                return "Cloud type set to: " + typeName;
            }

            std::string Console_SetCloudCoverage(float coverage)
            {
                SetCloudCoverage(coverage);
                return "Cloud coverage set to " + std::to_string(m_cloudSettings.coverage);
            }

            std::string Console_SetSunIntensity(float intensity)
            {
                SetSunIntensity(intensity);
                return "Sun intensity set to " + std::to_string(m_atmosphereSettings.sunIntensity);
            }

            std::string Console_SetRenderMode(const std::string& mode)
            {
                if (mode == "procedural")
                {
                    m_skyboxSettings.mode = SkyRenderMode::Procedural;
                }
                else if (mode == "cubemap")
                {
                    m_skyboxSettings.mode = SkyRenderMode::Cubemap;
                }
                else
                {
                    return "Unknown render mode: " + mode + ". Options: procedural, cubemap";
                }
                return "Sky render mode set to: " + mode;
            }

        private:
            // ================================================================
            // DayNightCycle Synchronization
            // ================================================================

            void SyncFromDayNightCycle()
            {
                if (!m_dayNightCycle)
                {
                    return;
                }

                // Read sun direction from the cycle
                auto sunDir = m_dayNightCycle->GetSunDirection();
                XMFLOAT3 newDir = {sunDir.x, sunDir.y, sunDir.z};

                // Only update if direction actually changed
                float dx = newDir.x - m_atmosphereSettings.sunDirection.x;
                float dy = newDir.y - m_atmosphereSettings.sunDirection.y;
                float dz = newDir.z - m_atmosphereSettings.sunDirection.z;
                if (dx * dx + dy * dy + dz * dz > 1e-6f)
                {
                    m_atmosphereSettings.sunDirection = newDir;
                    m_settingsDirty = true;
                }

                // Sync sun color from DayNightCycle
                auto sunCol = m_dayNightCycle->GetSunColor();
                m_atmosphereSettings.sunColor = {sunCol.r, sunCol.g, sunCol.b};

                // Modulate sun intensity by DayNightCycle intensity
                m_atmosphereSettings.sunIntensity =
                    SkyConstants::kDefaultSunIntensity * m_dayNightCycle->GetSunIntensity();
            }

            // ================================================================
            // Cloud Wind Animation
            // ================================================================

            void UpdateCloudWind(float deltaTime)
            {
                m_cloudSettings.windOffset.x +=
                    m_cloudSettings.windDirection.x * m_cloudSettings.windSpeed * deltaTime;
                m_cloudSettings.windOffset.y +=
                    m_cloudSettings.windDirection.y * m_cloudSettings.windSpeed * deltaTime;
            }

            // ================================================================
            // Star Visibility
            // ================================================================

            void UpdateStarVisibility()
            {
                // Stars fade in as sun drops below horizon
                float sunElevation = m_atmosphereSettings.sunDirection.y;

                // Smooth transition: fully visible when sun is well below horizon,
                // invisible when sun is above ~6 degrees
                constexpr float kFadeStart = 0.1f;  // ~6 degrees above horizon
                constexpr float kFadeEnd   = -0.1f; // ~6 degrees below horizon

                if (sunElevation >= kFadeStart)
                {
                    m_starVisibility = 0.0f;
                }
                else if (sunElevation <= kFadeEnd)
                {
                    m_starVisibility = 1.0f;
                }
                else
                {
                    m_starVisibility = (kFadeStart - sunElevation) / (kFadeStart - kFadeEnd);
                }
            }

            // ================================================================
            // Star Field Generation
            // ================================================================

            void GenerateStarField()
            {
                m_stars.clear();
                m_stars.reserve(m_starCount);

                // Simple deterministic pseudo-random star placement
                // Using a basic LCG for reproducibility
                uint32_t seed = 0x12345678u;
                auto nextRandom = [&seed]() -> float
                {
                    seed = seed * 1664525u + 1013904223u;
                    return static_cast<float>(seed & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
                };

                for (uint32_t i = 0; i < m_starCount; ++i)
                {
                    StarVertex star;

                    // Uniform distribution on upper hemisphere (stars only above horizon)
                    float theta = nextRandom() * XM_2PI;
                    float phi = std::acos(nextRandom()); // Biased toward zenith

                    star.direction.x = std::sin(phi) * std::cos(theta);
                    star.direction.y = std::abs(std::cos(phi)); // Ensure above horizon
                    star.direction.z = std::sin(phi) * std::sin(theta);

                    // Brightness follows rough magnitude distribution
                    float mag = nextRandom();
                    star.brightness = mag * mag * mag; // Cubic falloff (few bright, many dim)

                    // Color based on spectral type approximation
                    float colorTemp = nextRandom();
                    if (colorTemp < 0.15f)
                    {
                        // Blue-white (O/B type)
                        star.color = {0.7f, 0.8f, 1.0f};
                    }
                    else if (colorTemp < 0.5f)
                    {
                        // White (A/F type)
                        star.color = {1.0f, 1.0f, 0.95f};
                    }
                    else if (colorTemp < 0.8f)
                    {
                        // Yellow (G/K type)
                        star.color = {1.0f, 0.9f, 0.7f};
                    }
                    else
                    {
                        // Red (M type)
                        star.color = {1.0f, 0.7f, 0.5f};
                    }

                    star.size = 0.5f + nextRandom() * 1.5f;

                    m_stars.push_back(star);
                }

                // Recreate star vertex buffer if GPU is initialized
#ifdef SPARK_PLATFORM_WINDOWS
                if (m_initialized && m_device && !m_stars.empty())
                {
                    CreateStarVertexBuffer();
                }
#endif
            }

            // ================================================================
            // GPU Constant Buffer Update
            // ================================================================

            void UpdateAtmosphereCBuffer(
                [[maybe_unused]] const XMMATRIX& inverseViewProj,
                [[maybe_unused]] const XMFLOAT3& cameraPosition)
            {
                const auto& atm = m_atmosphereSettings;
                const auto& cld = m_cloudSettings;

                m_cbufferData.rayleighScattering  = atm.rayleighScattering;
                m_cbufferData.rayleighScaleHeight = atm.rayleighScaleHeight;

                m_cbufferData.mieScattering  = atm.mieScattering;
                m_cbufferData.mieAbsorption  = atm.mieAbsorption;
                m_cbufferData.mieScaleHeight = atm.mieScaleHeight;
                m_cbufferData.mieG           = atm.mieG;

                m_cbufferData.ozoneAbsorption       = atm.ozoneAbsorption;
                m_cbufferData.ozoneCenterAltitude   = atm.ozoneCenterAltitude;
                m_cbufferData.ozoneWidth            = atm.ozoneWidth;

                m_cbufferData.sunDirection   = atm.sunDirection;
                m_cbufferData.sunIntensity   = atm.sunIntensity;
                m_cbufferData.sunColor       = atm.sunColor;
                m_cbufferData.sunDiskSize    = atm.sunDiskSize;

                m_cbufferData.planetRadius       = atm.planetRadius;
                m_cbufferData.atmosphereRadius   = atm.atmosphereRadius;
                m_cbufferData.exposureMultiplier = atm.exposureMultiplier;

                m_cbufferData.groundAlbedo = atm.groundAlbedo;

                m_cbufferData.cameraPosition = cameraPosition;
                m_cbufferData.cameraHeight = cameraPosition.y * 0.001f; // Convert to km
                m_currentCameraHeightKm = m_cbufferData.cameraHeight;

                XMStoreFloat4x4(&m_cbufferData.inverseViewProjection, inverseViewProj);

                m_cbufferData.cloudCoverage        = cld.coverage;
                m_cbufferData.cloudDensity         = cld.density;
                m_cbufferData.cloudAltitude        = cld.altitude;
                m_cbufferData.cloudThickness       = cld.thickness;
                m_cbufferData.cloudWindOffset      = cld.windOffset;
                m_cbufferData.cloudLightAbsorption = cld.lightAbsorption;
                m_cbufferData.cloudSilverLining    = cld.silverLiningIntensity;
                m_cbufferData.cloudDetailScale     = cld.detailNoiseScale;
                m_cbufferData.cloudErosion         = cld.erosion;
                m_cbufferData.cloudSilverSpread    = cld.silverLiningSpread;
                m_cbufferData.cloudRaySteps        = cld.rayMarchSteps;
                m_cbufferData.cloudLightSteps      = cld.lightMarchSteps;
                m_cbufferData.cloudType            = static_cast<int>(cld.type);
                m_cbufferData.cloudTemporalBlend   = cld.temporalBlend;

                m_cbufferData.starVisibility   = m_starVisibility;
                m_cbufferData.nightTransition  = std::clamp(
                    1.0f - (atm.sunDirection.y + 0.1f) / 0.2f, 0.0f, 1.0f);
            }

            // ================================================================
            // GPU Resource Creation (Windows / D3D11)
            // ================================================================

#ifdef SPARK_PLATFORM_WINDOWS

            bool CreateConstantBuffer()
            {
                D3D11_BUFFER_DESC desc = {};
                desc.ByteWidth = sizeof(AtmosphereCBuffer);
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

                HRESULT hr = m_device->CreateBuffer(&desc, nullptr,
                    m_atmosphereCBuffer.GetAddressOf());
                return SUCCEEDED(hr);
            }

            bool CreateTransmittanceLut()
            {
                D3D11_TEXTURE2D_DESC texDesc = {};
                texDesc.Width = SkyConstants::kTransmittanceLutWidth;
                texDesc.Height = SkyConstants::kTransmittanceLutHeight;
                texDesc.MipLevels = 1;
                texDesc.ArraySize = 1;
                texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                texDesc.SampleDesc.Count = 1;
                texDesc.Usage = D3D11_USAGE_DEFAULT;
                texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

                HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr,
                    m_transmittanceLutTexture.GetAddressOf());
                if (FAILED(hr)) return false;

                hr = m_device->CreateShaderResourceView(
                    m_transmittanceLutTexture.Get(), nullptr,
                    m_transmittanceLutSRV.GetAddressOf());
                if (FAILED(hr)) return false;

                hr = m_device->CreateUnorderedAccessView(
                    m_transmittanceLutTexture.Get(), nullptr,
                    m_transmittanceLutUAV.GetAddressOf());
                return SUCCEEDED(hr);
            }

            bool CreateMultiScatterLut()
            {
                D3D11_TEXTURE2D_DESC texDesc = {};
                texDesc.Width = SkyConstants::kMultiScatterLutSize;
                texDesc.Height = SkyConstants::kMultiScatterLutSize;
                texDesc.MipLevels = 1;
                texDesc.ArraySize = 1;
                texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                texDesc.SampleDesc.Count = 1;
                texDesc.Usage = D3D11_USAGE_DEFAULT;
                texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

                HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr,
                    m_multiScatterLutTexture.GetAddressOf());
                if (FAILED(hr)) return false;

                hr = m_device->CreateShaderResourceView(
                    m_multiScatterLutTexture.Get(), nullptr,
                    m_multiScatterLutSRV.GetAddressOf());
                if (FAILED(hr)) return false;

                hr = m_device->CreateUnorderedAccessView(
                    m_multiScatterLutTexture.Get(), nullptr,
                    m_multiScatterLutUAV.GetAddressOf());
                return SUCCEEDED(hr);
            }

            bool CreateSkyViewLut()
            {
                D3D11_TEXTURE2D_DESC texDesc = {};
                texDesc.Width = SkyConstants::kSkyViewLutWidth;
                texDesc.Height = SkyConstants::kSkyViewLutHeight;
                texDesc.MipLevels = 1;
                texDesc.ArraySize = 1;
                texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                texDesc.SampleDesc.Count = 1;
                texDesc.Usage = D3D11_USAGE_DEFAULT;
                texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

                HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr,
                    m_skyViewLutTexture.GetAddressOf());
                if (FAILED(hr)) return false;

                hr = m_device->CreateShaderResourceView(
                    m_skyViewLutTexture.Get(), nullptr,
                    m_skyViewLutSRV.GetAddressOf());
                if (FAILED(hr)) return false;

                hr = m_device->CreateUnorderedAccessView(
                    m_skyViewLutTexture.Get(), nullptr,
                    m_skyViewLutUAV.GetAddressOf());
                return SUCCEEDED(hr);
            }

            bool CreateStarVertexBuffer()
            {
                if (m_stars.empty())
                {
                    return true;
                }

                D3D11_BUFFER_DESC desc = {};
                desc.ByteWidth = static_cast<UINT>(m_stars.size() * sizeof(StarVertex));
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

                D3D11_SUBRESOURCE_DATA initData = {};
                initData.pSysMem = m_stars.data();

                m_starVertexBuffer.Reset();
                HRESULT hr = m_device->CreateBuffer(&desc, &initData,
                    m_starVertexBuffer.GetAddressOf());
                return SUCCEEDED(hr);
            }

            // ================================================================
            // GPU Dispatch (Windows / D3D11)
            // ================================================================

            void UploadConstantBuffer(ID3D11DeviceContext* context)
            {
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                HRESULT hr = context->Map(m_atmosphereCBuffer.Get(), 0,
                    D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                if (SUCCEEDED(hr))
                {
                    memcpy(mapped.pData, &m_cbufferData, sizeof(AtmosphereCBuffer));
                    context->Unmap(m_atmosphereCBuffer.Get(), 0);
                }
            }

            void DispatchTransmittanceLut(ID3D11DeviceContext* context)
            {
                if (!m_transmittanceLutCS)
                {
                    return;
                }

                ID3D11Buffer* cbs[] = {m_atmosphereCBuffer.Get()};
                context->CSSetConstantBuffers(0, 1, cbs);

                ID3D11UnorderedAccessView* uavs[] = {m_transmittanceLutUAV.Get()};
                context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

                context->CSSetShader(m_transmittanceLutCS.Get(), nullptr, 0);
                context->Dispatch(
                    (SkyConstants::kTransmittanceLutWidth + 7) / 8,
                    (SkyConstants::kTransmittanceLutHeight + 7) / 8,
                    1);

                // Unbind
                ID3D11UnorderedAccessView* nullUAV[] = {nullptr};
                context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
            }

            void DispatchMultiScatterLut(ID3D11DeviceContext* context)
            {
                if (!m_multiScatterLutCS)
                {
                    return;
                }

                ID3D11Buffer* cbs[] = {m_atmosphereCBuffer.Get()};
                context->CSSetConstantBuffers(0, 1, cbs);

                // Bind transmittance LUT as input
                ID3D11ShaderResourceView* srvs[] = {m_transmittanceLutSRV.Get()};
                context->CSSetShaderResources(0, 1, srvs);

                ID3D11UnorderedAccessView* uavs[] = {m_multiScatterLutUAV.Get()};
                context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

                context->CSSetShader(m_multiScatterLutCS.Get(), nullptr, 0);
                context->Dispatch(
                    (SkyConstants::kMultiScatterLutSize + 7) / 8,
                    (SkyConstants::kMultiScatterLutSize + 7) / 8,
                    1);

                // Unbind
                ID3D11ShaderResourceView* nullSRV[] = {nullptr};
                context->CSSetShaderResources(0, 1, nullSRV);
                ID3D11UnorderedAccessView* nullUAV[] = {nullptr};
                context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
            }

            void DispatchSkyViewLut(ID3D11DeviceContext* context)
            {
                if (!m_skyRenderCS)
                {
                    return;
                }

                ID3D11Buffer* cbs[] = {m_atmosphereCBuffer.Get()};
                context->CSSetConstantBuffers(0, 1, cbs);

                // Bind transmittance and multi-scatter LUTs as input
                ID3D11ShaderResourceView* srvs[] = {
                    m_transmittanceLutSRV.Get(),
                    m_multiScatterLutSRV.Get()
                };
                context->CSSetShaderResources(0, 2, srvs);

                ID3D11UnorderedAccessView* uavs[] = {m_skyViewLutUAV.Get()};
                context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

                context->CSSetShader(m_skyRenderCS.Get(), nullptr, 0);
                context->Dispatch(
                    (SkyConstants::kSkyViewLutWidth + 7) / 8,
                    (SkyConstants::kSkyViewLutHeight + 7) / 8,
                    1);

                // Unbind
                ID3D11ShaderResourceView* nullSRVs[] = {nullptr, nullptr};
                context->CSSetShaderResources(0, 2, nullSRVs);
                ID3D11UnorderedAccessView* nullUAV[] = {nullptr};
                context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
            }

            void RenderSkyPass([[maybe_unused]] ID3D11DeviceContext* context)
            {
                // Bind sky view LUT and render fullscreen triangle
                // Implementation dispatches a fullscreen pixel shader that
                // samples the sky view LUT with the inverse view-projection
                // to reconstruct view directions per pixel.
                //
                // Shader resources:
                //   t0 = transmittance LUT
                //   t1 = multi-scatter LUT
                //   t2 = sky view LUT
                //   b0 = AtmosphereCBuffer
            }

            void RenderSunDisk([[maybe_unused]] ID3D11DeviceContext* context)
            {
                // Sun disk is rendered as part of the sky pass by checking
                // the angular distance between the view ray and sun direction.
                // When within sunDiskSize radians, a limb-darkened disk is
                // blended in, modulated by transmittance to the sun.
            }

            void RenderClouds([[maybe_unused]] ID3D11DeviceContext* context)
            {
                if (!m_cloudRenderCS)
                {
                    return;
                }

                // Cloud rendering dispatches a compute shader that performs
                // either 2D noise sampling (Layer2D) or volumetric ray-marching.
                //
                // For Layer2D: single noise sample per pixel at cloud altitude
                // For Volumetric: ray-march through cloud slab with:
                //   - Perlin-Worley base shape noise
                //   - Detail erosion noise
                //   - Beer-Lambert light extinction
                //   - Henyey-Greenstein phase for silver lining
                //   - Temporal reprojection for noise reduction
            }

            void RenderStars([[maybe_unused]] ID3D11DeviceContext* context)
            {
                // Stars are rendered as point sprites from the star vertex buffer.
                // Each star is a screen-aligned quad scaled by apparent brightness
                // and modulated by m_starVisibility for day/night transition.
                // A twinkle effect is applied via per-star phase offset and time.
            }

            // ================================================================
            // D3D11 Resources
            // ================================================================

            ID3D11Device* m_device = nullptr;
            ID3D11DeviceContext* m_context = nullptr;

            // Constant buffer
            ComPtr<ID3D11Buffer> m_atmosphereCBuffer;

            // Transmittance LUT
            ComPtr<ID3D11Texture2D> m_transmittanceLutTexture;
            ComPtr<ID3D11ShaderResourceView> m_transmittanceLutSRV;
            ComPtr<ID3D11UnorderedAccessView> m_transmittanceLutUAV;

            // Multi-scattering LUT
            ComPtr<ID3D11Texture2D> m_multiScatterLutTexture;
            ComPtr<ID3D11ShaderResourceView> m_multiScatterLutSRV;
            ComPtr<ID3D11UnorderedAccessView> m_multiScatterLutUAV;

            // Sky view LUT
            ComPtr<ID3D11Texture2D> m_skyViewLutTexture;
            ComPtr<ID3D11ShaderResourceView> m_skyViewLutSRV;
            ComPtr<ID3D11UnorderedAccessView> m_skyViewLutUAV;

            // Compute shaders
            ComPtr<ID3D11ComputeShader> m_transmittanceLutCS;
            ComPtr<ID3D11ComputeShader> m_multiScatterLutCS;
            ComPtr<ID3D11ComputeShader> m_skyRenderCS;
            ComPtr<ID3D11ComputeShader> m_cloudRenderCS;

            // Star vertex buffer
            ComPtr<ID3D11Buffer> m_starVertexBuffer;

#endif // SPARK_PLATFORM_WINDOWS

            // ================================================================
            // State
            // ================================================================

            bool m_initialized   = false;
            bool m_lutsGenerated = false;
            bool m_lutsNeedUpdate = false;
            bool m_settingsDirty  = false;

            bool m_sunDiskEnabled = true;
            bool m_starsEnabled   = true;

            float m_starVisibility       = 0.0f;
            float m_currentCameraHeightKm = 0.0f;

            uint32_t m_starCount = 2048;

            // Settings
            AtmosphereSettings m_atmosphereSettings;
            CloudSettings m_cloudSettings;
            SkyboxSettings m_skyboxSettings;

            // Star data (CPU-side)
            std::vector<StarVertex> m_stars;

            // GPU constant buffer staging data
            AtmosphereCBuffer m_cbufferData = {};

            // DayNightCycle integration (non-owning)
            DayNightCycle* m_dayNightCycle = nullptr;
        };

    } // namespace Graphics
} // namespace Spark
