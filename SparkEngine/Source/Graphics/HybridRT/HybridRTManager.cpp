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
#include "../AssetPipeline.h"
#include "../RHI/RHIDevice.h"
#include "../RHI/RHIResources.h"
#include "../../Utils/Validate.h"

#ifdef SPARK_HARDWARE_RT
#include "../RHI/DXRSupport.h"
#endif

#ifdef SPARK_PLATFORM_MACOS
#include "../RHI/Metal/MetalRayTracing.h"
#include "../RHI/Metal/MetalDevice.h"
#endif

#include <algorithm>
#include <cstring>
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
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "HybridRTManager: detected backend '%s'",
                       RayTracingBackendToString(m_activeBackend).c_str());

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

        auto createRT = [&](const char* name) -> std::unique_ptr<RHI::IRHITexture>
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

#ifdef SPARK_PLATFORM_MACOS
        // Spin up the Metal RT scaffold when the detected backend is Metal.
        // Initialize() returns false today (no trace pipelines yet), which
        // leaves `m_metalRT->IsAvailable()` false — the Execute() path then
        // falls back to SDFGI. Keeping the object alive here means once the
        // real pipelines land the wiring is already in place.
        if (m_activeBackend == RHI::RayTracingBackend::HardwareMetalRT)
        {
            auto* metalDevice = dynamic_cast<Spark::RHI::Metal::MetalDevice*>(device);
            if (metalDevice)
            {
                m_metalRT = std::make_unique<Spark::RHI::Metal::MetalRayTracingSystem>();
                (void)m_metalRT->Initialize(metalDevice);
            }
        }
#endif

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "HybridRTManager initialized (%ux%u)", width, height);
        return true;
    }

    void HybridRTManager::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "HybridRTManager shutting down");
        if (m_sdfScene)
            m_sdfScene->Shutdown();
        if (m_compositor)
            m_compositor->Shutdown();
        if (m_probes)
            m_probes->Shutdown();

        m_sdfScene.reset();
        m_compositor.reset();
        m_probes.reset();

#ifdef SPARK_PLATFORM_MACOS
        if (m_metalRT)
            m_metalRT->Shutdown();
        m_metalRT.reset();
        m_metalRTMeshes.clear();
        m_metalRTTLASDirty = false;
#endif

        m_rtReflections.reset();
        m_rtGI.reset();
        m_rtShadows.reset();
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
            auto recreateRT = [&](std::unique_ptr<RHI::IRHITexture>& tex, const char* name)
            {
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
            // Vulkan RT: fall back to SDFGI until the backend matures.
            ExecuteSDFGI(cmd, params, gbufferNormals, gbufferDepth, gbufferAlbedo);
            break;

        case RHI::RayTracingBackend::HardwareMetalRT:
#ifdef SPARK_PLATFORM_MACOS
            // Consult the Metal RT system. Feed this frame's uniforms and
            // GBuffer/output textures, then DispatchFrame. The returned mask
            // tells us which passes actually ran; we back-fill the rest with
            // SDFGI so the compositor always has data to blend.
            if (m_metalRT && m_metalRT->IsAvailable())
            {
                // Rebuild the TLAS when scene pushes invalidated it.
                if (m_metalRTTLASDirty && !m_metalRTMeshes.empty())
                {
                    std::vector<Spark::RHI::Metal::TLASInstance> instances;
                    instances.reserve(m_metalRTMeshes.size());
                    for (const auto& m : m_metalRTMeshes)
                    {
                        Spark::RHI::Metal::TLASInstance inst{};
                        inst.blasIndex = m.blasIndex;
                        std::memcpy(inst.transform, m.transform, sizeof(inst.transform));
                        inst.instanceMask = m.isOpaque ? 0xFF : 0x01;
                        instances.push_back(inst);
                    }
                    m_metalRT->BuildTLAS(instances);
                    m_metalRTTLASDirty = false;
                }
                Spark::RHI::Metal::FrameParams mrtp{};
                DirectX::XMMATRIX viewProj = DirectX::XMMatrixMultiply(view, proj);
                DirectX::XMVECTOR det = DirectX::XMMatrixDeterminant(viewProj);
                DirectX::XMMATRIX invVP = DirectX::XMMatrixInverse(&det, viewProj);
                DirectX::XMFLOAT4X4 invVPStore;
                DirectX::XMStoreFloat4x4(&invVPStore, invVP);
                std::memcpy(mrtp.invViewProj, &invVPStore, sizeof(mrtp.invViewProj));
                mrtp.cameraPos[0] = cameraPos.x;
                mrtp.cameraPos[1] = cameraPos.y;
                mrtp.cameraPos[2] = cameraPos.z;
                mrtp.lightDir[0] = lightDir.x;
                mrtp.lightDir[1] = lightDir.y;
                mrtp.lightDir[2] = lightDir.z;
                mrtp.resolutionX = m_width;
                mrtp.resolutionY = m_height;
                m_metalRT->SetFrameParams(mrtp);
                m_metalRT->SetInputTextures(gbufferDepth, gbufferNormals);
                m_metalRT->SetOutputTextures(m_rtShadows.get(), m_rtReflections.get(),
                                             /*ao*/ nullptr, m_rtGI.get());

                auto allPasses = Spark::RHI::Metal::TracePass::Reflections | Spark::RHI::Metal::TracePass::Shadows |
                                 Spark::RHI::Metal::TracePass::AmbientOcclusion |
                                 Spark::RHI::Metal::TracePass::GlobalIllumination;
                auto executed = m_metalRT->DispatchFrame(allPasses);
                (void)executed;
            }
#endif
            // Any passes Metal RT didn't execute (or all of them if Metal
            // RT isn't available) still need data — run SDFGI as a blanket
            // fallback so the compositor has something to blend.
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
            m_compositor->Composite(cmd, m_rtReflections.get(), m_rtGI.get(), m_rtShadows.get(), ssReflections, ssao,
                                    lightingOutput, gbufferNormals, gbufferAlbedo, lightingOutput, ssrSettings,
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
            m_sdfScene->TraceReflections(cmd, constants, gbufferNormals, gbufferDepth, m_rtReflections.get());
        }

        if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::GlobalIllumination) && m_rtGI)
        {
            constants.traceMode = 1;
            m_sdfScene->TraceGI(cmd, constants, gbufferNormals, gbufferDepth, gbufferAlbedo, m_rtGI.get());
        }

        if (RHI::HasEffect(m_enabledEffects, RHI::RTEffect::Shadows) && m_rtShadows)
        {
            constants.traceMode = 2;
            m_sdfScene->TraceShadows(cmd, constants, gbufferNormals, gbufferDepth, m_rtShadows.get());
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

    void HybridRTManager::PushTriangleMesh(const TriangleMeshDesc& mesh)
    {
#ifdef SPARK_PLATFORM_MACOS
        if (!m_metalRT || !m_metalRT->IsAvailable())
            return;

        Spark::RHI::Metal::BLASGeometry geom{};
        geom.name = mesh.name;
        geom.vertexData = mesh.vertexData;
        geom.vertexCount = mesh.vertexCount;
        geom.vertexStride = mesh.vertexStride;
        geom.indexData = mesh.indexData;
        geom.indexCount = mesh.indexCount;
        geom.isOpaque = mesh.isOpaque;
        geom.allowUpdate = mesh.allowDynamicUpdate;

        const uint32_t blasIndex = m_metalRT->CreateBLAS(geom);
        if (blasIndex == UINT32_MAX)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "HybridRTManager: MetalRT BLAS creation failed for '" + mesh.name + "'");
            return;
        }

        PushedMesh pushed{};
        pushed.blasIndex = blasIndex;
        std::memcpy(pushed.transform, mesh.transform, sizeof(pushed.transform));
        pushed.isOpaque = mesh.isOpaque;
        m_metalRTMeshes.push_back(pushed);
        m_metalRTTLASDirty = true;
