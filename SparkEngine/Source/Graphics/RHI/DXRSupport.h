/**
 * @file DXRSupport.h
 * @brief DirectX Raytracing (DXR) support for ray-traced reflections, GI, and shadows
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides optional DXR acceleration:
 * - Bottom-Level Acceleration Structure (BLAS) per mesh
 * - Top-Level Acceleration Structure (TLAS) for the scene
 * - Ray-traced reflections
 * - Ray-traced ambient occlusion
 * - Ray-traced shadows (soft shadows)
 * - Ray-traced global illumination (diffuse GI)
 *
 * Requires D3D12 and DXR-capable GPU. Falls back gracefully when unavailable.
 *
 * @warning **Partial implementation — ~80% done.** As of 2026-04-10:
 *          - Device capability check, root signature, command queue
 *            (`Initialize()`, ~lines 269–360 of `DXRSupport.cpp`) — DONE.
 *          - `CreateBLAS` / `UpdateBLAS` / `DestroyBLAS` with acceleration
 *            structure resource management — DONE (~lines 525–630).
 *          - `BuildTLAS` with full GPU-side TLAS build and barriers — DONE
 *            (~lines 643–729).
 *          - HLSL ray shaders — present under
 *            `Shaders/HLSL/RayTracing/DXR{Reflections,Shadows,AO,GI}.hlsl`.
 *          - `TraceReflections()` / `TraceShadows()` / `TraceAO()` /
 *            `TraceGI()` declared but **stubbed** — they need shader
 *            binding table (SBT) construction, state-object creation via
 *            `ID3D12StateObject`, and `DispatchRays()` calls before they
 *            produce usable output.
 *          - `DXRManager::GetInstance().IsAvailable()` is checked in
 *            `GraphicsEngine.cpp:409` and the trace entry points are
 *            already called at `GraphicsEngine.cpp:1161-1176`, so as soon
 *            as the trace methods are filled in the rest of the render
 *            pipeline picks them up automatically.
 *
 *          No tests exist under `Tests/` for DXR specifically — because
 *          the D3D12 device stack is not reachable from the native Linux
 *          test runner, any future coverage needs a MinGW + Wine +
 *          Lavapipe path or a D3D12 mock.
 *
 *          Finishing this file is a larger scoped session than simple
 *          wiring — see `.claude/knowledge/engine-next-steps-2026-04-10.md`
 *          Phase B item 4.
 */

#pragma once
#include "../../Core/Platform.h"
#include "RHITypes.h" // RayTracingBackend for GetBackend()

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <utility>

// DXR lives in Spark::Graphics (not Spark::RHI) because it requires D3D12
// and is an optional high-level feature, unlike the backend-agnostic RHI layer.
namespace Spark::Graphics
{

    // ============================================================================
    // DXR Feature Flags
    // ============================================================================

    enum class RTFeature : uint32_t
    {
        None = 0,
        Reflections = 1 << 0,
        Shadows = 1 << 1,
        AmbientOcclusion = 1 << 2,
        GlobalIllumination = 1 << 3,
        All = 0xFFFFFFFF
    };

    inline RTFeature operator|(RTFeature a, RTFeature b)
    {
        return static_cast<RTFeature>(std::to_underlying(a) | std::to_underlying(b));
    }
    inline RTFeature operator&(RTFeature a, RTFeature b)
    {
        return static_cast<RTFeature>(std::to_underlying(a) & std::to_underlying(b));
    }
    inline bool operator!(RTFeature a)
    {
        return static_cast<uint32_t>(a) == 0;
    }

    // Helper to test if a feature flag is set
    inline bool HasFeature(RTFeature flags, RTFeature feature)
    {
        return static_cast<uint32_t>(flags & feature) != 0;
    }

    // ============================================================================
    // Acceleration Structure Descriptions
    // ============================================================================

    struct BLASDesc
    {
        std::string meshName;
        const void* vertexData;
        uint32_t vertexCount;
        uint32_t vertexStride;
        const uint32_t* indexData;
        uint32_t indexCount;
        bool isOpaque = true;
        bool allowUpdate = false; ///< For animated meshes
    };

    struct BLASInstance
    {
        uint32_t blasIndex;
        DirectX::XMFLOAT4X4 transform;
        uint32_t instanceID;
        uint32_t hitGroupIndex;
        uint8_t instanceMask = 0xFF;
    };

    // ============================================================================
    // Ray Tracing Settings
    // ============================================================================

