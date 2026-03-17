/**
 * @file HybridRTManager.cpp
 * @brief Hybrid ray tracing coordinator implementation
 *
 * Orchestrates the full hybrid RT pipeline:
 *   1. Detect best backend (DXR 1.1, Vulkan RT, or SDFGI software)
 *   2. Build SDF scene from submitted primitives
 *   3. Dispatch trace compute shaders (or DXR DispatchRays)
 *   4. Update irradiance probes for cached GI
 *   5. Composite all results (RT + screen-space) into lighting buffer
 *
 * ## Backend Detection (March 2026 spec)
 * DX12: ID3D12Device::CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5)
 *   → RaytracingTier >= D3D12_RAYTRACING_TIER_1_0 → HardwareDXR
 * Vulkan: vkEnumerateDeviceExtensionProperties()
 *   → VK_KHR_ray_tracing_pipeline + acceleration_structure → HardwareVKRT
 * DX11: CS 5.0 always available → Software_SDFGI
 */

#include "HybridRTManager.h"
#include "../RHI/RHIDevice.h"
#include "../RHI/RHIResources.h"

#ifdef SPARK_HARDWARE_RT
#include "../RHI/DXRSupport.h"
#endif

#include <algorithm>
#include <sstream>

namespace Spark::Graphics
{

    HybridRTManager::~HybridRTManager()
    {
        Shutdown();
    }

    bool HybridRTManager::Initialize(RHI::IRHIDevice* device, uint32_t width, uint32_t height)
    {
        if (!device)
            return false;

        m_device = device;
        m_width = width;
        m_height = height;

        // Detect best available backend from device capabilities
        m_activeBackend = DetectBestBackend();

        // If everything is disabled and no compute support, we can't do anything
        if (m_activeBackend == RHI::RayTracingBackend::Disabled)
        {
            // Still mark as initialized — Execute() will be a no-op
            m_initialized = true;
            return true;
        }

        // Create intermediate RT output textures (half-res by default for Medium quality)
        auto qualityParams = GetQualityParams(m_quality);
        uint32_t rtWidth = static_cast<uint32_t>(static_cast<float>(width) * qualityParams.resolutionScale);
        uint32_t rtHeight = static_cast<uint32_t>(static_cast<float>(height) * qualityParams.resolutionScale);
        rtWidth = std::max(rtWidth, 1u);
        rtHeight = std::max(rtHeight, 1u);

        auto createRT = [&](const char* name) -> RHI::IRHITexture*
        {
            RHI::RHITextureDesc desc;
            desc.width = rtWidth;
            desc.height = rtHeight;
            desc.format = RHI::PixelFormat::R16G16B16A16_FLOAT;
            desc.usage = RHI::RHITextureUsage::ShaderResource | RHI::RHITextureUsage::UnorderedAccess;
            desc.debugName = name;
            return device->CreateTexture(desc);
        };

        m_rtReflections = createRT("RT_Reflections");
        m_rtGI = createRT("RT_GI");
        m_rtShadows = createRT("RT_Shadows");

        // Initialize software SDFGI path (always available as fallback)
        m_sdfScene = std::make_unique<SDFSceneManager>();
        if (!m_sdfScene->Initialize(device, rtWidth, rtHeight))
        {
            m_sdfScene.reset();
        }

        // Initialize compositor
        m_compositor = std::make_unique<RTCompositor>();
        if (!m_compositor->Initialize(device, width, height))
        {
            m_compositor.reset();
        }

        // Initialize probe system for cached GI
        if (qualityParams.enableProbes)
        {
            ProbeGridConfig probeConfig;
            probeConfig.spacing = 2.0f;
            probeConfig.dimensionsX = 16;
            probeConfig.dimensionsY = 8;
            probeConfig.dimensionsZ = 16;
            probeConfig.raysPerProbe = 32;

            m_probes = std::make_unique<ProbeSystem>();
            if (!m_probes->Initialize(device, probeConfig))
            {
                m_probes.reset();
            }
        }

        m_pendingPrimitives.reserve(SDFSceneManager::kMaxPrimitives);
        m_initialized = true;
        return true;
    }

    void HybridRTManager::Shutdown()
    {
        if (m_sdfScene)
            m_sdfScene->Shutdown();
        if (m_compositor)
            m_compositor->Shutdown();
        if (m_probes)
            m_probes->Shutdown();

        m_sdfScene.reset();
        m_compositor.reset();
        m_probes.reset();

        if (m_device)
        {
            if (m_rtReflections)
                m_device->DestroyTexture(m_rtReflections);
            if (m_rtGI)
                m_device->DestroyTexture(m_rtGI);
            if (m_rtShadows)
                m_device->DestroyTexture(m_rtShadows);
        }

        m_rtReflections = nullptr;
        m_rtGI = nullptr;
        m_rtShadows = nullptr;
        m_device = nullptr;
        m_initialized = false;
    }

