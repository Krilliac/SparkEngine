// TestHybridRT.cpp - Tests for the hybrid ray tracing system
// Tests backend selection logic, SDF math, quality presets, and type conversions

#include "TestFramework.h"
#include "Graphics/RHI/RHITypes.h"
#include "Graphics/HybridRT/HybridRTTypes.h"

using namespace Spark::RHI;
using namespace Spark::Graphics;

// ============================================================================
// Backend Selection Tests
// ============================================================================

TEST(HybridRT_BackendEnum_HasAllValues)
{
    // Verify all backend values exist and are distinct
    EXPECT_NE(static_cast<int>(RayTracingBackend::Disabled), static_cast<int>(RayTracingBackend::Software_SDFGI));
    EXPECT_NE(static_cast<int>(RayTracingBackend::Software_SDFGI), static_cast<int>(RayTracingBackend::HardwareDXR));
    EXPECT_NE(static_cast<int>(RayTracingBackend::HardwareDXR), static_cast<int>(RayTracingBackend::HardwareVKRT));
    EXPECT_NE(static_cast<int>(RayTracingBackend::Software_VCT), static_cast<int>(RayTracingBackend::Software_SDFGI));
}

TEST(HybridRT_QualityEnum_HasAllValues)
{
    EXPECT_NE(static_cast<int>(RayTracingQuality::Off), static_cast<int>(RayTracingQuality::Low));
    EXPECT_NE(static_cast<int>(RayTracingQuality::Low), static_cast<int>(RayTracingQuality::Medium));
    EXPECT_NE(static_cast<int>(RayTracingQuality::Medium), static_cast<int>(RayTracingQuality::High));
    EXPECT_NE(static_cast<int>(RayTracingQuality::High), static_cast<int>(RayTracingQuality::Ultra));
}

TEST(HybridRT_RTCapabilities_DefaultDisabled)
{
    RTCapabilities caps;
    EXPECT_EQ(static_cast<int>(caps.bestBackend), static_cast<int>(RayTracingBackend::Disabled));
    EXPECT_FALSE(caps.supportsHardwareRT);
    EXPECT_FALSE(caps.supportsInlineRT);
    EXPECT_EQ(caps.maxRecursionDepth, 0u);
    EXPECT_FALSE(caps.supportsVRS);
}

TEST(HybridRT_RTCapabilities_InDeviceCaps)
{
    RHIDeviceCapabilities deviceCaps;
    // Verify RTCapabilities is accessible as a member
    deviceCaps.rayTracing.bestBackend = RayTracingBackend::Software_SDFGI;
    deviceCaps.rayTracing.supportsHardwareRT = false;
    EXPECT_EQ(static_cast<int>(deviceCaps.rayTracing.bestBackend),
              static_cast<int>(RayTracingBackend::Software_SDFGI));
    EXPECT_FALSE(deviceCaps.rayTracing.supportsHardwareRT);
}

TEST(HybridRT_RTCapabilities_HardwareDXR)
{
    RTCapabilities caps;
    caps.bestBackend = RayTracingBackend::HardwareDXR;
    caps.supportsHardwareRT = true;
    caps.supportsInlineRT = true;
    caps.maxRecursionDepth = 31;
    caps.raytracingTier = 11; // D3D12_RAYTRACING_TIER_1_1

    EXPECT_TRUE(caps.supportsHardwareRT);
    EXPECT_TRUE(caps.supportsInlineRT);
    EXPECT_EQ(caps.maxRecursionDepth, 31u);
}

// ============================================================================
// Quality Preset Tests
// ============================================================================

TEST(HybridRT_QualityParams_Off)
{
    auto p = GetQualityParams(RayTracingQuality::Off);
    EXPECT_EQ(p.maxSteps, 0);
    EXPECT_EQ(p.maxBounces, 0);
    EXPECT_EQ(p.resolutionScale, 0.0f);
}

TEST(HybridRT_QualityParams_Low)
{
    auto p = GetQualityParams(RayTracingQuality::Low);
    EXPECT_EQ(p.resolutionScale, 0.25f);
    EXPECT_EQ(p.maxSteps, 32);
    EXPECT_EQ(p.maxBounces, 1);
    EXPECT_EQ(p.maxDistance, 50.0f);
    EXPECT_FALSE(p.enableDenoising);
    EXPECT_FALSE(p.enableProbes);
}

TEST(HybridRT_QualityParams_Medium)
{
    auto p = GetQualityParams(RayTracingQuality::Medium);
    EXPECT_EQ(p.resolutionScale, 0.5f);
    EXPECT_EQ(p.maxSteps, 64);
    EXPECT_EQ(p.maxBounces, 2);
    EXPECT_EQ(p.maxDistance, 100.0f);
    EXPECT_TRUE(p.enableProbes);
}

TEST(HybridRT_QualityParams_High)
{
    auto p = GetQualityParams(RayTracingQuality::High);
    EXPECT_EQ(p.resolutionScale, 1.0f);
    EXPECT_EQ(p.maxSteps, 96);
    EXPECT_TRUE(p.enableDenoising);
    EXPECT_TRUE(p.enableProbes);
}

