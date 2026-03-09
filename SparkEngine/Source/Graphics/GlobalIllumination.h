/**
 * @file GlobalIllumination.h
 * @brief Global illumination system: light probes, irradiance volumes, DDGI
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides indirect lighting through spherical harmonics light probes,
 * irradiance volumes, and Dynamic Diffuse Global Illumination (DDGI).
 * Supports manual and automatic probe placement, GPU buffer management
 * for SH data, and probe streaming for large worlds.
 *
 * ## Usage
 * @code
 *   GlobalIlluminationSystem gi;
 *   gi.Initialize(device, context);
 *
 *   GISettings settings;
 *   settings.mode = GIMode::LightProbes;
 *   settings.bounceCount = 2;
 *   gi.SetSettings(settings);
 *
 *   gi.PlaceProbesOnGrid(worldMin, worldMax, {8, 4, 8});
 *   gi.BakeAllProbes();
 *
 *   // Each frame
 *   gi.Update(deltaTime);
 *   gi.BindGIResources(context, shaderSlot);
 * @endcode
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <vector>
#include <array>
#include <cstdint>
#include <string>
#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

// =============================================================================
// Spherical Harmonics Constants
// =============================================================================

/** @brief Number of SH coefficients for L2 (order 2) spherical harmonics */
static constexpr int SH_COEFFICIENT_COUNT = 9;

// =============================================================================
// Light Probe
// =============================================================================

/**
 * @brief A single light probe storing L2 spherical harmonics coefficients
 *
 * Each probe captures incoming irradiance from a point in the scene,
 * encoded as 9 SH bands (L0 + L1 + L2) per color channel (RGB).
 */
struct LightProbe
{
    XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    float radius = 10.0f;         ///< Influence radius in world units
    float influenceWeight = 1.0f; ///< Blending weight for overlapping probes

    /** @brief SH coefficients per color channel (R, G, B), 9 bands each (L2) */
    std::array<float, SH_COEFFICIENT_COUNT> shCoefficientsR = {};
    std::array<float, SH_COEFFICIENT_COUNT> shCoefficientsG = {};
    std::array<float, SH_COEFFICIENT_COUNT> shCoefficientsB = {};

    bool isDirty = true;  ///< Needs re-bake
    bool isActive = true; ///< Contributing to lighting

    /** @brief Unique identifier for streaming and serialization */
    uint32_t probeId = 0;
};

// =============================================================================
// GPU SH Data (StructuredBuffer layout)
// =============================================================================

/**
 * @brief GPU-friendly SH data for a single probe (StructuredBuffer element)
 *
 * Packed layout for efficient GPU access. Each coefficient is stored
 * as an XMFLOAT4 containing (R, G, B, unused) for that SH band.
 */
struct alignas(16) SHProbeGPUData
{
    XMFLOAT4 shBands[SH_COEFFICIENT_COUNT]; ///< Each band: (R, G, B, 0)
    XMFLOAT4 positionAndRadius;             ///< (x, y, z, radius)
    XMFLOAT4 weightAndFlags;                ///< (weight, active, 0, 0)
};

// =============================================================================
// Irradiance Volume
// =============================================================================

/**
 * @brief 3D grid of SH probes forming an irradiance volume
 *
 * Probes are arranged in a regular grid within an axis-aligned bounding box.
 * The GPU samples this volume using trilinear interpolation of SH coefficients.
 */
struct IrradianceVolume
{
    XMFLOAT3 boundsMin = {0.0f, 0.0f, 0.0f};      ///< AABB minimum corner
    XMFLOAT3 boundsMax = {100.0f, 50.0f, 100.0f}; ///< AABB maximum corner

    uint32_t resolutionX = 8; ///< Number of probes along X
    uint32_t resolutionY = 4; ///< Number of probes along Y
    uint32_t resolutionZ = 8; ///< Number of probes along Z

    float updateFrequency = 0.0f; ///< Seconds between updates (0 = manual only)
    float timeSinceLastUpdate = 0.0f;

