/**
 * @file SDFSceneManager.cpp
 * @brief SDF scene management and SDFGI compute shader dispatch
 *
 * Implements the software ray tracing fallback using SDF sphere tracing.
 * Works on any backend that supports compute shaders (DX11 CS 5.0+).
 *
 * Key data flow:
 *   1. UpdateScene() uploads SDF primitives to structured buffer
 *   2. TraceReflections/GI/Shadows() dispatch SDFTrace.hlsl compute shaders
 *   3. Output textures consumed by RTCompositor for final compositing
 */

#include "SDFSceneManager.h"
#include "../RHI/RHIDevice.h"
#include "../RHI/RHIResources.h"
#include "../../Utils/Validate.h"
#include "../../Utils/LogMacros.h"

#include <algorithm>
#include <cstring>

namespace Spark::Graphics
{

    SDFSceneManager::~SDFSceneManager()
    {
        Shutdown();
    }

    bool SDFSceneManager::Initialize(RHI::IRHIDevice* device, uint32_t width, uint32_t height)
    {
        if (!device)
            return false;

        // Require compute shader support for SDFGI
        if (!device->GetCapabilities().computeShaderSupport)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "SDFSceneManager: compute shader support not available, cannot initialize");
            return false;
        }

        m_device = device;
        m_width = width;
        m_height = height;

        // Create structured buffer for SDF primitives (64 bytes each)
        RHI::RHIBufferDesc primBufDesc;
        primBufDesc.size = sizeof(SDFPrimitive) * kMaxPrimitives;
        primBufDesc.stride = sizeof(SDFPrimitive);
        primBufDesc.usage = RHI::RHIBufferUsage::Structured | RHI::RHIBufferUsage::Storage;
        primBufDesc.access = RHI::RHIBufferAccess::Dynamic;
        primBufDesc.debugName = "SDF_PrimitiveBuffer";
        m_primitiveBuffer = device->CreateBuffer(primBufDesc);
        if (!m_primitiveBuffer)
            return false;

        // Create constant buffer for trace parameters
        RHI::RHIBufferDesc cbDesc;
        cbDesc.size = sizeof(SDFTraceConstants);
        cbDesc.usage = RHI::RHIBufferUsage::Constant;
        cbDesc.access = RHI::RHIBufferAccess::Dynamic;
        cbDesc.debugName = "SDF_TraceConstants";
        m_traceConstantBuffer = device->CreateBuffer(cbDesc);
        if (!m_traceConstantBuffer)
            return false;

        // Load compute shaders for each trace mode
        RHI::RHIShaderDesc reflCS;
        reflCS.stage = RHI::RHIShaderStage::Compute;
        reflCS.filePath = "Shaders/HLSL/RayTracing/SDFTrace.hlsl";
        reflCS.entryPoint = "CSTraceReflections";
        reflCS.debugName = "SDFGI_TraceReflections_CS";
        m_traceReflectionsCS = device->CreateShader(reflCS);

        RHI::RHIShaderDesc giCS;
        giCS.stage = RHI::RHIShaderStage::Compute;
        giCS.filePath = "Shaders/HLSL/RayTracing/SDFTrace.hlsl";
        giCS.entryPoint = "CSTraceGI";
        giCS.debugName = "SDFGI_TraceGI_CS";
        m_traceGICS = device->CreateShader(giCS);

        RHI::RHIShaderDesc shadowCS;
        shadowCS.stage = RHI::RHIShaderStage::Compute;
        shadowCS.filePath = "Shaders/HLSL/RayTracing/SDFTrace.hlsl";
        shadowCS.entryPoint = "CSTraceShadows";
        shadowCS.debugName = "SDFGI_TraceShadows_CS";
        m_traceShadowsCS = device->CreateShader(shadowCS);

        // Generate noise data using existing SSAOKernel utilities for trace jitter
        // This reuses the ScreenSpaceEffects infrastructure rather than duplicating it
        m_noiseData = SSAOKernel::GenerateNoiseTexture(4, 42);

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SDFSceneManager initialized (%ux%u, max %u primitives)", width,
                       height, kMaxPrimitives);
        return true;
    }

    void SDFSceneManager::Shutdown()
    {
        if (!m_device)
            return;

        m_primitiveBuffer.reset();
        m_traceConstantBuffer.reset();
        m_traceReflectionsCS.reset();
        m_traceGICS.reset();
        m_traceShadowsCS.reset();
        m_device = nullptr;
        m_initialized = false;
    }

    void SDFSceneManager::Resize(uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;
    }

    void SDFSceneManager::UpdateScene(const std::vector<SDFPrimitive>& primitives)
    {
        if (!m_initialized || !m_primitiveBuffer)
            return;

        m_primitiveCount = static_cast<uint32_t>(std::min(primitives.size(), static_cast<size_t>(kMaxPrimitives)));
        SPARK_LOG_TRACE(Spark::LogCategory::Graphics, "Updating SDF scene with %u primitives", m_primitiveCount);

        if (m_primitiveCount > 0)
        {
            m_device->UpdateBuffer(m_primitiveBuffer.get(), primitives.data(), sizeof(SDFPrimitive) * m_primitiveCount);
        }
    }

    void SDFSceneManager::TraceReflections(RHI::IRHICommandList* cmd, const SDFTraceConstants& constants,
                                           RHI::IRHITexture* gbufferNormals, RHI::IRHITexture* gbufferDepth,
                                           RHI::IRHITexture* output)
    {
        if (!m_initialized || !m_traceReflectionsCS || m_primitiveCount == 0)
            return;

        RHI::IRHITexture* srvs[] = {gbufferNormals, gbufferDepth};
        DispatchTrace(cmd, m_traceReflectionsCS.get(), constants, srvs, 2, output);
    }

    void SDFSceneManager::TraceGI(RHI::IRHICommandList* cmd, const SDFTraceConstants& constants,
                                  RHI::IRHITexture* gbufferNormals, RHI::IRHITexture* gbufferDepth,
                                  RHI::IRHITexture* gbufferAlbedo, RHI::IRHITexture* output)
    {
        if (!m_initialized || !m_traceGICS || m_primitiveCount == 0)
            return;

        RHI::IRHITexture* srvs[] = {gbufferNormals, gbufferDepth, gbufferAlbedo};
        DispatchTrace(cmd, m_traceGICS.get(), constants, srvs, 3, output);
    }

    void SDFSceneManager::TraceShadows(RHI::IRHICommandList* cmd, const SDFTraceConstants& constants,
                                       RHI::IRHITexture* gbufferNormals, RHI::IRHITexture* gbufferDepth,
                                       RHI::IRHITexture* output)
    {
        if (!m_initialized || !m_traceShadowsCS || m_primitiveCount == 0)
            return;

        RHI::IRHITexture* srvs[] = {gbufferNormals, gbufferDepth};
        DispatchTrace(cmd, m_traceShadowsCS.get(), constants, srvs, 2, output);
    }

    void SDFSceneManager::DispatchTrace(RHI::IRHICommandList* cmd, RHI::IRHIShader* shader,
                                        const SDFTraceConstants& constants, RHI::IRHITexture** srvs, uint32_t srvCount,
                                        RHI::IRHITexture* output)
    {
        if (!cmd || !shader || !output)
            return;

        cmd->BeginEvent("SDFGI_Trace");

        // Update constant buffer with trace parameters
        m_device->UpdateBuffer(m_traceConstantBuffer.get(), &constants, sizeof(SDFTraceConstants));

        // Bind resources:
        // t0 = SDF primitive structured buffer (bound as shader resource on the buffer)
        // t1..tN = GBuffer textures
        // u0 = Output UAV texture
        // b0 = Trace constants

        cmd->SetConstantBuffer(RHI::RHIShaderStage::Compute, 0, m_traceConstantBuffer.get());

        // Bind GBuffer SRVs starting at slot 1 (slot 0 is the structured buffer, bound separately)
        for (uint32_t i = 0; i < srvCount; ++i)
        {
            cmd->SetShaderResource(RHI::RHIShaderStage::Compute, i + 1, srvs[i]);
        }

        // Dispatch compute shader: ceil(width/8) x ceil(height/8) x 1
        uint32_t dispatchX = (m_width + 7) / 8;
        uint32_t dispatchY = (m_height + 7) / 8;
        cmd->Dispatch(dispatchX, dispatchY, 1);

        cmd->EndEvent();
    }

} // namespace Spark::Graphics