TEST(HybridRT_QualityParams_Ultra)
{
    auto p = GetQualityParams(RayTracingQuality::Ultra);
    EXPECT_EQ(p.resolutionScale, 1.0f);
    EXPECT_EQ(p.maxSteps, 128);
    EXPECT_EQ(p.maxBounces, 3);
    EXPECT_EQ(p.maxDistance, 200.0f);
    EXPECT_TRUE(p.enableDenoising);
    EXPECT_TRUE(p.enableProbes);
    EXPECT_TRUE(p.enableTemporalAccum);
}

// ============================================================================
// RT Effect Flag Tests
// ============================================================================

TEST(HybridRT_RTEffect_BitwiseOr)
{
    auto combined = RTEffect::Reflections | RTEffect::Shadows;
    EXPECT_TRUE(HasEffect(combined, RTEffect::Reflections));
    EXPECT_TRUE(HasEffect(combined, RTEffect::Shadows));
    EXPECT_FALSE(HasEffect(combined, RTEffect::GlobalIllumination));
}

TEST(HybridRT_RTEffect_All)
{
    EXPECT_TRUE(HasEffect(RTEffect::All, RTEffect::Reflections));
    EXPECT_TRUE(HasEffect(RTEffect::All, RTEffect::GlobalIllumination));
    EXPECT_TRUE(HasEffect(RTEffect::All, RTEffect::Shadows));
    EXPECT_TRUE(HasEffect(RTEffect::All, RTEffect::AmbientOcclusion));
}

TEST(HybridRT_RTEffect_None)
{
    EXPECT_FALSE(HasEffect(RTEffect::None, RTEffect::Reflections));
    EXPECT_FALSE(HasEffect(RTEffect::None, RTEffect::Shadows));
}

// ============================================================================
// SDF Primitive Type Tests
// ============================================================================

TEST(HybridRT_SDFPrimitive_Size)
{
    // SDFPrimitive must be 64 bytes for GPU structured buffer alignment
    EXPECT_EQ(sizeof(SDFPrimitive), 64u);
}

TEST(HybridRT_SDFPrimitive_DefaultValues)
{
    SDFPrimitive prim;
    EXPECT_EQ(prim.radius, 1.0f);
    EXPECT_EQ(prim.roughness, 0.5f);
    EXPECT_EQ(prim.metallic, 0.0f);
    EXPECT_EQ(prim.type, 0u); // Sphere by default
}

TEST(HybridRT_SDFPrimitiveType_Values)
{
    EXPECT_EQ(static_cast<uint32_t>(SDFPrimitiveType::Sphere), 0u);
    EXPECT_EQ(static_cast<uint32_t>(SDFPrimitiveType::Box), 1u);
    EXPECT_EQ(static_cast<uint32_t>(SDFPrimitiveType::Capsule), 2u);
    EXPECT_EQ(static_cast<uint32_t>(SDFPrimitiveType::RoundedBox), 3u);
}

// ============================================================================
// String Conversion Tests
// ============================================================================

TEST(HybridRT_BackendToString)
{
    EXPECT_EQ(RayTracingBackendToString(RayTracingBackend::Disabled), "Disabled");
    EXPECT_EQ(RayTracingBackendToString(RayTracingBackend::Software_SDFGI), "Software (SDFGI)");
    EXPECT_EQ(RayTracingBackendToString(RayTracingBackend::HardwareDXR), "Hardware (DXR 1.1)");
    EXPECT_EQ(RayTracingBackendToString(RayTracingBackend::HardwareVKRT), "Hardware (Vulkan RT)");
}

TEST(HybridRT_QualityToString)
{
    EXPECT_EQ(RayTracingQualityToString(RayTracingQuality::Off), "Off");
    EXPECT_EQ(RayTracingQualityToString(RayTracingQuality::Low), "Low");
    EXPECT_EQ(RayTracingQualityToString(RayTracingQuality::Medium), "Medium");
    EXPECT_EQ(RayTracingQualityToString(RayTracingQuality::High), "High");
    EXPECT_EQ(RayTracingQualityToString(RayTracingQuality::Ultra), "Ultra");
}

// ============================================================================
// SDFTraceConstants Layout Tests
// ============================================================================

TEST(HybridRT_SDFTraceConstants_DefaultValues)
{
    SDFTraceConstants constants;
    EXPECT_EQ(constants.maxDistance, 100.0f);
    EXPECT_EQ(constants.maxSteps, 64);
    EXPECT_EQ(constants.primitiveCount, 0);
    EXPECT_EQ(constants.traceMode, 0);
    EXPECT_EQ(constants.sdfBias, 0.001f);
    EXPECT_EQ(constants.shadowSoftness, 16.0f);
    EXPECT_EQ(constants.giIntensity, 1.0f);
    EXPECT_EQ(constants.temporalBlend, 0.9f);
}

TEST(HybridRT_SDFTraceConstants_Alignment)
{
    // Must be 16-byte aligned for GPU constant buffer upload
    EXPECT_EQ(alignof(SDFTraceConstants), 16u);
}