    void HybridRTManager::Resize(uint32_t width, uint32_t height)
    {
        if (!m_initialized || (width == m_width && height == m_height))
            return;

        m_width = width;
        m_height = height;

        auto qualityParams = GetQualityParams(m_quality);
        uint32_t rtW = std::max(1u, static_cast<uint32_t>(static_cast<float>(width) * qualityParams.resolutionScale));
        uint32_t rtH = std::max(1u, static_cast<uint32_t>(static_cast<float>(height) * qualityParams.resolutionScale));

        // Recreate RT textures at new resolution
        if (m_device)
        {
            auto recreateRT = [&](RHI::IRHITexture*& tex, const char* name)
            {
                if (tex)
                    m_device->DestroyTexture(tex);
                RHI::RHITextureDesc desc;
                desc.width = rtW;
                desc.height = rtH;
                desc.format = RHI::PixelFormat::R16G16B16A16_FLOAT;
                desc.usage = RHI::RHITextureUsage::ShaderResource | RHI::RHITextureUsage::UnorderedAccess;
                desc.debugName = name;
                tex = m_device->CreateTexture(desc);
            };

            recreateRT(m_rtReflections, "RT_Reflections");
            recreateRT(m_rtGI, "RT_GI");
            recreateRT(m_rtShadows, "RT_Shadows");
        }

        if (m_sdfScene)
            m_sdfScene->Resize(rtW, rtH);
        if (m_compositor)
            m_compositor->Resize(width, height);
    }

    void HybridRTManager::Execute(RHI::IRHICommandList* cmd, const DirectX::XMMATRIX& view,
                                  const DirectX::XMMATRIX& proj, const DirectX::XMFLOAT3& cameraPos,
                                  const DirectX::XMFLOAT3& lightDir, RHI::IRHITexture* gbufferNormals,
                                  RHI::IRHITexture* gbufferDepth, RHI::IRHITexture* gbufferAlbedo,
                                  RHI::IRHITexture* ssReflections, RHI::IRHITexture* ssao,
                                  RHI::IRHITexture* lightingOutput, const SSRSettings& ssrSettings)
    {
        if (!m_initialized || !cmd || m_quality == RHI::RayTracingQuality::Off)
            return;

        // Use override if set, otherwise use detected backend
        auto backend = (m_overrideBackend != RHI::RayTracingBackend::Disabled) ? m_overrideBackend : m_activeBackend;

        if (backend == RHI::RayTracingBackend::Disabled)
            return;

        cmd->BeginEvent("HybridRT");

        // Build frame parameters
        RTFrameParams params;
        params.view = view;
        params.proj = proj;
        params.cameraPos = cameraPos;
        params.lightDir = lightDir;
        params.width = m_width;
        params.height = m_height;

        // Update probe grid origin to follow camera
        if (m_probes)
        {
            m_probes->UpdateGridOrigin(cameraPos);
        }

        // Dispatch based on active backend
        switch (backend)
        {
        case RHI::RayTracingBackend::Software_SDFGI:
            ExecuteSDFGI(cmd, params, gbufferNormals, gbufferDepth, gbufferAlbedo);
            break;

        case RHI::RayTracingBackend::HardwareDXR:
#ifdef SPARK_HARDWARE_RT
        {
            // Delegate to existing DXRManager for hardware path
            auto& dxr = DXRManager::GetInstance();
            if (dxr.IsAvailable())
            {
                DirectX::XMMATRIX viewProj = DirectX::XMMatrixMultiply(view, proj);
                if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::Reflections))
                    dxr.TraceReflections(viewProj, cameraPos);
                if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::Shadows))
                    dxr.TraceShadows(lightDir);
                if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::AmbientOcclusion))
                    dxr.TraceAmbientOcclusion(viewProj, cameraPos);
                if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::GlobalIllumination))
                    dxr.TraceGlobalIllumination(viewProj, cameraPos);
            }
        }
