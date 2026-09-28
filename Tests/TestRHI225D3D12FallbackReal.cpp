/**
 * @file TestRHI225D3D12FallbackReal.cpp
 * @brief RHI-225: D3D12 advanced-feature capabilities are exact, stable, and fall back explicitly.
 *
 * D3D12Device::DetectCapabilities/DetectDXRSupport fill the RHI capability flags from
 * CheckFeatureSupport. These tests re-query the same device independently and require every
 * advanced flag to equal the driver's answer, require two initializations to report identical
 * capabilities, and require the DXR entry point to follow its flag: a device without a DXR tier
 * exposes no ID3D12Device5 and selects the software ray-tracing backend, one with a tier exposes
 * it and selects hardware DXR. They run on the adapter D3D12Device selects (the largest hardware
 * adapter, or WARP on a GPU-less host), so hosted and GPU hosts exercise different answers. A host
 * where neither can be created fails. Mesh shaders, enhanced barriers and GPU upload heaps have no
 * RHI entry point yet, so for those only the reported flag is checked.
 */

#include "TestFramework.h"

#if defined(_WIN32) && !defined(SPARK_NO_D3D12)

#include "Graphics/RHI/D3D12/D3D12Device.h"
#include "Graphics/RHI/RHITypes.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>

namespace
{
    using namespace Spark::RHI;
    using Spark::RHI::D3D12::D3D12Device;

    void InitializeDefault(D3D12Device& device)
    {
        RHIDeviceDesc desc;
        desc.applicationName = "SparkTests_RHI225_D3D12Fallback";
        ASSERT_TRUE(device.Initialize(desc));
        const RHIDeviceCapabilities& caps = device.GetCapabilities();
        std::printf("[RHI-225 D3D12 ADAPTER] %s software=%s dxr=%s mesh=%s\n", caps.deviceName.c_str(),
                    caps.isSoftwareDevice ? "yes" : "no", caps.rayTracing.supportsHardwareRT ? "yes" : "no",
                    caps.meshShaderSupport ? "yes" : "no");
    }

    /// The driver's answer for one D3D12 feature struct, or a zeroed struct when the query fails
    /// (the device then reports the feature unsupported).
    template <typename T> T Query(ID3D12Device* device, D3D12_FEATURE feature)
    {
        T data{};
        if (FAILED(device->CheckFeatureSupport(feature, &data, sizeof(data))))
            return T{};
        return data;
    }
} // namespace

