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
#include <cstdint>

// DXR lives in Spark::Graphics (not Spark::RHI) because it requires D3D12
// and is an optional high-level feature, unlike the backend-agnostic RHI layer.
namespace Spark::Graphics {

// ============================================================================
// DXR Feature Flags
// ============================================================================

enum class RTFeature : uint32_t {
    None          = 0,
    Reflections   = 1 << 0,
    Shadows       = 1 << 1,
    AmbientOcclusion = 1 << 2,
    GlobalIllumination = 1 << 3,
    All           = 0xFFFFFFFF
};

inline RTFeature operator|(RTFeature a, RTFeature b) {
    return static_cast<RTFeature>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline RTFeature operator&(RTFeature a, RTFeature b) {
    return static_cast<RTFeature>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool operator!(RTFeature a) {
    return static_cast<uint32_t>(a) == 0;
}

// Helper to test if a feature flag is set
inline bool HasFeature(RTFeature flags, RTFeature feature) {
    return static_cast<uint32_t>(flags & feature) != 0;
}

// ============================================================================
// Acceleration Structure Descriptions
// ============================================================================

struct BLASDesc {
    std::string meshName;
    const void* vertexData;
    uint32_t vertexCount;
    uint32_t vertexStride;
    const uint32_t* indexData;
    uint32_t indexCount;
    bool isOpaque = true;
    bool allowUpdate = false;  ///< For animated meshes
};

struct BLASInstance {
    uint32_t blasIndex;
    DirectX::XMFLOAT4X4 transform;
    uint32_t instanceID;
    uint32_t hitGroupIndex;
    uint8_t instanceMask = 0xFF;
};

// ============================================================================
// Ray Tracing Settings
// ============================================================================

struct RTReflectionSettings {
    bool enabled = false;
    int maxBounces = 1;
    float roughnessThreshold = 0.5f;  ///< Only trace below this roughness
    int samplesPerPixel = 1;
    float maxDistance = 100.0f;
    bool useTemporalAccumulation = true;
};

struct RTShadowSettings {
    bool enabled = false;
    int samplesPerPixel = 1;
    float softShadowRadius = 0.05f;   ///< Larger = softer shadows
    bool denoise = true;
};

struct RTAOSettings {
    bool enabled = false;
    float radius = 3.0f;
    int samplesPerPixel = 1;
    float power = 1.5f;
    bool denoise = true;
};

struct RTGISettings {
    bool enabled = false;
    int maxBounces = 2;
    int samplesPerPixel = 1;
    float maxDistance = 50.0f;
    bool useProbeGrid = true;         ///< Use irradiance probes for caching
    DirectX::XMFLOAT3 probeGridDensity{2.0f, 2.0f, 2.0f};  ///< Probes per meter
};

struct DXRSettings {
    RTFeature enabledFeatures = RTFeature::None;
    RTReflectionSettings reflections;
    RTShadowSettings shadows;
    RTAOSettings ambientOcclusion;
    RTGISettings globalIllumination;
    float renderScale = 1.0f;         ///< RT render at lower resolution
};

// ============================================================================
// DXR Manager
// ============================================================================

class DXRManager {
public:
    static DXRManager& GetInstance();

    /// Check if DXR is available on this system
    bool IsAvailable() const { return m_isAvailable; }

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
    struct DXRStats {
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

    // Acceleration structures (opaque — actual D3D12 resources in implementation)
    struct BLASData {
        BLASDesc desc;
        void* resource = nullptr;
        uint64_t size = 0;
    };
    std::vector<BLASData> m_blasList;
    void* m_tlasResource = nullptr;
    uint64_t m_tlasSize = 0;
    uint32_t m_tlasInstanceCount = 0;

    mutable DXRStats m_stats{};
};

} // namespace Spark::Graphics