#endif
        break;

        case RHI::RayTracingBackend::HardwareVKRT:
            // Vulkan RT: falls through to SDFGI until Vulkan backend matures
            ExecuteSDFGI(cmd, params, gbufferNormals, gbufferDepth, gbufferAlbedo);
            break;

        default:
            break;
        }

        // Update probes with current SDF scene data
        if (m_probes)
        {
            m_probes->UpdateProbes(cmd, lightDir, 1.0f);
        }

        // Composite RT results with screen-space effects
        if (m_compositor && lightingOutput)
        {
            m_compositor->Composite(cmd, m_rtReflections, m_rtGI, m_rtShadows, ssReflections, ssao, lightingOutput,
                                    gbufferNormals, gbufferAlbedo, lightingOutput, ssrSettings,
                                    static_cast<float>(m_frameIndex));
        }

        m_frameIndex++;
        cmd->EndEvent();
    }

    void HybridRTManager::ExecuteSDFGI(RHI::IRHICommandList* cmd, const RTFrameParams& params,
                                       RHI::IRHITexture* gbufferNormals, RHI::IRHITexture* gbufferDepth,
                                       RHI::IRHITexture* gbufferAlbedo)
    {
        if (!m_sdfScene)
            return;

        // Flush pending primitives to GPU
        FlushScene();

        auto qualityParams = GetQualityParams(m_quality);
        uint32_t rtW =
            std::max(1u, static_cast<uint32_t>(static_cast<float>(params.width) * qualityParams.resolutionScale));
        uint32_t rtH =
            std::max(1u, static_cast<uint32_t>(static_cast<float>(params.height) * qualityParams.resolutionScale));

        // Build trace constants
        DirectX::XMMATRIX viewProj = DirectX::XMMatrixMultiply(params.view, params.proj);
        DirectX::XMMATRIX invViewProj = DirectX::XMMatrixInverse(nullptr, viewProj);

        SDFTraceConstants constants;
        DirectX::XMStoreFloat4x4(&constants.invViewProj, invViewProj);
        constants.cameraPos = params.cameraPos;
        constants.maxDistance = qualityParams.maxDistance;
        constants.lightDir = params.lightDir;
        constants.maxSteps = qualityParams.maxSteps;
        constants.primitiveCount = static_cast<int>(m_sdfScene->GetPrimitiveCount());
        constants.outputWidth = static_cast<float>(rtW);
        constants.outputHeight = static_cast<float>(rtH);

        // Dispatch enabled effects
        if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::Reflections) && m_rtReflections)
        {
            constants.traceMode = 0;
            m_sdfScene->TraceReflections(cmd, constants, gbufferNormals, gbufferDepth, m_rtReflections);
        }

        if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::GlobalIllumination) && m_rtGI)
        {
            constants.traceMode = 1;
            m_sdfScene->TraceGI(cmd, constants, gbufferNormals, gbufferDepth, gbufferAlbedo, m_rtGI);
        }

        if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::Shadows) && m_rtShadows)
        {
            constants.traceMode = 2;
            m_sdfScene->TraceShadows(cmd, constants, gbufferNormals, gbufferDepth, m_rtShadows);
        }
    }

    void HybridRTManager::SubmitSDFPrimitive(const SDFPrimitive& primitive)
    {
        if (m_pendingPrimitives.size() < SDFSceneManager::kMaxPrimitives)
        {
            m_pendingPrimitives.push_back(primitive);
        }
    }

    void HybridRTManager::ClearScene()
    {
        m_pendingPrimitives.clear();
    }

    void HybridRTManager::FlushScene()
    {
        if (m_sdfScene && !m_pendingPrimitives.empty())
        {
            m_sdfScene->UpdateScene(m_pendingPrimitives);
        }
    }

    void HybridRTManager::SetQuality(RHI::RayTracingQuality quality)
    {
        if (quality == m_quality)
            return;
        m_quality = quality;

        // Resize intermediate textures for new quality level
        if (m_initialized)
        {
            Resize(m_width, m_height);
        }
    }

    void HybridRTManager::SetBackendOverride(RHI::RayTracingBackend backend)
    {
        m_overrideBackend = backend;
    }

    void HybridRTManager::SetEnabledEffects(RHI::RTEffect effects)
    {
        m_enabledEffects = effects;
    }

    RHI::RayTracingBackend HybridRTManager::DetectBestBackend() const
    {
        if (!m_device)
            return RHI::RayTracingBackend::Disabled;

        const auto& caps = m_device->GetCapabilities();

        // Check hardware RT first (best quality)
        if (caps.rayTracing.supportsHardwareRT)
        {
            if (caps.backend == RHI::GraphicsBackend::D3D12)
                return RHI::RayTracingBackend::HardwareDXR;
            if (caps.backend == RHI::GraphicsBackend::Vulkan)
                return RHI::RayTracingBackend::HardwareVKRT;
        }

        // Software fallback via compute — requires CS 5.0
        if (caps.computeShaderSupport)
            return RHI::RayTracingBackend::Software_SDFGI;

        return RHI::RayTracingBackend::Disabled;
    }

    std::string HybridRTManager::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "=== Hybrid Ray Tracing Status ===\n";
        ss << "Backend: " << RayTracingBackendToString(m_activeBackend) << "\n";
        ss << "Quality: " << RayTracingQualityToString(m_quality) << "\n";

        if (m_overrideBackend != RHI::RayTracingBackend::Disabled)
            ss << "Override: " << RayTracingBackendToString(m_overrideBackend) << "\n";

        auto qp = GetQualityParams(m_quality);
        ss << "Resolution scale: " << qp.resolutionScale << "x\n";
        ss << "Max steps: " << qp.maxSteps << "\n";
        ss << "Max bounces: " << qp.maxBounces << "\n";
        ss << "Max distance: " << qp.maxDistance << "\n";
        ss << "Probes: " << (m_probes ? "active" : "disabled") << "\n";

        if (m_sdfScene)
            ss << "SDF primitives: " << m_sdfScene->GetPrimitiveCount() << " / " << m_sdfScene->GetMaxPrimitives()
               << "\n";

        ss << "Enabled effects: ";
        if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::Reflections))
            ss << "Reflections ";
        if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::GlobalIllumination))
            ss << "GI ";
        if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::Shadows))
            ss << "Shadows ";
        if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::AmbientOcclusion))
            ss << "AO ";
        ss << "\n";

        return ss.str();
    }

} // namespace Spark::Graphics