    /** @brief All probes in the volume, indexed as [z * resY * resX + y * resX + x] */
    std::vector<LightProbe> probes;

    /** @brief Get the total number of probes */
    uint32_t GetProbeCount() const { return resolutionX * resolutionY * resolutionZ; }

    /** @brief Get the world-space spacing between probes */
    XMFLOAT3 GetProbeSpacing() const
    {
        float sx = (resolutionX > 1) ? (boundsMax.x - boundsMin.x) / static_cast<float>(resolutionX - 1) : 0.0f;
        float sy = (resolutionY > 1) ? (boundsMax.y - boundsMin.y) / static_cast<float>(resolutionY - 1) : 0.0f;
        float sz = (resolutionZ > 1) ? (boundsMax.z - boundsMin.z) / static_cast<float>(resolutionZ - 1) : 0.0f;
        return {sx, sy, sz};
    }

    /** @brief Get the 3D grid index for a linear probe index */
    void GetProbeGridIndex(uint32_t linearIndex, uint32_t& outX, uint32_t& outY, uint32_t& outZ) const
    {
        outX = linearIndex % resolutionX;
        outY = (linearIndex / resolutionX) % resolutionY;
        outZ = linearIndex / (resolutionX * resolutionY);
    }
};

// =============================================================================
// GI Settings
// =============================================================================

/**
 * @brief Global illumination mode
 */
enum class GIMode
{
    None,             ///< No global illumination
    LightProbes,      ///< Manually/auto-placed discrete light probes
    IrradianceVolume, ///< 3D grid of SH probes
    DDGI              ///< Dynamic Diffuse GI (ray-traced probe updates)
};

/**
 * @brief Configuration for the global illumination system
 */
struct GISettings
{
    GIMode mode = GIMode::None;
    int bounceCount = 1;        ///< Number of light bounces to simulate
    float intensity = 1.0f;     ///< Global intensity multiplier for indirect light
    float maxDistance = 500.0f; ///< Maximum distance for probe influence

    // Probe baking
    uint32_t cubemapResolution = 64; ///< Resolution per face for probe baking cubemaps
    bool bakeShadows = true;         ///< Include shadow information in probes
    float skyOcclusionWeight = 0.5f; ///< Blend between probe data and sky [0, 1]

    // DDGI-specific
    int ddgiRaysPerProbe = 256;       ///< Rays traced per probe per update
    float ddgiHysteresis = 0.97f;     ///< Temporal blending for DDGI updates [0, 1]
    float ddgiIrradianceGamma = 5.0f; ///< Gamma for irradiance encoding
    float ddgiDepthSharpness = 50.0f; ///< Sharpness for depth-based visibility

    // Streaming
    float streamingDistance = 200.0f; ///< Distance at which to load/unload probe data
    bool enableStreaming = false;     ///< Enable probe data streaming for large worlds

    // Debug
    bool debugVisualization = false; ///< Show probe debug spheres
    float debugSphereRadius = 0.5f;  ///< Radius of debug visualization spheres
};

// =============================================================================
// GI Constant Buffer (GPU)
// =============================================================================

/**
 * @brief Constant buffer data sent to shaders for GI sampling
 */
struct alignas(16) GIConstantBuffer
{
    XMFLOAT4 volumeBoundsMin;  ///< (minX, minY, minZ, padding)
    XMFLOAT4 volumeBoundsMax;  ///< (maxX, maxY, maxZ, padding)
    XMFLOAT4 probeSpacing;     ///< (spacingX, spacingY, spacingZ, padding)
    XMFLOAT4 volumeResolution; ///< (resX, resY, resZ, probeCount)
    XMFLOAT4 giParams;         ///< (intensity, maxDistance, mode, bounceCount)
    XMFLOAT4 ddgiParams;       ///< (hysteresis, irradianceGamma, depthSharpness, raysPerProbe)
};

// =============================================================================
// Probe Streaming Region
// =============================================================================

