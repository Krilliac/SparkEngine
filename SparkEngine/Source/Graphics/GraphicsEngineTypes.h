/**
 * @file GraphicsEngineTypes.h
 * @brief Enums, settings structs, and data-transfer types used by GraphicsEngine
 * @author Spark Engine Team
 * @date 2025
 *
 * Extracted from GraphicsEngine.h to keep type definitions separate from the
 * main GraphicsEngine class declaration.
 */

#pragma once

#include <cstdint>
#include <cstddef>

/**
 * @brief Render path / pipeline types for advanced graphics
 *
 * Unified enum replacing the previous separate RenderingPipeline and RenderPath
 * enums which had identical values.
 */
enum class RenderPath
{
    Forward,     ///< Forward rendering
    Deferred,    ///< Deferred rendering
    ForwardPlus, ///< Forward+ (tiled forward) rendering
    Clustered    ///< Clustered rendering
};

/// @brief Legacy alias - use RenderPath directly for new code
using RenderingPipeline = RenderPath;

/**
 * @brief Variable Rate Shading mode
 *
 * Controls how shading rate varies across the screen. Requires hardware
 * support (D3D12 Tier 1/2 VRS, Vulkan VK_KHR_fragment_shading_rate).
 */
enum class VRSMode
{
    Off,     ///< No variable rate shading
    PerDraw, ///< Shading rate set per draw call (Tier 1)
    PerTile  ///< Shading rate varies per screen-space tile (Tier 2)
};

/**
 * @brief Variable Rate Shading quality/rate
 */
enum class VRSShadingRate
{
    Full,   ///< 1x1 — full rate shading
    Half,   ///< 2x2 — quarter rate (4 pixels per shading invocation)
    Quarter ///< 4x4 — 1/16 rate (16 pixels per shading invocation)
};

/**
 * @brief Rendering quality presets
 */
enum class QualityPreset
{
    Low,    ///< Low quality (mobile/integrated graphics)
    Medium, ///< Medium quality (mid-range hardware)
    High,   ///< High quality (high-end hardware)
    Ultra,  ///< Ultra quality (enthusiast hardware)
    Custom  ///< Custom quality settings
};

/**
 * @brief Multi-sampling anti-aliasing settings
 */
enum class MSAALevel
{
    None = 1,   ///< No MSAA
    MSAA2x = 2, ///< 2x MSAA
    MSAA4x = 4, ///< 4x MSAA
    MSAA8x = 8  ///< 8x MSAA
};

// TAASettings is defined in TemporalEffects.h (included above)

/**
 * @brief Screen-space ambient occlusion settings
 */
struct SSAOSettings
{
    bool enabled = false;   ///< Enable SSAO
    float radius = 0.5f;    ///< SSAO sampling radius
    float intensity = 1.0f; ///< SSAO intensity
    int sampleCount = 16;   ///< Number of SSAO samples
    float bias = 0.025f;    ///< SSAO bias to prevent self-occlusion
    bool blur = true;       ///< Enable SSAO blur
};

/**
 * @brief Screen-space reflection settings
 */
struct SSRSettings
{
    bool enabled = false;       ///< Enable SSR
    float maxDistance = 100.0f; ///< Maximum reflection distance
    int maxSteps = 32;          ///< Maximum ray marching steps
    float thickness = 0.5f;     ///< Surface thickness for intersection
    float fadeStart = 80.0f;    ///< Distance to start fading reflections
    float fadeEnd = 100.0f;     ///< Distance to fully fade reflections
};

/**
 * @brief Volumetric lighting settings
 */
struct VolumetricSettings
{
    bool enabled = false;     ///< Enable volumetric lighting
    int sampleCount = 32;     ///< Number of volumetric samples
    float scattering = 0.1f;  ///< Light scattering factor
    float extinction = 0.01f; ///< Light extinction factor
    float anisotropy = 0.3f;  ///< Phase function anisotropy
};

/**
 * @brief Advanced graphics settings
 */
struct GraphicsSettings
{
    // Rendering
    RenderPath renderPath = RenderPath::Deferred;
    QualityPreset qualityPreset = QualityPreset::High;
    bool vsync = true;
    bool hdr = true;
    uint32_t msaaSamples = 4;

    // Textures
    uint32_t maxTextureSize = 2048;
    bool anisotropicFiltering = true;
    uint32_t anisotropyLevel = 16;

    // Shadows
    bool shadows = true;
    uint32_t shadowMapSize = 2048;
    uint32_t cascadeCount = 3;

    // Post-processing
    bool bloom = true;
    bool ssao = false;
    bool taa = false;
    bool motionBlur = false;

    // Performance
    bool frustumCulling = true;
    bool occlusionCulling = false;
    bool levelOfDetail = true;
    uint32_t maxDrawCalls = 1000;

    // Variable Rate Shading
    VRSMode vrsMode = VRSMode::Off;
    VRSShadingRate vrsShadingRate = VRSShadingRate::Full;

    // Debug / display
    bool wireframeMode = false;
    bool debugMode = false;
    bool showFPS = false;
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float renderScale = 1.0f;
    bool enableGPUTiming = false;
};

/**
 * @brief Comprehensive render statistics
 */
struct RenderStatistics
{
    // Performance
    float frameTime = 0.0f; ///< Total frame time (ms)
    float cpuTime = 0.0f;   ///< CPU time (ms)
    float gpuTime = 0.0f;   ///< GPU time (ms)
    uint32_t fps = 0;       ///< Frames per second

    // Rendering
    uint32_t drawCalls = 0;        ///< Draw calls per frame
    uint32_t triangles = 0;        ///< Triangles rendered
    uint32_t vertices = 0;         ///< Vertices processed
    uint32_t textureBinds = 0;     ///< Texture binds per frame
    uint32_t materialSwitches = 0; ///< Material switches per frame

    // Culling
    uint32_t totalObjects = 0;   ///< Total objects in scene
    uint32_t visibleObjects = 0; ///< Objects after culling
    uint32_t culledObjects = 0;  ///< Objects culled
    float cullingTime = 0.0f;    ///< Culling time (ms)

    // Memory
    size_t textureMemory = 0;  ///< Texture memory usage (bytes)
    size_t meshMemory = 0;     ///< Mesh memory usage (bytes)
    size_t totalGPUMemory = 0; ///< Total GPU memory usage (bytes)

    // Lighting
    uint32_t activeLights = 0;     ///< Active lights
    uint32_t shadowUpdates = 0;    ///< Shadow map updates
    float lightCullingTime = 0.0f; ///< Light culling time (ms)

    // Post-processing
    float postProcessTime = 0.0f;   ///< Post-processing time (ms)
    uint32_t postProcessPasses = 0; ///< Post-processing passes

    // Timing breakdown
    float renderTime = 0.0f;    ///< Render time (ms)
    float presentTime = 0.0f;   ///< Present/swap time (ms)
    size_t bufferMemory = 0;    ///< Buffer memory usage (bytes)
    float gpuUsage = 0.0f;      ///< GPU utilization (%)
    bool vsyncEnabled = false;  ///< VSync state
    bool wireframeMode = false; ///< Wireframe state
    bool debugMode = false;     ///< Debug mode state
};
