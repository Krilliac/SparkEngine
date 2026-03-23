/**
 * @file LightingTools.h
 * @brief Advanced lighting and environment system for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <Core/framework.h>
#include <cstdint>
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>


namespace SparkEditor
{

    /**
 * @brief Light types supported by Spark Engine
 */
    enum class SparkLightType : uint32_t
    {
        Directional = 0, ///< Directional light (sun)
        Point = 1,       ///< Point light (bulb)
        Spot = 2,        ///< Spot light (flashlight)
        Area = 3,        ///< Area light (panel/window)
        Environment = 4, ///< Environment/IBL light
        Volumetric = 5   ///< Volumetric light (fog lights)
    };

    /**
 * @brief Light falloff models
 */
    enum class LightFalloff : uint32_t
    {
        Linear = 0,        ///< Linear falloff
        Quadratic = 1,     ///< Realistic quadratic falloff
        InverseSquare = 2, ///< Physically accurate inverse square
        Custom = 3         ///< Custom falloff curve
    };

    /**
 * @brief Shadow quality settings
 */
    enum class ShadowQuality : uint32_t
    {
        Disabled = 0, ///< No shadows
        Low = 1,      ///< 512x512 shadow maps
        Medium = 2,   ///< 1024x1024 shadow maps
        High = 3,     ///< 2048x2048 shadow maps
        Ultra = 4,    ///< 4096x4096 shadow maps
        RTX = 5       ///< Ray-traced shadows
    };

    /**
 * @brief Advanced light configuration
 */
    struct SparkLightData
    {
        // Basic properties
        SparkLightType type = SparkLightType::Point;
        XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
        XMFLOAT3 direction = {0.0f, -1.0f, 0.0f};
        XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        float range = 10.0f;

        // Advanced properties
        float innerConeAngle = 30.0f; ///< Inner cone angle for spot lights
        float outerConeAngle = 45.0f; ///< Outer cone angle for spot lights
        float temperature = 6500.0f;  ///< Color temperature in Kelvin
        LightFalloff falloffType = LightFalloff::Quadratic;
        float falloffExponent = 2.0f; ///< Custom falloff exponent

        // Shadow properties
        ShadowQuality shadowQuality = ShadowQuality::Medium;
        float shadowBias = 0.001f;     ///< Shadow bias to prevent acne
        float shadowNormalBias = 0.1f; ///< Normal-based shadow bias
        int shadowCascades = 4;        ///< Number of shadow cascades
        float shadowDistance = 100.0f; ///< Maximum shadow distance

        // Area light properties (for area lights)
        XMFLOAT2 areaSize = {1.0f, 1.0f}; ///< Width/height for area lights

        // Volumetric properties
        bool enableVolumetrics = false;  ///< Enable volumetric lighting
        float volumetricStrength = 1.0f; ///< Volumetric effect strength
        float volumetricDensity = 0.1f;  ///< Fog/atmosphere density

        // Performance
        bool castShadows = true;        ///< Whether light casts shadows
        bool affectTransparency = true; ///< Affect transparent objects
        float cullingRadius = -1.0f;    ///< Auto-calculate if -1
        int maxAffectedObjects = 256;   ///< Max objects this light affects

        // Animation/Time of Day
        bool animateIntensity = false; ///< Animate intensity over time
        bool animateColor = false;     ///< Animate color over time
        bool animatePosition = false;  ///< Animate position (e.g., sun)
        std::string animationCurve;    ///< Animation curve data

        // Metadata
        std::string name = "SparkLight"; ///< Light name
        std::string description;         ///< Light description
        bool isActive = true;            ///< Light enabled state
        int priority = 0;                ///< Rendering priority
        uint32_t layerMask = 0xFFFFFFFF; ///< Layer mask for affected objects
    };

    /**
 * @brief Global illumination settings
 */
    struct GlobalIlluminationSettings
    {
        bool enableGI = true;    ///< Enable global illumination
        bool enableSSAO = true;  ///< Screen space ambient occlusion
        bool enableSSR = true;   ///< Screen space reflections
        bool enableRTGI = false; ///< Ray-traced global illumination

        // Light probes
        int lightProbeResolution = 32;  ///< Light probe texture resolution
        float lightProbeSpacing = 5.0f; ///< Spacing between light probes
        int maxLightProbes = 1000;      ///< Maximum number of light probes

        // Lightmap settings
        int lightmapResolution = 1024; ///< Lightmap texture resolution
        float lightmapPadding = 2.0f;  ///< UV padding for lightmaps
        bool useDenoising = true;      ///< Apply denoising to lightmaps
        int bounceCount = 3;           ///< Number of light bounces

        // Ambient lighting
        XMFLOAT3 ambientColor = {0.1f, 0.1f, 0.1f};
        float ambientIntensity = 1.0f;
        std::string skyboxTexture;   ///< HDR skybox texture path
        float skyboxRotation = 0.0f; ///< Skybox rotation in degrees
        float skyboxExposure = 1.0f; ///< Skybox exposure adjustment
    };

    /**
 * @brief Atmosphere and weather settings
 */
    struct AtmosphereSettings
    {
        // Time of day
        float timeOfDay = 12.0f;       ///< Time in hours (0-24)
        float dayDuration = 300.0f;    ///< Day duration in seconds
        bool animateTimeOfDay = false; ///< Auto-animate time of day

        // Sun/moon
        XMFLOAT3 sunDirection = {0.3f, -0.6f, 0.75f};
        XMFLOAT3 sunColor = {1.0f, 0.95f, 0.8f};
        float sunIntensity = 3.0f;
        float sunAngularSize = 0.53f; ///< Sun angular size in degrees

        XMFLOAT3 moonDirection = {-0.3f, -0.6f, -0.75f};
        XMFLOAT3 moonColor = {0.8f, 0.8f, 1.0f};
        float moonIntensity = 0.3f;

        // Atmosphere scattering
        bool enableAtmosphereScattering = true;
        XMFLOAT3 rayleighScattering = {0.0025f, 0.0041f, 0.0081f};
        float mieScattering = 0.003f;
        float turbidity = 2.0f; ///< Atmosphere turbidity

        // Fog and clouds
        bool enableFog = true;
        XMFLOAT3 fogColor = {0.7f, 0.8f, 0.9f};
        float fogDensity = 0.01f;
        float fogStartDistance = 10.0f;
        float fogEndDistance = 200.0f;

        bool enableClouds = false;
        float cloudCoverage = 0.5f;
        float cloudDensity = 0.8f;
        XMFLOAT2 cloudWindDirection = {1.0f, 0.0f};
        float cloudWindSpeed = 0.1f;

        // Weather effects
        float rainIntensity = 0.0f; ///< Rain intensity (0-1)
        float snowIntensity = 0.0f; ///< Snow intensity (0-1)
        float windStrength = 0.5f;  ///< Wind strength for particles
        XMFLOAT3 windDirection = {1.0f, 0.0f, 0.0f};
    };

    /**
   * @brief Post-processing and tonemapping
   */
    struct PostProcessingSettings
    {
        // Tonemapping
        bool enableTonemapping = true;
        std::string tonemappingOperator = "ACES"; // ACES, Reinhard, Filmic, etc.
        float exposure = 1.0f;
        float gamma = 2.2f;

        // Color grading
        bool enableColorGrading = false;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float brightness = 0.0f;
        XMFLOAT3 colorTint = {1.0f, 1.0f, 1.0f};

        // Bloom
        bool enableBloom = true;
        float bloomThreshold = 1.0f;
        float bloomIntensity = 0.3f;
        float bloomRadius = 1.0f;

        // Other effects
        bool enableMotionBlur = false;
        bool enableDepthOfField = false;
        bool enableChromaticAberration = false;
        bool enableVignette = false;
    };

    /**
 * @brief Light baking progress callback
 */
    using LightBakeProgressCallback = std::function<void(float progress, const std::string& status)>;

    /**
 * @brief Light changed callback
 */
    using LightChangedCallback = std::function<void(const SparkLightData& light)>;

    /**
 * @brief Advanced lighting and environment system for Spark Engine
 * 
 * This comprehensive lighting system provides professional-grade lighting tools
 * including real-time and baked lighting, global illumination, time-of-day
 * simulation, weather effects, and advanced post-processing.
 */
    class LightingTools
    {
      public:
        /**
     * @brief Constructor
     */
        LightingTools();

        /**
     * @brief Destructor
     */
        ~LightingTools();

        /**
     * @brief Initialize the lighting system
     * @param device DirectX device
     * @param context DirectX context
     * @return true if initialization succeeded
     */
        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

        /**
     * @brief Update the lighting system
     * @param deltaTime Time since last update
     */
        void Update(float deltaTime);

        /**
     * @brief Render the lighting tools UI
     */
        void RenderUI();

        /**
     * @brief Shutdown the lighting system
     */
        void Shutdown();

        // === LIGHT MANAGEMENT ===

        /**
     * @brief Create a new light
     * @param lightData Light configuration
     * @return Light ID
     */
        uint32_t CreateLight(const SparkLightData& lightData);

        /**
     * @brief Update an existing light
     * @param lightId Light ID
     * @param lightData New light configuration
     */
        void UpdateLight(uint32_t lightId, const SparkLightData& lightData);

        /**
     * @brief Delete a light
     * @param lightId Light ID
     */
        void DeleteLight(uint32_t lightId);

        /**
     * @brief Get light data
     * @param lightId Light ID
     * @return Pointer to light data, or nullptr if not found
     */
        const SparkLightData* GetLight(uint32_t lightId) const;

        /**
     * @brief Get all lights
     * @return Vector of all light data
     */
        std::vector<SparkLightData> GetAllLights() const;

        /**
     * @brief Set light changed callback
     * @param callback Callback function
     */
        void SetLightChangedCallback(LightChangedCallback callback);

        // === GLOBAL ILLUMINATION ===

        /**
     * @brief Set global illumination settings
     * @param settings GI settings
     */
        void SetGlobalIlluminationSettings(const GlobalIlluminationSettings& settings);

        /**
     * @brief Get global illumination settings
     * @return Current GI settings
     */
        const GlobalIlluminationSettings& GetGlobalIlluminationSettings() const;

        /**
     * @brief Bake lightmaps
     * @param progressCallback Progress callback
     * @return true if baking succeeded
     */
        bool BakeLightmaps(LightBakeProgressCallback progressCallback = nullptr);

        /**
     * @brief Generate light probes
     * @param bounds Bounding volume for probe generation
     * @param spacing Spacing between probes
     * @return Number of probes generated
     */
        int GenerateLightProbes(const XMFLOAT3& bounds, float spacing);

        /**
     * @brief Clear all baked lighting data
     */
        void ClearBakedLighting();

        // === ATMOSPHERE AND WEATHER ===

        /**
     * @brief Set atmosphere settings
     * @param settings Atmosphere settings
     */
        void SetAtmosphereSettings(const AtmosphereSettings& settings);

        /**
     * @brief Get atmosphere settings
     * @return Current atmosphere settings
     */
        const AtmosphereSettings& GetAtmosphereSettings() const;

        /**
     * @brief Set time of day
     * @param timeInHours Time in hours (0-24)
     */
        void SetTimeOfDay(float timeInHours);

        /**
     * @brief Get current time of day
     * @return Time in hours (0-24)
     */
        float GetTimeOfDay() const;

        /**
     * @brief Enable/disable automatic time of day animation
     * @param enabled Animation enabled
     * @param dayDuration Duration of full day in seconds
     */
        void SetTimeOfDayAnimation(bool enabled, float dayDuration = 300.0f);

        // === POST-PROCESSING ===

        /**
     * @brief Set post-processing settings
     * @param settings Post-processing settings
     */
        void SetPostProcessingSettings(const PostProcessingSettings& settings);

        /**
     * @brief Get post-processing settings
     * @return Current post-processing settings
     */
        const PostProcessingSettings& GetPostProcessingSettings() const;

        // === PRESETS AND PROFILES ===

        /**
     * @brief Save lighting profile
     * @param profileName Profile name
     * @return true if save succeeded
     */
        bool SaveLightingProfile(const std::string& profileName);

        /**
     * @brief Load lighting profile
     * @param profileName Profile name
     * @return true if load succeeded
     */
        bool LoadLightingProfile(const std::string& profileName);

        /**
     * @brief Get available lighting profiles
     * @return Vector of profile names
     */
        std::vector<std::string> GetAvailableLightingProfiles() const;

        /**
     * @brief Apply lighting preset
     * @param presetName Preset name ("Sunny Day", "Sunset", "Night", etc.)
     */
        void ApplyLightingPreset(const std::string& presetName);

        // === PERFORMANCE AND OPTIMIZATION ===

        /**
     * @brief Get lighting performance metrics
     */
        struct LightingMetrics
        {
            int activeLights = 0;
            int shadowCastingLights = 0;
            int lightmapTextures = 0;
            int lightProbes = 0;
            float renderTime = 0.0f;
            float shadowRenderTime = 0.0f;
            size_t lightmapMemory = 0;
            size_t shadowMapMemory = 0;
        };
        LightingMetrics GetLightingMetrics() const;

        /**
     * @brief Optimize lighting for performance
     * @param targetFPS Target frame rate
     */
        void OptimizeLightingPerformance(float targetFPS = 60.0f);

      private:
        /**
     * @brief Render light list UI
     */
        void RenderLightListUI();

        /**
     * @brief Render light inspector UI
     */
        void RenderLightInspectorUI();

        /**
     * @brief Render global illumination UI
     */
        void RenderGlobalIlluminationUI();

        /**
     * @brief Render atmosphere UI
     */
        void RenderAtmosphereUI();

        /**
     * @brief Render post-processing UI
     */
        void RenderPostProcessingUI();

        /**
     * @brief Render performance UI
     */
        void RenderPerformanceUI();

        /**
     * @brief Render presets UI
     */
        void RenderPresetsUI();

        /**
     * @brief Update sun position based on time of day
     */
        void UpdateSunPosition();

        /**
     * @brief Calculate sun color based on time of day
     */
        XMFLOAT3 CalculateSunColor(float timeOfDay) const;

        /**
     * @brief Update atmosphere scattering parameters
     */
        void UpdateAtmosphereScattering();

        /**
     * @brief Validate light configuration
     * @param lightData Light data to validate
     * @return true if valid
     */
        bool ValidateLightData(const SparkLightData& lightData) const;

      private:
        // DirectX resources
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;

        // Lighting data
        std::unordered_map<uint32_t, SparkLightData> m_lights;
        uint32_t m_nextLightId = 1;
        uint32_t m_selectedLightId = 0;

        // Settings
        GlobalIlluminationSettings m_giSettings;
        AtmosphereSettings m_atmosphereSettings;
        PostProcessingSettings m_postProcessingSettings;

        // State
        bool m_isInitialized = false;
        bool m_lightmapBakeInProgress = false;
        float m_bakeProgress = 0.0f;
        std::string m_bakeStatus;
        LightBakeProgressCallback m_bakeProgressCallback;
        LightChangedCallback m_lightChangedCallback;

        // Performance metrics
        mutable LightingMetrics m_metrics;

        // Time of day animation
        bool m_animateTimeOfDay = false;
        float m_timeOfDaySpeed = 1.0f;

        // UI state
        bool m_showLightList = true;
        bool m_showLightInspector = true;
        bool m_showGlobalIllumination = true;
        bool m_showAtmosphere = false;
        bool m_showPostProcessing = false;
        bool m_showPerformance = false;
        bool m_showPresets = false;
    };

} // namespace SparkEditor