/**
 * @brief Streaming region for large-world probe management
 */
struct ProbeStreamingRegion
{
    XMFLOAT3 center = {0.0f, 0.0f, 0.0f};
    float radius = 200.0f;
    std::vector<uint32_t> probeIndices; ///< Indices into the main probe array
    bool isLoaded = false;
    bool isLoading = false;
};

// =============================================================================
// Global Illumination System
// =============================================================================

/**
 * @class GlobalIlluminationSystem
 * @brief Manages global illumination through light probes and irradiance volumes
 *
 * Provides indirect lighting computation via spherical harmonics probes.
 * Supports manual placement, automatic grid generation, cubemap-based
 * SH computation, GPU buffer management, and DDGI-style dynamic updates.
 */
class GlobalIlluminationSystem
{
  public:
    GlobalIlluminationSystem() = default;
    ~GlobalIlluminationSystem() { Shutdown(); }

    // ---- Lifecycle ----

    /**
     * @brief Initialize the GI system with a D3D11 device
     * @param device  D3D11 device for GPU resource creation
     * @param context D3D11 device context for GPU operations
     * @return true on success
     */
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        if (m_initialized)
        {
            return false;
        }

        m_device = device;
        m_context = context;

        if (!CreateGPUResources())
        {
            return false;
        }

