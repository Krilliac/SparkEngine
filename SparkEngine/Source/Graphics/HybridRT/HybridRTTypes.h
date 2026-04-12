/**
 * @file HybridRTTypes.h
 * @brief Shared types for the hybrid ray tracing system
 * @author Spark Engine Team
 * @date 2026
 *
 * SDF primitive descriptions, trace parameters, and GPU constant buffer
 * layouts used by both CPU-side managers and HLSL compute shaders.
 */

#pragma once

#include "../../Core/Platform.h"
#include "../RHI/RHITypes.h"

// DirectXMath types provided by Platform.h (stubs on Linux).

#include <cstdint>
#include <string>

namespace Spark::Graphics
{

    // ============================================================================
    // SDF Primitive Types
    // ============================================================================

    enum class SDFPrimitiveType : uint32_t
    {
        Sphere = 0,
        Box = 1,
        Capsule = 2,
        RoundedBox = 3
    };

    /// @brief GPU-uploadable SDF primitive (matches HLSL SDFPrimitive struct)
    struct alignas(16) SDFPrimitive
    {
        DirectX::XMFLOAT3 center = {0, 0, 0};
        float radius = 1.0f;
        DirectX::XMFLOAT3 halfExtents = {1, 1, 1};
        uint32_t type = 0; // SDFPrimitiveType
        DirectX::XMFLOAT4 albedo = {0.5f, 0.5f, 0.5f, 1.0f};
        float roughness = 0.5f;
        float metallic = 0.0f;
        float emissive = 0.0f;
        float padding = 0.0f;
    };
    static_assert(sizeof(SDFPrimitive) == 64, "SDFPrimitive must be 64 bytes for GPU upload");

    // ============================================================================
    // Trace Parameters (CPU → GPU constant buffer)
    // ============================================================================

    /// @brief Matches HLSL TraceConstants cbuffer layout
    struct alignas(16) SDFTraceConstants
    {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT3 cameraPos;
        float maxDistance = 100.0f;
        DirectX::XMFLOAT3 lightDir;
        int maxSteps = 64;
        int primitiveCount = 0;
        int traceMode = 0; ///< 0=reflections, 1=GI, 2=shadows
        float outputWidth = 1920.0f;
        float outputHeight = 1080.0f;
        float sdfBias = 0.001f;
        float shadowSoftness = 16.0f;
        float giIntensity = 1.0f;
        float temporalBlend = 0.9f;
    };

    /// @brief Per-frame parameters passed to HybridRTManager::Execute
    struct RTFrameParams
    {
        DirectX::XMMATRIX view;
        DirectX::XMMATRIX proj;
        DirectX::XMFLOAT3 cameraPos;
        DirectX::XMFLOAT3 lightDir;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // ============================================================================
    // Quality Preset Parameters
    // ============================================================================

    struct RTQualityParams
    {
        float resolutionScale = 1.0f; ///< 0.25, 0.5, or 1.0
        int maxSteps = 64;
        int maxBounces = 2;
        float maxDistance = 100.0f;
        bool enableDenoising = false;
        bool enableProbes = true;
        bool enableTemporalAccum = true;
    };

    /// @brief Get trace parameters for a given quality preset
    inline RTQualityParams GetQualityParams(Spark::RHI::RayTracingQuality quality)
    {
        RTQualityParams p;
        switch (quality)
        {
        case RHI::RayTracingQuality::Low:
            p = {0.25f, 32, 1, 50.0f, false, false, true};
            break;
        case RHI::RayTracingQuality::Medium:
            p = {0.5f, 64, 2, 100.0f, false, true, true};
            break;
        case RHI::RayTracingQuality::High:
            p = {1.0f, 96, 2, 150.0f, true, true, true};
            break;
        case RHI::RayTracingQuality::Ultra:
            p = {1.0f, 128, 3, 200.0f, true, true, true};
            break;
        default:
            p = {0.0f, 0, 0, 0.0f, false, false, false};
            break;
        }
        return p;
    }

    /// @brief Convert RayTracingBackend to display string
    inline std::string RayTracingBackendToString(RHI::RayTracingBackend b)
    {
        switch (b)
        {
        case RHI::RayTracingBackend::Disabled:
            return "Disabled";
        case RHI::RayTracingBackend::Software_SDFGI:
            return "Software (SDFGI)";
        case RHI::RayTracingBackend::Software_VCT:
            return "Software (VCT)";
        case RHI::RayTracingBackend::HardwareDXR:
            return "Hardware (DXR 1.1)";
        case RHI::RayTracingBackend::HardwareVKRT:
            return "Hardware (Vulkan RT)";
        }
        return "Unknown";
    }

    inline std::string RayTracingQualityToString(RHI::RayTracingQuality q)
    {
        switch (q)
        {
        case RHI::RayTracingQuality::Off:
            return "Off";
        case RHI::RayTracingQuality::Low:
            return "Low";
        case RHI::RayTracingQuality::Medium:
            return "Medium";
        case RHI::RayTracingQuality::High:
            return "High";
        case RHI::RayTracingQuality::Ultra:
            return "Ultra";
        }
        return "Unknown";
    }

} // namespace Spark::Graphics
