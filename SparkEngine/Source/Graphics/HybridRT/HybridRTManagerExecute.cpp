/**
 * @file HybridRTManagerExecute.cpp
 * @brief HybridRTManager per-frame execution: the Execute() backend dispatch
 *        (SDFGI software, DXR, Vulkan RT fallback, Metal RT with TLAS
 *        rebuild), the probe update + compositor follow-up, and the
 *        ExecuteSDFGI compute trace path. Split from HybridRTManager.cpp.
 */

#include "HybridRTManager.h"
#include "../RHI/RHIDevice.h"
#include "../RHI/RHIResources.h"

#ifdef SPARK_HARDWARE_RT
#include "../RHI/DXRSupport.h"
#endif

#ifdef SPARK_METAL_SUPPORT
#include "../RHI/Metal/MetalRayTracing.h"
#endif

#include <algorithm>
#include <cstring>

namespace Spark::Graphics
{

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
#ifdef SPARK_METAL_SUPPORT
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
                DirectX::XMMATRIX invVP = DirectX::XMMatrixInverse(nullptr, viewProj);
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

} // namespace Spark::Graphics