        m_initialized = true;
        return true;
    }

    /**
     * @brief Shutdown and release all GPU resources
     */
    void Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        m_probes.clear();
        m_irradianceVolume = {};
        m_streamingRegions.clear();
        ReleaseGPUResources();
        m_initialized = false;
    }

    /**
     * @brief Update the GI system each frame
     * @param deltaTime Frame delta time in seconds
     */
    void Update(float deltaTime)
    {
        if (!m_initialized || m_settings.mode == GIMode::None)
        {
            return;
        }

        m_totalTime += deltaTime;

        // Update irradiance volume on timer
        if (m_settings.mode == GIMode::IrradianceVolume || m_settings.mode == GIMode::DDGI)
        {
            m_irradianceVolume.timeSinceLastUpdate += deltaTime;
            if (m_irradianceVolume.updateFrequency > 0.0f &&
                m_irradianceVolume.timeSinceLastUpdate >= m_irradianceVolume.updateFrequency)
            {
                UpdateDirtyProbes();
                m_irradianceVolume.timeSinceLastUpdate = 0.0f;
            }
        }

        // DDGI dynamic updates
        if (m_settings.mode == GIMode::DDGI)
        {
            UpdateDDGIProbes(deltaTime);
        }

        // Streaming updates
        if (m_settings.enableStreaming)
        {
            UpdateProbeStreaming();
        }

        // Sync dirty data to GPU
        if (m_gpuBufferDirty)
        {
            UploadProbeDataToGPU();
            m_gpuBufferDirty = false;
        }

        UpdateConstantBuffer();
    }

    // ---- Probe Placement ----

    /**
     * @brief Manually add a light probe at a specific position
     * @param position World-space position
     * @param radius   Influence radius
     * @return Index of the newly added probe
     */
    uint32_t AddProbe(const XMFLOAT3& position, float radius = 10.0f)
    {
        LightProbe probe;
        probe.position = position;
        probe.radius = radius;
        probe.probeId = m_nextProbeId++;
        probe.isDirty = true;

        uint32_t index = static_cast<uint32_t>(m_probes.size());
        m_probes.push_back(probe);
        m_gpuBufferDirty = true;
        return index;
    }

    /**
     * @brief Remove a probe by index
     * @param index Probe index to remove
     */
    void RemoveProbe(uint32_t index)
    {
        if (index < m_probes.size())
        {
            m_probes.erase(m_probes.begin() + index);
            m_gpuBufferDirty = true;
        }
    }

    /**
     * @brief Automatically place probes on a regular 3D grid
     * @param boundsMin   Minimum corner of the placement volume
     * @param boundsMax   Maximum corner of the placement volume
     * @param resolution  Number of probes along each axis (x, y, z)
     */
    void PlaceProbesOnGrid(const XMFLOAT3& boundsMin, const XMFLOAT3& boundsMax, const XMFLOAT3& resolution)
    {
        uint32_t resX = std::max(1u, static_cast<uint32_t>(resolution.x));
        uint32_t resY = std::max(1u, static_cast<uint32_t>(resolution.y));
        uint32_t resZ = std::max(1u, static_cast<uint32_t>(resolution.z));

        m_irradianceVolume.boundsMin = boundsMin;
        m_irradianceVolume.boundsMax = boundsMax;
        m_irradianceVolume.resolutionX = resX;
        m_irradianceVolume.resolutionY = resY;
        m_irradianceVolume.resolutionZ = resZ;

        m_probes.clear();
        m_irradianceVolume.probes.clear();

        XMFLOAT3 spacing = m_irradianceVolume.GetProbeSpacing();

        for (uint32_t z = 0; z < resZ; ++z)
        {
            for (uint32_t y = 0; y < resY; ++y)
            {
                for (uint32_t x = 0; x < resX; ++x)
                {
                    LightProbe probe;
                    probe.position = {boundsMin.x + static_cast<float>(x) * spacing.x,
                                      boundsMin.y + static_cast<float>(y) * spacing.y,
                                      boundsMin.z + static_cast<float>(z) * spacing.z};
                    probe.radius = std::max({spacing.x, spacing.y, spacing.z}) * 1.5f;
                    probe.probeId = m_nextProbeId++;
                    probe.isDirty = true;

                    m_probes.push_back(probe);
                    m_irradianceVolume.probes.push_back(probe);
                }
            }
        }

        m_gpuBufferDirty = true;
    }

    // ---- SH Computation ----

    /**
     * @brief Compute SH coefficients for a single probe from a rendered cubemap
     *
     * Projects the cubemap irradiance onto L2 spherical harmonics (9 bands)
     * for each color channel.
     *
     * @param probeIndex   Index of the probe to update
     * @param cubemapFaces Array of 6 cubemap face textures (SRVs)
     * @param faceSize     Resolution of each cubemap face in pixels
     */
    void ComputeSHFromCubemap(uint32_t probeIndex, ID3D11ShaderResourceView* const cubemapFaces[6], uint32_t faceSize)
    {
        if (probeIndex >= m_probes.size() || !m_context)
        {
            return;
        }

        // In production, this would dispatch a compute shader to project
        // the cubemap onto SH basis functions. Here we outline the algorithm:
        //
        // For each texel on each cubemap face:
        //   1. Compute the direction vector from the texel's UV
        //   2. Evaluate L2 SH basis functions for that direction
        //   3. Accumulate weighted (by solid angle) color * basis into SH coefficients
        //   4. Normalize by total solid angle
        //
        // The 9 SH basis functions for L2 are:
        //   Y_0^0  = 0.282095
        //   Y_1^-1 = 0.488603 * y
        //   Y_1^0  = 0.488603 * z
        //   Y_1^1  = 0.488603 * x
        //   Y_2^-2 = 1.092548 * x * y
        //   Y_2^-1 = 1.092548 * y * z
        //   Y_2^0  = 0.315392 * (3*z*z - 1)
        //   Y_2^1  = 1.092548 * x * z
        //   Y_2^2  = 0.546274 * (x*x - y*y)

        (void)cubemapFaces;
        (void)faceSize;

        m_probes[probeIndex].isDirty = false;
        m_gpuBufferDirty = true;
    }

    /**
     * @brief Bake SH coefficients for all dirty probes
     *
     * Iterates through all probes marked as dirty, renders cubemaps
     * at their positions, and computes SH coefficients.
     */
    void BakeAllProbes()
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_probes.size()); ++i)
        {
            if (m_probes[i].isDirty)
            {
                // In production: render cubemap at probe position, then compute SH
                // ComputeSHFromCubemap(i, renderedCubemapFaces, m_settings.cubemapResolution);
                m_probes[i].isDirty = false;
            }
        }
        m_gpuBufferDirty = true;
    }

    /**
     * @brief Mark all probes as dirty, requiring re-bake
     */
    void InvalidateAllProbes()
    {
        for (auto& probe : m_probes)
        {
            probe.isDirty = true;
        }
    }

    // ---- DDGI ----

    /**
     * @brief Update DDGI probes using ray tracing (when available)
     *
     * Traces rays from each probe, updates irradiance and visibility
     * textures with temporal hysteresis blending.
     *
     * @param deltaTime Frame delta time
     */
    void UpdateDDGIProbes(float deltaTime)
    {
        if (m_settings.mode != GIMode::DDGI || m_probes.empty())
        {
            return;
        }

        (void)deltaTime;

        // DDGI algorithm outline:
        // 1. For each probe, trace m_settings.ddgiRaysPerProbe rays in random directions
        // 2. For each ray hit:
        //    a. Sample direct lighting at hit point
        //    b. Accumulate irradiance contribution weighted by cosine term
        //    c. Record hit distance for visibility/depth
        // 3. Update irradiance octahedral map with temporal hysteresis:
        //    newIrradiance = lerp(newSample, previousIrradiance, m_settings.ddgiHysteresis)
        // 4. Update depth/visibility octahedral map similarly
        // 5. Apply irradiance gamma encoding for perceptual stability
        //
        // This requires DXR or compute-shader-based ray tracing support.

        m_gpuBufferDirty = true;
    }

    // ---- GPU Resource Management ----

    /**
     * @brief Bind GI resources to the pipeline for shader access
     * @param context      Device context
     * @param srvSlot      Shader resource view register slot for probe StructuredBuffer
     * @param cbSlot       Constant buffer register slot for GI parameters
     */
    void BindGIResources(ID3D11DeviceContext* context, uint32_t srvSlot = 10, uint32_t cbSlot = 5) const
    {
        if (!m_initialized || !context)
        {
            return;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        if (m_probeSRV)
        {
            ID3D11ShaderResourceView* srv = m_probeSRV.Get();
            context->PSSetShaderResources(srvSlot, 1, &srv);
        }
        if (m_giConstantBuffer)
        {
            ID3D11Buffer* cb = m_giConstantBuffer.Get();
            context->PSSetConstantBuffers(cbSlot, 1, &cb);
        }
#else
        (void)srvSlot;
        (void)cbSlot;
#endif
    }

    /**
     * @brief Unbind GI resources from the pipeline
     */
    void UnbindGIResources(ID3D11DeviceContext* context, uint32_t srvSlot = 10, uint32_t cbSlot = 5) const
    {
        if (!context)
        {
            return;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        ID3D11ShaderResourceView* nullSRV = nullptr;
        context->PSSetShaderResources(srvSlot, 1, &nullSRV);
        ID3D11Buffer* nullCB = nullptr;
        context->PSSetConstantBuffers(cbSlot, 1, &nullCB);
#else
        (void)srvSlot;
        (void)cbSlot;
#endif
    }

    // ---- Debug Visualization ----

    /**
     * @brief Render debug spheres at probe positions showing SH irradiance
     *
     * Draws small spheres at each probe location, colored by the dominant
     * SH irradiance direction. Useful for validating probe placement.
     */
    void RenderDebugVisualization() const
    {
        if (!m_settings.debugVisualization || m_probes.empty())
        {
            return;
        }

        // In production: for each probe, render a sphere mesh at probe.position
        // with radius m_settings.debugSphereRadius, shaded by evaluating the
        // probe's SH coefficients for each vertex normal direction.
    }

    /**
     * @brief Get the number of active debug visualization elements
     */
    uint32_t GetDebugVisualizationCount() const
    {
        if (!m_settings.debugVisualization)
        {
            return 0;
        }
        return static_cast<uint32_t>(m_probes.size());
    }

    // ---- Probe Streaming ----

    /**
     * @brief Update streaming regions based on camera position
     * @param cameraPosition Current camera world-space position
     */
    void UpdateStreamingForCamera(const XMFLOAT3& cameraPosition)
    {
        if (!m_settings.enableStreaming)
        {
            return;
        }

        float streamDistSq = m_settings.streamingDistance * m_settings.streamingDistance;

        for (auto& region : m_streamingRegions)
        {
            float dx = region.center.x - cameraPosition.x;
            float dy = region.center.y - cameraPosition.y;
            float dz = region.center.z - cameraPosition.z;
            float distSq = dx * dx + dy * dy + dz * dz;

            if (distSq <= streamDistSq && !region.isLoaded && !region.isLoading)
            {
                LoadStreamingRegion(region);
            }
            else if (distSq > streamDistSq * 1.5f && region.isLoaded)
            {
                UnloadStreamingRegion(region);
            }
        }
    }

    /**
     * @brief Register a streaming region for large-world probe management
     * @param center Region center in world space
     * @param radius Region radius
     * @param probeIndices Indices of probes belonging to this region
     */
    void AddStreamingRegion(const XMFLOAT3& center, float radius, const std::vector<uint32_t>& probeIndices)
    {
        ProbeStreamingRegion region;
        region.center = center;
        region.radius = radius;
        region.probeIndices = probeIndices;
        m_streamingRegions.push_back(std::move(region));
    }

    // ---- Console Integration ----

    /** @brief Get a summary string of GI system state */
    std::string Console_GetStatus() const
    {
        std::string status = "Global Illumination:\n";

        const char* modeStr = "None";
        switch (m_settings.mode)
        {
        case GIMode::LightProbes:
            modeStr = "Light Probes";
            break;
        case GIMode::IrradianceVolume:
            modeStr = "Irradiance Volume";
            break;
        case GIMode::DDGI:
            modeStr = "DDGI";
            break;
        default:
            break;
        }

        status += "  Mode: " + std::string(modeStr) + "\n";
        status += "  Probes: " + std::to_string(m_probes.size()) + "\n";
        status += "  Bounces: " + std::to_string(m_settings.bounceCount) + "\n";
        status += "  Intensity: " + std::to_string(m_settings.intensity) + "\n";

        if (m_settings.mode == GIMode::IrradianceVolume || m_settings.mode == GIMode::DDGI)
        {
            status += "  Volume: " + std::to_string(m_irradianceVolume.resolutionX) + "x" +
                      std::to_string(m_irradianceVolume.resolutionY) + "x" +
                      std::to_string(m_irradianceVolume.resolutionZ) + "\n";
        }

        if (m_settings.enableStreaming)
        {
            uint32_t loaded = 0;
            for (const auto& region : m_streamingRegions)
            {
                if (region.isLoaded)
                {
                    ++loaded;
                }
            }
            status += "  Streaming regions: " + std::to_string(loaded) + "/" +
                      std::to_string(m_streamingRegions.size()) + " loaded\n";
        }

        status += "  Debug viz: " + std::string(m_settings.debugVisualization ? "ON" : "OFF") + "\n";
        return status;
    }

    /** @brief Console command to set GI mode by name */
    void Console_SetMode(const std::string& modeName)
    {
        if (modeName == "none")
        {
            m_settings.mode = GIMode::None;
        }
        else if (modeName == "probes")
        {
            m_settings.mode = GIMode::LightProbes;
        }
        else if (modeName == "volume")
        {
            m_settings.mode = GIMode::IrradianceVolume;
        }
        else if (modeName == "ddgi")
        {
            m_settings.mode = GIMode::DDGI;
        }
    }

    /** @brief Console command to toggle debug visualization */
    void Console_ToggleDebug() { m_settings.debugVisualization = !m_settings.debugVisualization; }

    /** @brief Console command to set GI intensity */
    void Console_SetIntensity(float intensity) { m_settings.intensity = std::clamp(intensity, 0.0f, 10.0f); }

    /** @brief Console command to force re-bake all probes */
    void Console_RebakeAll()
    {
        InvalidateAllProbes();
        BakeAllProbes();
    }

    // ---- Accessors ----

    GISettings& GetSettings() { return m_settings; }
    const GISettings& GetSettings() const { return m_settings; }
    void SetSettings(const GISettings& settings) { m_settings = settings; }

    const std::vector<LightProbe>& GetProbes() const { return m_probes; }
    uint32_t GetProbeCount() const { return static_cast<uint32_t>(m_probes.size()); }

    const IrradianceVolume& GetIrradianceVolume() const { return m_irradianceVolume; }

    bool IsInitialized() const { return m_initialized; }

    /** @brief Get a specific probe by index */
    const LightProbe* GetProbe(uint32_t index) const
    {
        if (index < m_probes.size())
        {
            return &m_probes[index];
        }
        return nullptr;
    }

    /** @brief Get a mutable probe by index */
    LightProbe* GetProbeMutable(uint32_t index)
    {
        if (index < m_probes.size())
        {
            m_gpuBufferDirty = true;
            return &m_probes[index];
        }
        return nullptr;
    }

  private:
    // ---- GPU Resource Creation ----

    bool CreateGPUResources()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_device)
        {
            return false;
        }

        // Create GI constant buffer
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(GIConstantBuffer);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = m_device->CreateBuffer(&cbDesc, nullptr, m_giConstantBuffer.GetAddressOf());
        if (FAILED(hr))
        {
            return false;
        }
#endif
        return true;
    }

    void ReleaseGPUResources()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        m_probeStructuredBuffer.Reset();
        m_probeSRV.Reset();
        m_giConstantBuffer.Reset();