#else
        (void)mesh;
#endif
    }

    void HybridRTManager::PushTriangleMesh(const ::MeshAsset& asset, const DirectX::XMMATRIX& worldTransform,
                                           bool allowDynamicUpdate)
    {
        const auto& data = asset.GetMeshData();
        if (data.vertices.empty() || data.indices.size() < 3)
            return;

        // Flatten row-major 3x4 from an XMMATRIX's first 3 rows. `XMStoreFloat4x4`
        // stores row-major so `m[row][col]` lays out exactly the way the Metal
        // instance descriptor expects its 3x4 transform.
        DirectX::XMFLOAT4X4 m;
        DirectX::XMStoreFloat4x4(&m, worldTransform);

        TriangleMeshDesc desc{};
        desc.name = asset.GetPath();
        desc.vertexData = data.vertices.data();
        desc.vertexCount = static_cast<uint32_t>(data.vertices.size());
        desc.vertexStride = sizeof(MeshAssetData::Vertex);
        desc.indexData = data.indices.data();
        desc.indexCount = static_cast<uint32_t>(data.indices.size());
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 4; ++col)
                desc.transform[row * 4 + col] = m.m[row][col];
        }
        desc.isOpaque = true;
        desc.allowDynamicUpdate = allowDynamicUpdate;

        PushTriangleMesh(desc);
    }

    void HybridRTManager::ClearTriangleMeshes()
    {
#ifdef SPARK_PLATFORM_MACOS
        if (m_metalRT)
        {
            for (auto& m : m_metalRTMeshes)
                m_metalRT->DestroyBLAS(m.blasIndex);
            // Rebuild an empty TLAS so the compute passes see no instances.
            m_metalRT->BuildTLAS({});
            // Drop materials along with the geometry so the kernels
            // don't dereference a stale buffer on the next frame.
            m_metalRT->SetMaterials({});
        }
        m_metalRTMeshes.clear();
        m_metalRTTLASDirty = false;
#endif
    }

#ifdef SPARK_PLATFORM_MACOS
    void HybridRTManager::SetMetalMaterials(const std::vector<RHI::Metal::MaterialParams>& materials)
    {
        if (m_metalRT)
            m_metalRT->SetMaterials(materials);
    }
#endif

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

#ifdef SPARK_PLATFORM_MACOS
        if (m_metalRT)
        {
            ss << m_metalRT->GetStatusString() << "\n";
            ss << "Pushed meshes: " << m_metalRTMeshes.size() << (m_metalRTTLASDirty ? " (TLAS rebuild pending)" : "")
               << "\n";
        }
#endif

        return ss.str();
    }

} // namespace Spark::Graphics
