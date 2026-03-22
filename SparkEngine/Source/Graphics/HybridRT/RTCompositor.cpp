/**
 * @file RTCompositor.cpp
 * @brief RT/SS result compositing with temporal accumulation
 *
 * Dispatches RTComposite.hlsl to blend screen-space and ray-traced results.
 * Coordinates with existing SSR via SSRSettings to determine fallback weights.
 */

#include "RTCompositor.h"
#include "../RHI/RHIDevice.h"
#include "../RHI/RHIResources.h"

namespace Spark::Graphics
{

    RTCompositor::~RTCompositor()
    {
        Shutdown();
    }

    bool RTCompositor::Initialize(RHI::IRHIDevice* device, uint32_t width, uint32_t height)
    {
        if (!device)
            return false;

        m_device = device;
        m_width = width;
        m_height = height;

        // Create composite compute shader
        RHI::RHIShaderDesc csDesc;
        csDesc.stage = RHI::RHIShaderStage::Compute;
        csDesc.filePath = "Shaders/HLSL/RayTracing/RTComposite.hlsl";
        csDesc.entryPoint = "CSComposite";
        csDesc.debugName = "RT_Composite_CS";
        m_compositeCS = device->CreateShader(csDesc);

        // Constant buffer
        RHI::RHIBufferDesc cbDesc;
        cbDesc.size = sizeof(CompositeConstants);
        cbDesc.usage = RHI::RHIBufferUsage::Constant;
        cbDesc.access = RHI::RHIBufferAccess::Dynamic;
        cbDesc.debugName = "RT_CompositeConstants";
        m_constantBuffer = device->CreateBuffer(cbDesc);

        // History buffer for temporal accumulation
        RHI::RHITextureDesc histDesc;
        histDesc.width = width;
        histDesc.height = height;
        histDesc.format = RHI::PixelFormat::R16G16B16A16_FLOAT;
        histDesc.usage = RHI::RHITextureUsage::ShaderResource | RHI::RHITextureUsage::UnorderedAccess;
        histDesc.debugName = "RT_HistoryBuffer";
        m_historyBuffer = device->CreateTexture(histDesc);

        m_constants.screenWidth = static_cast<float>(width);
        m_constants.screenHeight = static_cast<float>(height);

        m_initialized = (m_compositeCS && m_constantBuffer && m_historyBuffer);
        return m_initialized;
    }

    void RTCompositor::Shutdown()
    {
        if (!m_device)
            return;

        m_compositeCS.reset();
        m_constantBuffer.reset();
        m_historyBuffer.reset();
        m_device = nullptr;
        m_initialized = false;
    }

    void RTCompositor::Resize(uint32_t width, uint32_t height)
    {
        if (width == m_width && height == m_height)
            return;

        m_width = width;
        m_height = height;
        m_constants.screenWidth = static_cast<float>(width);
        m_constants.screenHeight = static_cast<float>(height);

        // Recreate history buffer at new size
        if (m_historyBuffer && m_device)
        {
            RHI::RHITextureDesc histDesc;
            histDesc.width = width;
            histDesc.height = height;
            histDesc.format = RHI::PixelFormat::R16G16B16A16_FLOAT;
            histDesc.usage = RHI::RHITextureUsage::ShaderResource | RHI::RHITextureUsage::UnorderedAccess;
            histDesc.debugName = "RT_HistoryBuffer";
            m_historyBuffer = m_device->CreateTexture(histDesc);
        }
    }

    void RTCompositor::Composite(RHI::IRHICommandList* cmd, RHI::IRHITexture* rtReflections, RHI::IRHITexture* rtGI,
                                 RHI::IRHITexture* rtShadows, RHI::IRHITexture* ssReflections, RHI::IRHITexture* ssao,
                                 RHI::IRHITexture* lightingBuffer, RHI::IRHITexture* gbufferNormals,
                                 RHI::IRHITexture* gbufferAlbedo, RHI::IRHITexture* output,
                                 const SSRSettings& ssrSettings, float frameIndex)
    {
        if (!m_initialized || !cmd || !output)
            return;

        cmd->BeginEvent("RT_Composite");

        // Coordinate SSR fallback weight with existing screen-space settings:
        // If SSR is enabled and high quality, use more SS; if disabled, rely fully on RT
        m_constants.ssrFallbackWeight = ssrSettings.enabled ? ssrSettings.temporalBlend * 0.5f : 0.0f;
        m_constants.frameIndex = frameIndex;

        // Update constant buffer
        m_device->UpdateBuffer(m_constantBuffer.get(), &m_constants, sizeof(CompositeConstants));

        // Bind all inputs as SRVs (t0..t8)
        cmd->SetConstantBuffer(RHI::RHIShaderStage::Compute, 0, m_constantBuffer.get());
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 0, rtReflections);
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 1, rtGI);
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 2, rtShadows);
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 3, ssReflections);
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 4, ssao);
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 5, lightingBuffer);
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 6, gbufferNormals);
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 7, gbufferAlbedo);
        cmd->SetShaderResource(RHI::RHIShaderStage::Compute, 8, m_historyBuffer.get());

        // Dispatch composite
        uint32_t dispatchX = (m_width + 7) / 8;
        uint32_t dispatchY = (m_height + 7) / 8;
        cmd->Dispatch(dispatchX, dispatchY, 1);

        // Copy composite output to history buffer for next frame's temporal accumulation
        if (m_historyBuffer && output)
        {
            cmd->CopyTexture(m_historyBuffer.get(), output);
        }

        cmd->EndEvent();
    }

    void RTCompositor::SetStrengths(float reflections, float gi, float shadows)
    {
        m_constants.reflectionStrength = reflections;
        m_constants.giStrength = gi;
        m_constants.shadowStrength = shadows;
    }

    void RTCompositor::SetTemporalBlend(float blend)
    {
        m_constants.temporalBlend = blend;
    }

} // namespace Spark::Graphics