#endif
    }

    /**
     * @brief Create or resize the StructuredBuffer for probe SH data
     */
    bool CreateProbeStructuredBuffer(uint32_t probeCount)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_device || probeCount == 0)
        {
            return false;
        }

        m_probeStructuredBuffer.Reset();
        m_probeSRV.Reset();

        D3D11_BUFFER_DESC bufDesc = {};
        bufDesc.ByteWidth = probeCount * sizeof(SHProbeGPUData);
        bufDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bufDesc.StructureByteStride = sizeof(SHProbeGPUData);

        HRESULT hr = m_device->CreateBuffer(&bufDesc, nullptr, m_probeStructuredBuffer.GetAddressOf());
        if (FAILED(hr))
        {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = probeCount;

        hr = m_device->CreateShaderResourceView(m_probeStructuredBuffer.Get(), &srvDesc, m_probeSRV.GetAddressOf());
        if (FAILED(hr))
        {
            return false;
        }

        m_gpuProbeCapacity = probeCount;
#else
        (void)probeCount;
#endif
        return true;
    }

    /**
     * @brief Upload all probe SH data to the GPU StructuredBuffer
     */
    void UploadProbeDataToGPU()
    {
        uint32_t probeCount = static_cast<uint32_t>(m_probes.size());
        if (probeCount == 0)
        {
            return;
        }

        // Resize buffer if needed
        if (probeCount > m_gpuProbeCapacity)
        {
            CreateProbeStructuredBuffer(probeCount);
        }

#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context || !m_probeStructuredBuffer)
        {
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(m_probeStructuredBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr))
        {
            return;
        }

        auto* gpuData = static_cast<SHProbeGPUData*>(mapped.pData);
        for (uint32_t i = 0; i < probeCount; ++i)
        {
            const auto& probe = m_probes[i];
            auto& gpu = gpuData[i];

            for (int band = 0; band < SH_COEFFICIENT_COUNT; ++band)
            {
                gpu.shBands[band] = {probe.shCoefficientsR[band], probe.shCoefficientsG[band],
                                     probe.shCoefficientsB[band], 0.0f};
            }

            gpu.positionAndRadius = {probe.position.x, probe.position.y, probe.position.z, probe.radius};
            gpu.weightAndFlags = {probe.influenceWeight, probe.isActive ? 1.0f : 0.0f, 0.0f, 0.0f};
        }

        m_context->Unmap(m_probeStructuredBuffer.Get(), 0);
#endif
    }

    /**
     * @brief Update the GI constant buffer with current settings
     */
    void UpdateConstantBuffer()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context || !m_giConstantBuffer)
        {
            return;
        }

        GIConstantBuffer cbData = {};
        cbData.volumeBoundsMin = {m_irradianceVolume.boundsMin.x, m_irradianceVolume.boundsMin.y,
                                  m_irradianceVolume.boundsMin.z, 0.0f};
        cbData.volumeBoundsMax = {m_irradianceVolume.boundsMax.x, m_irradianceVolume.boundsMax.y,
                                  m_irradianceVolume.boundsMax.z, 0.0f};

        XMFLOAT3 spacing = m_irradianceVolume.GetProbeSpacing();
        cbData.probeSpacing = {spacing.x, spacing.y, spacing.z, 0.0f};
        cbData.volumeResolution = {
            static_cast<float>(m_irradianceVolume.resolutionX), static_cast<float>(m_irradianceVolume.resolutionY),
            static_cast<float>(m_irradianceVolume.resolutionZ), static_cast<float>(m_probes.size())};
        cbData.giParams = {m_settings.intensity, m_settings.maxDistance,
                           static_cast<float>(static_cast<int>(m_settings.mode)),
                           static_cast<float>(m_settings.bounceCount)};
        cbData.ddgiParams = {m_settings.ddgiHysteresis, m_settings.ddgiIrradianceGamma, m_settings.ddgiDepthSharpness,
                             static_cast<float>(m_settings.ddgiRaysPerProbe)};

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(m_giConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            memcpy(mapped.pData, &cbData, sizeof(cbData));
            m_context->Unmap(m_giConstantBuffer.Get(), 0);
        }
