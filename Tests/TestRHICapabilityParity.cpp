/**
 * @file TestRHICapabilityParity.cpp
 * @brief Tests normalization/parity logic for cross-backend RHI capabilities
 */

#include "TestFramework.h"
#include "Graphics/RHI/RHITypes.h"

using namespace Spark::RHI;

TEST(RHICaps_Finalize_EnablesLegacyRTFlagForSoftwarePath)
{
    RHIDeviceCapabilities caps;
    caps.rayTracing.bestBackend = RayTracingBackend::Software_SDFGI;
    caps.computeShaderSupport = true;

    FinalizeDeviceCapabilities(caps);

    EXPECT_TRUE(caps.rayTracingSupport);
    EXPECT_EQ(static_cast<int>(caps.rayTracing.bestBackend), static_cast<int>(RayTracingBackend::Software_SDFGI));
}

TEST(RHICaps_Finalize_DisablesIncoherentRTFieldsWhenRTIsOff)
{
    RHIDeviceCapabilities caps;
    caps.rayTracing.bestBackend = RayTracingBackend::Disabled;
    caps.rayTracing.supportsHardwareRT = true;
    caps.rayTracing.supportsInlineRT = true;
    caps.rayTracing.maxRecursionDepth = 8;
    caps.rayTracing.supportsVRS = true;
    caps.rayTracing.raytracingTier = 2;

    FinalizeDeviceCapabilities(caps);

    EXPECT_FALSE(caps.rayTracingSupport);
    EXPECT_FALSE(caps.rayTracing.supportsHardwareRT);
    EXPECT_FALSE(caps.rayTracing.supportsInlineRT);
    EXPECT_EQ(caps.rayTracing.maxRecursionDepth, 0u);
    EXPECT_FALSE(caps.rayTracing.supportsVRS);
    EXPECT_EQ(caps.rayTracing.raytracingTier, 0u);
}

TEST(RHICaps_Finalize_ClampsMinimumCapabilityValues)
{
    RHIDeviceCapabilities caps;
    caps.maxTextureSize = 0;
    caps.maxRenderTargets = 0;
    caps.maxSamplers = 0;
    caps.maxConstantBuffers = 0;
    caps.maxVertexAttributes = 0;
    caps.maxMSAASamples = 0;
    caps.maxAnisotropy = 0.0f;

    FinalizeDeviceCapabilities(caps);

    EXPECT_EQ(caps.maxTextureSize, 1u);
    EXPECT_EQ(caps.maxRenderTargets, 1u);
    EXPECT_EQ(caps.maxSamplers, 1u);
    EXPECT_EQ(caps.maxConstantBuffers, 1u);
    EXPECT_EQ(caps.maxVertexAttributes, 1u);
    EXPECT_EQ(caps.maxMSAASamples, 1u);
    EXPECT_EQ(caps.maxAnisotropy, 1.0f);
}

TEST(RHICaps_Finalize_ClearsRTVRSWhenGlobalVRSIsUnavailable)
{
    RHIDeviceCapabilities caps;
    caps.variableRateShadingSupport = false;
    caps.rayTracing.bestBackend = RayTracingBackend::HardwareDXR;
    caps.rayTracing.supportsVRS = true;

    FinalizeDeviceCapabilities(caps);

    EXPECT_TRUE(caps.rayTracingSupport);
    EXPECT_FALSE(caps.rayTracing.supportsVRS);
}