    struct RTReflectionSettings
    {
        bool enabled = false;
        int maxBounces = 1;
        float roughnessThreshold = 0.5f; ///< Only trace below this roughness
        int samplesPerPixel = 1;
        float maxDistance = 100.0f;
        bool useTemporalAccumulation = true;
    };

    struct RTShadowSettings
    {
        bool enabled = false;
        int samplesPerPixel = 1;
        float softShadowRadius = 0.05f; ///< Larger = softer shadows
        bool denoise = true;
    };

    struct RTAOSettings
    {
        bool enabled = false;
        float radius = 3.0f;
        int samplesPerPixel = 1;
        float power = 1.5f;
        bool denoise = true;
    };

    struct RTGISettings
    {
        bool enabled = false;
        int maxBounces = 2;
        int samplesPerPixel = 1;
        float maxDistance = 50.0f;
        bool useProbeGrid = true;                             ///< Use irradiance probes for caching
        DirectX::XMFLOAT3 probeGridDensity{2.0f, 2.0f, 2.0f}; ///< Probes per meter
    };

    struct DXRSettings
    {
        RTFeature enabledFeatures = RTFeature::None;
        RTReflectionSettings reflections;
        RTShadowSettings shadows;
        RTAOSettings ambientOcclusion;
        RTGISettings globalIllumination;
        float renderScale = 1.0f; ///< RT render at lower resolution
    };

    // ============================================================================
    // DXR Manager
    // ============================================================================

    class DXRManager
    {
      public:
        static DXRManager& GetInstance();

        /// Check if DXR is available on this system
        bool IsAvailable() const { return m_isAvailable; }

        /// @brief Get the ray tracing backend type for HybridRTManager coordination
        Spark::RHI::RayTracingBackend GetBackend() const
        {
            return m_isAvailable ? Spark::RHI::RayTracingBackend::HardwareDXR : Spark::RHI::RayTracingBackend::Disabled;
        }

        /// Initialize DXR (requires D3D12 device)
        bool Initialize(void* d3d12Device);
        void Shutdown();

        /// Build/rebuild acceleration structures
        uint32_t CreateBLAS(const BLASDesc& desc);
        void UpdateBLAS(uint32_t blasIndex);
        void DestroyBLAS(uint32_t blasIndex);

        /// Build TLAS from instances
        void BuildTLAS(const std::vector<BLASInstance>& instances);

        /// Dispatch ray tracing
        void TraceReflections(const DirectX::XMMATRIX& viewProj, const DirectX::XMFLOAT3& cameraPos);
        void TraceShadows(const DirectX::XMFLOAT3& lightDirection);
        void TraceAmbientOcclusion(const DirectX::XMMATRIX& viewProj, const DirectX::XMFLOAT3& cameraPos);
        void TraceGlobalIllumination(const DirectX::XMMATRIX& viewProj, const DirectX::XMFLOAT3& cameraPos);

        /// Settings
        void SetSettings(const DXRSettings& settings);
        const DXRSettings& GetSettings() const { return m_settings; }

        /// Statistics
        struct DXRStats
        {
            uint32_t blasCount;
            uint32_t tlasInstanceCount;
            uint64_t accelerationStructureMemory;
            float rtReflectionsTimeMs;
            float rtShadowsTimeMs;
            float rtAOTimeMs;
            float rtGITimeMs;
        };
        DXRStats GetStats() const;

        /// Console integration
        std::string Console_GetStatus() const;
        void Console_EnableFeature(const std::string& feature, bool enabled);
        void Console_SetQuality(const std::string& quality);

      private:
        DXRManager() = default;

        bool m_isAvailable = false;
        bool m_isInitialized = false;
        DXRSettings m_settings;

        // Acceleration structures — opaque handles. The D3D12 ComPtr resources
        // are stored inside DXRInternalState (DXRSupport.cpp) and indexed by these.
        struct BLASData
        {
            BLASDesc desc;
            uint32_t internalIndex = UINT32_MAX; ///< Index into DXRInternalState::blasResources
            uint64_t size = 0;
        };
        std::vector<BLASData> m_blasList;
        std::unordered_map<std::string, uint32_t> m_blasLookup; ///< meshName → blasIndex dedup

        uint32_t m_tlasInternalIndex = UINT32_MAX;
        uint64_t m_tlasSize = 0;
        uint32_t m_tlasInstanceCount = 0;

        bool BuildRTPSOs();       ///< Create ray tracing pipeline state objects
        bool BuildShaderTables(); ///< Build shader binding tables with proper alignment

        mutable DXRStats m_stats{};
    };

} // namespace Spark::Graphics