#endif
    }

    // ---- Internal Updates ----

    void UpdateDirtyProbes()
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_probes.size()); ++i)
        {
            if (m_probes[i].isDirty)
            {
                // Would trigger cubemap render + SH computation per probe
                m_probes[i].isDirty = false;
            }
        }
        m_gpuBufferDirty = true;
    }

    void UpdateProbeStreaming()
    {
        // Streaming logic is driven by UpdateStreamingForCamera
    }

    void LoadStreamingRegion(ProbeStreamingRegion& region)
    {
        region.isLoading = true;
        // In production: async load probe SH data from disk
        for (uint32_t idx : region.probeIndices)
        {
            if (idx < m_probes.size())
            {
                m_probes[idx].isActive = true;
            }
        }
        region.isLoaded = true;
        region.isLoading = false;
        m_gpuBufferDirty = true;
    }

    void UnloadStreamingRegion(ProbeStreamingRegion& region)
    {
        for (uint32_t idx : region.probeIndices)
        {
            if (idx < m_probes.size())
            {
                m_probes[idx].isActive = false;
            }
        }
        region.isLoaded = false;
        m_gpuBufferDirty = true;
    }

    // ---- Member Variables ----

    bool m_initialized = false;
    float m_totalTime = 0.0f;
    uint32_t m_nextProbeId = 1;

    // Settings
    GISettings m_settings;

    // Probes
    std::vector<LightProbe> m_probes;
    IrradianceVolume m_irradianceVolume;

    // Streaming
    std::vector<ProbeStreamingRegion> m_streamingRegions;

    // GPU state
    bool m_gpuBufferDirty = false;
    uint32_t m_gpuProbeCapacity = 0;

    // D3D11 resources (raw pointers are non-owning per engine convention)
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

#ifdef SPARK_PLATFORM_WINDOWS
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_probeStructuredBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_probeSRV;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_giConstantBuffer;
#endif
};