TEST(D3D12Fallback_CapabilitiesMatchFeatureQueries)
{
    D3D12Device device;
    InitializeDefault(device);
    ID3D12Device* native = device.GetD3D12Device();
    ASSERT_TRUE(native != nullptr);
    const RHIDeviceCapabilities& caps = device.GetCapabilities();

    DXGI_ADAPTER_DESC1 adapter{};
    ASSERT_TRUE(SUCCEEDED(device.GetAdapter()->GetDesc1(&adapter)));
    EXPECT_EQ(caps.isSoftwareDevice, (adapter.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0);

    const auto options = Query<D3D12_FEATURE_DATA_D3D12_OPTIONS>(native, D3D12_FEATURE_D3D12_OPTIONS);
    EXPECT_EQ(caps.conservativeRasterSupport,
              options.ConservativeRasterizationTier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED);
    EXPECT_EQ(caps.bindlessResourceSupport, options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3);

    const auto options7 = Query<D3D12_FEATURE_DATA_D3D12_OPTIONS7>(native, D3D12_FEATURE_D3D12_OPTIONS7);
    EXPECT_EQ(caps.meshShaderSupport, options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);

    const auto options6 = Query<D3D12_FEATURE_DATA_D3D12_OPTIONS6>(native, D3D12_FEATURE_D3D12_OPTIONS6);
    EXPECT_EQ(caps.variableRateShadingSupport,
              options6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED);

    const auto options5 = Query<D3D12_FEATURE_DATA_D3D12_OPTIONS5>(native, D3D12_FEATURE_D3D12_OPTIONS5);
    const bool dxrTier = options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    EXPECT_EQ(caps.rayTracing.supportsHardwareRT, dxrTier);
    EXPECT_EQ(caps.rayTracing.supportsInlineRT, options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
    EXPECT_EQ(caps.rayTracing.raytracingTier, dxrTier ? static_cast<uint32_t>(options5.RaytracingTier) : 0u);

#ifdef __ID3D12Device10_FWD_DEFINED__
    const auto options12 = Query<D3D12_FEATURE_DATA_D3D12_OPTIONS12>(native, D3D12_FEATURE_D3D12_OPTIONS12);
    EXPECT_EQ(caps.enhancedBarrierSupport, options12.EnhancedBarriersSupported == TRUE);
#else
    EXPECT_FALSE(caps.enhancedBarrierSupport);
#endif
#ifdef __ID3D12Device12_FWD_DEFINED__
    const auto options16 = Query<D3D12_FEATURE_DATA_D3D12_OPTIONS16>(native, D3D12_FEATURE_D3D12_OPTIONS16);
    EXPECT_EQ(caps.hostImageCopySupport, options16.GPUUploadHeapSupported == TRUE);
#else
    EXPECT_FALSE(caps.hostImageCopySupport);
#endif

    // The largest of 8/4/2 samples with at least one RGBA8 quality level.
    uint32_t expectedMsaa = 0;
    for (uint32_t samples = 8; samples >= 2 && expectedMsaa == 0; samples /= 2)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
        levels.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        levels.SampleCount = samples;
        if (SUCCEEDED(native->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) &&
            levels.NumQualityLevels > 0)
        {
            expectedMsaa = samples;
        }
    }
    if (expectedMsaa != 0)
        EXPECT_EQ(caps.maxMSAASamples, expectedMsaa);

    device.Shutdown();
}

TEST(D3D12Fallback_CapabilitiesStableAcrossInit)
{
    D3D12Device first;
    InitializeDefault(first);
    const RHIDeviceCapabilities a = first.GetCapabilities();
    first.Shutdown();

    D3D12Device second;
    InitializeDefault(second);
    const RHIDeviceCapabilities& b = second.GetCapabilities();

    EXPECT_EQ(a.deviceName, b.deviceName);
    EXPECT_EQ(a.isSoftwareDevice, b.isSoftwareDevice);
    EXPECT_EQ(a.conservativeRasterSupport, b.conservativeRasterSupport);
    EXPECT_EQ(a.bindlessResourceSupport, b.bindlessResourceSupport);
    EXPECT_EQ(a.meshShaderSupport, b.meshShaderSupport);
    EXPECT_EQ(a.variableRateShadingSupport, b.variableRateShadingSupport);
    EXPECT_EQ(a.enhancedBarrierSupport, b.enhancedBarrierSupport);
    EXPECT_EQ(a.hostImageCopySupport, b.hostImageCopySupport);
    EXPECT_EQ(a.rayTracingSupport, b.rayTracingSupport);
    EXPECT_EQ(a.rayTracing.supportsHardwareRT, b.rayTracing.supportsHardwareRT);
    EXPECT_EQ(a.rayTracing.supportsInlineRT, b.rayTracing.supportsInlineRT);
    EXPECT_EQ(a.rayTracing.raytracingTier, b.rayTracing.raytracingTier);
    EXPECT_TRUE(a.rayTracing.bestBackend == b.rayTracing.bestBackend);
    EXPECT_EQ(a.maxMSAASamples, b.maxMSAASamples);
    second.Shutdown();
}

TEST(D3D12Fallback_UnsupportedEntryPointsFailExplicitly)
{
    D3D12Device device;
    InitializeDefault(device);
    const RHIDeviceCapabilities& caps = device.GetCapabilities();

    if (caps.rayTracing.supportsHardwareRT)
    {
        // Supported: the DXR device is exposed and hardware DXR is the selected backend.
        EXPECT_TRUE(device.GetDXRDevice() != nullptr);
        EXPECT_TRUE(caps.rayTracing.bestBackend == RayTracingBackend::HardwareDXR);
        EXPECT_EQ(caps.rayTracing.maxRecursionDepth, 31u);
    }
    else
    {
        // Unsupported: no ID3D12Device5 to misuse, the software backend is selected, and no
        // hardware-only detail survives.
        EXPECT_TRUE(device.GetDXRDevice() == nullptr);
        EXPECT_TRUE(caps.rayTracing.bestBackend == RayTracingBackend::Software_SDFGI);
        EXPECT_FALSE(caps.rayTracing.supportsInlineRT);
        EXPECT_EQ(caps.rayTracing.raytracingTier, 0u);
        EXPECT_EQ(caps.rayTracing.maxRecursionDepth, 0u);
    }
    // A hardware-RT-only VRS flag must not outlive VRS itself.
    if (!caps.variableRateShadingSupport)
        EXPECT_FALSE(caps.rayTracing.supportsVRS);
    device.Shutdown();
}

#endif // _WIN32 && !SPARK_NO_D3D12
