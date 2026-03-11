/**
 * @file RenderGraphBuilder.cpp
 * @brief Implementation of StandardPipelineBuilder (R3.3 -- Activate RenderGraph pipeline)
 * @author Spark Engine Team
 * @date 2026
 */

#include "RenderGraphBuilder.h"

#include <cassert>
#include <sstream>

namespace Spark::Graphics
{

    // ========================================================================
    // Configuration
    // ========================================================================

    void StandardPipelineBuilder::Configure(const PipelineConfig& config)
    {
        m_config = config;
    }

    void StandardPipelineBuilder::SetFrameData(const PipelineFrameData& frameData)
    {
        m_frameData = frameData;
    }

    void StandardPipelineBuilder::SetCallbacks(const PipelineCallbacks& callbacks)
    {
        m_callbacks = callbacks;
    }

    // ========================================================================
    // Descriptor helpers
    // ========================================================================

    RenderGraphTextureDesc StandardPipelineBuilder::MakeColorDesc(RenderTargetFormat format) const
    {
        RenderGraphTextureDesc desc{};
        desc.width = m_config.GetScaledWidth();
        desc.height = m_config.GetScaledHeight();
        desc.depth = 1;
        desc.arraySize = 1;
        desc.mipLevels = 1;
        desc.sampleCount = 1;
        desc.format = format;
        desc.usage = RenderTargetUsage::RenderTarget | RenderTargetUsage::ShaderResource;
        desc.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
        return desc;
    }

    RenderGraphTextureDesc StandardPipelineBuilder::MakeDepthDesc() const
    {
        RenderGraphTextureDesc desc{};
        desc.width = m_config.GetScaledWidth();
        desc.height = m_config.GetScaledHeight();
        desc.depth = 1;
        desc.arraySize = 1;
        desc.mipLevels = 1;
        desc.sampleCount = 1;
        desc.format = RenderTargetFormat::D32_FLOAT;
        desc.usage = RenderTargetUsage::DepthStencil | RenderTargetUsage::ShaderResource;
        desc.clearDepth = 1.0f;
        desc.clearStencil = 0;
        return desc;
    }

    RenderGraphTextureDesc StandardPipelineBuilder::MakeShadowDesc() const
    {
        RenderGraphTextureDesc desc{};
        desc.width = m_config.shadowMapSize;
        desc.height = m_config.shadowMapSize;
        desc.depth = 1;
        desc.arraySize = m_config.shadowCascades;
        desc.mipLevels = 1;
        desc.sampleCount = 1;
        desc.format = RenderTargetFormat::D32_FLOAT;
        desc.usage = RenderTargetUsage::DepthStencil | RenderTargetUsage::ShaderResource;
        desc.clearDepth = 1.0f;
        desc.clearStencil = 0;
        return desc;
    }

    // ========================================================================
    // Build -- top-level graph construction
    // ========================================================================

    void StandardPipelineBuilder::Build(RenderGraph& graph)
    {
        // Passes are added in dependency order.  The RenderGraph compiler
        // will verify and reorder if needed, but adding them in the natural
        // order makes the setup lambdas straightforward.

        if (m_config.shadowsEnabled)
        {
            AddShadowPass(graph);
        }

        if (m_config.deferredEnabled)
        {
            AddGBufferPass(graph);
        }

        AddLightingPass(graph);
        AddPostProcessPass(graph);

        if (m_config.uiEnabled)
        {
            AddUIPass(graph);
        }

        if (m_config.debugPassEnabled)
        {
            AddDebugPass(graph);
        }
    }

    // ========================================================================
    // Shadow pass
    // ========================================================================

    void StandardPipelineBuilder::AddShadowPass(RenderGraph& graph)
    {
        // Capture frame data and callbacks by value for the execute lambda.
        const auto frameData = m_frameData;
        const auto executeFn = m_callbacks.shadowExecute;

        graph.AddPass(
            "ShadowPass", RenderGraphPassType::Graphics,
            [this, &graph](RenderGraphBuilder& builder)
            {
                auto& data = graph.GetBlackboard().Add<ShadowPassData>();

                auto shadowDesc = MakeShadowDesc();
                data.shadowAtlas = builder.Create("ShadowAtlas", shadowDesc);
                data.shadowAtlas = builder.Write(data.shadowAtlas);
                data.cascadeCount = m_config.shadowCascades;
            },
            [executeFn, frameData](const RenderGraphResourceRegistry& registry)
            {
                if (executeFn)
                {
                    executeFn(registry, frameData);
                }
            });
    }

    // ========================================================================
    // GBuffer pass
    // ========================================================================

    void StandardPipelineBuilder::AddGBufferPass(RenderGraph& graph)
    {
        const auto frameData = m_frameData;
        const auto executeFn = m_callbacks.gBufferExecute;

        graph.AddPass(
            "GBufferPass", RenderGraphPassType::Graphics,
            [this, &graph](RenderGraphBuilder& builder)
            {
                auto& data = graph.GetBlackboard().Add<GBufferPassData>();

                // Albedo: RGBA8 sRGB
                auto albedoDesc = MakeColorDesc(RenderTargetFormat::RGBA8_UNORM);
                data.albedo = builder.Create("GBuffer_Albedo", albedoDesc);
                data.albedo = builder.Write(data.albedo);

                // Normals: high-precision half-float
                auto normalDesc = MakeColorDesc(RenderTargetFormat::RGBA16_FLOAT);
                data.normals = builder.Create("GBuffer_Normals", normalDesc);
                data.normals = builder.Write(data.normals);

                // Material properties (roughness, metalness, AO, specular)
                auto materialDesc = MakeColorDesc(RenderTargetFormat::RGBA8_UNORM);
                data.material = builder.Create("GBuffer_Material", materialDesc);
                data.material = builder.Write(data.material);

                // Motion vectors
                auto motionDesc = MakeColorDesc(RenderTargetFormat::RG16_FLOAT);
                data.motion = builder.Create("GBuffer_Motion", motionDesc);
                data.motion = builder.Write(data.motion);

                // Depth
                auto depthDesc = MakeDepthDesc();
                data.depth = builder.Create("GBuffer_Depth", depthDesc);
                data.depth = builder.Write(data.depth);

                // The GBuffer pass may also read the shadow atlas if available.
                auto* shadowData = graph.GetBlackboard().TryGet<ShadowPassData>();
                if (shadowData && shadowData->shadowAtlas.IsValid())
                {
                    builder.Read(shadowData->shadowAtlas);
                }
            },
            [executeFn, frameData](const RenderGraphResourceRegistry& registry)
            {
                if (executeFn)
                {
                    executeFn(registry, frameData);
                }
            });
    }

    // ========================================================================
    // Lighting pass
    // ========================================================================

    void StandardPipelineBuilder::AddLightingPass(RenderGraph& graph)
    {
        const auto frameData = m_frameData;
        const auto executeFn = m_callbacks.lightingExecute;
        const auto hdrFormat = m_config.hdrEnabled ? m_config.hdrFormat : RenderTargetFormat::RGBA8_UNORM;

        graph.AddPass(
            "LightingPass", RenderGraphPassType::Graphics,
            [this, &graph, hdrFormat](RenderGraphBuilder& builder)
            {
                auto& data = graph.GetBlackboard().Add<LightingPassData>();

                // Create the HDR colour target.
                auto hdrDesc = MakeColorDesc(hdrFormat);
                data.hdrColor = builder.Create("HDR_Color", hdrDesc);
                data.hdrColor = builder.Write(data.hdrColor);

                // Read GBuffer outputs.
                auto* gbufferData = graph.GetBlackboard().TryGet<GBufferPassData>();
                if (gbufferData)
                {
                    if (gbufferData->albedo.IsValid())
                    {
                        builder.Read(gbufferData->albedo);
                    }
                    if (gbufferData->normals.IsValid())
                    {
                        builder.Read(gbufferData->normals);
                    }
                    if (gbufferData->material.IsValid())
                    {
                        builder.Read(gbufferData->material);
                    }
                    if (gbufferData->depth.IsValid())
                    {
                        builder.Read(gbufferData->depth);
                    }
                }

                // Read shadow atlas if available.
                auto* shadowData = graph.GetBlackboard().TryGet<ShadowPassData>();
                if (shadowData && shadowData->shadowAtlas.IsValid())
                {
                    builder.Read(shadowData->shadowAtlas);
                }
            },
            [executeFn, frameData](const RenderGraphResourceRegistry& registry)
            {
                if (executeFn)
                {
                    executeFn(registry, frameData);
                }
            });
    }

    // ========================================================================
    // Post-process pass
    // ========================================================================

    void StandardPipelineBuilder::AddPostProcessPass(RenderGraph& graph)
    {
        const auto frameData = m_frameData;
        const auto executeFn = m_callbacks.postProcessExecute;

        graph.AddPass(
            "PostProcessPass", RenderGraphPassType::Graphics,
            [this, &graph](RenderGraphBuilder& builder)
            {
                auto& data = graph.GetBlackboard().Add<PostProcessPassData>();

                // LDR output after tonemapping.
                auto ldrDesc = MakeColorDesc(RenderTargetFormat::RGBA8_UNORM);
                data.ldrColor = builder.Create("LDR_Color", ldrDesc);
                data.ldrColor = builder.Write(data.ldrColor);

                // Read HDR colour from lighting pass.
                auto* lightingData = graph.GetBlackboard().TryGet<LightingPassData>();
                if (lightingData && lightingData->hdrColor.IsValid())
                {
                    builder.Read(lightingData->hdrColor);
                }

                // Optional bloom intermediate.
                if (m_config.bloomEnabled)
                {
                    // Bloom at quarter resolution.
                    auto bloomDesc = MakeColorDesc(RenderTargetFormat::R11G11B10_FLOAT);
                    bloomDesc.width = m_config.GetScaledWidth() / 2;
                    bloomDesc.height = m_config.GetScaledHeight() / 2;
                    data.bloom = builder.Create("Bloom", bloomDesc);
                    data.bloom = builder.Write(data.bloom);
                }

                // Optional SSAO.
                if (m_config.ssaoEnabled)
                {
                    auto ssaoDesc = MakeColorDesc(RenderTargetFormat::R8_UNORM);
                    data.ssao = builder.Create("SSAO", ssaoDesc);
                    data.ssao = builder.Write(data.ssao);

                    // SSAO needs depth.
                    auto* gbufferData = graph.GetBlackboard().TryGet<GBufferPassData>();
                    if (gbufferData && gbufferData->depth.IsValid())
                    {
                        builder.Read(gbufferData->depth);
                    }
                }

                // TAA needs motion vectors.
                if (m_config.taaEnabled || m_config.motionBlurEnabled)
                {
                    auto* gbufferData = graph.GetBlackboard().TryGet<GBufferPassData>();
                    if (gbufferData && gbufferData->motion.IsValid())
                    {
                        builder.Read(gbufferData->motion);
                    }
                }

                // Alias bloom with shadow atlas if both lifetimes are disjoint.
                if (m_config.bloomEnabled && m_config.shadowsEnabled)
                {
                    auto* shadowData = graph.GetBlackboard().TryGet<ShadowPassData>();
                    if (shadowData && shadowData->shadowAtlas.IsValid() && data.bloom.IsValid())
                    {
                        builder.Alias(shadowData->shadowAtlas, data.bloom);
                    }
                }
            },
            [executeFn, frameData](const RenderGraphResourceRegistry& registry)
            {
                if (executeFn)
                {
                    executeFn(registry, frameData);
                }
            });
    }

    // ========================================================================
    // UI pass
    // ========================================================================

    void StandardPipelineBuilder::AddUIPass(RenderGraph& graph)
    {
        const auto frameData = m_frameData;
        const auto executeFn = m_callbacks.uiExecute;

        auto& pass = graph.AddPass(
            "UIPass", RenderGraphPassType::Graphics,
            [&graph](RenderGraphBuilder& builder)
            {
                auto& data = graph.GetBlackboard().Add<UIPassData>();

                // The UI pass composites onto a final output target.
                RenderGraphTextureDesc compDesc{};
                compDesc.width = 0;  // Will be set below
                compDesc.height = 0; // Will be set below
                compDesc.format = RenderTargetFormat::RGBA8_UNORM;
                compDesc.usage = RenderTargetUsage::RenderTarget | RenderTargetUsage::ShaderResource;

                // Read the LDR colour from post-processing.
                auto* postData = graph.GetBlackboard().TryGet<PostProcessPassData>();
                if (postData && postData->ldrColor.IsValid())
                {
                    // Reuse the LDR target -- UI composites on top of it.
                    data.composited = builder.Read(postData->ldrColor);
                    data.composited = builder.Write(postData->ldrColor);
                }
                else
                {
                    // Fallback: create a standalone target.
                    compDesc.width = 1920;
                    compDesc.height = 1080;
                    data.composited = builder.Create("UI_Composited", compDesc);
                    data.composited = builder.Write(data.composited);
                }

                // UI presents to the swap chain -- mark as side-effect so it
                // is never culled by dead-code elimination.
                builder.SideEffect();
            },
            [executeFn, frameData](const RenderGraphResourceRegistry& registry)
            {
                if (executeFn)
                {
                    executeFn(registry, frameData);
                }
            });

        // Redundant but explicit: ensure the pass is not culled.
        (void)pass;
    }

    // ========================================================================
    // Debug pass (optional)
    // ========================================================================

    void StandardPipelineBuilder::AddDebugPass(RenderGraph& graph)
    {
        const auto frameData = m_frameData;
        const auto executeFn = m_callbacks.debugExecute;

        graph.AddPass(
            "DebugPass", RenderGraphPassType::Graphics,
            [&graph](RenderGraphBuilder& builder)
            {
                // Read depth for wireframe / debug overlays.
                auto* gbufferData = graph.GetBlackboard().TryGet<GBufferPassData>();
                if (gbufferData && gbufferData->depth.IsValid())
                {
                    builder.Read(gbufferData->depth);
                }

                // Read composited output to overlay debug info.
                auto* uiData = graph.GetBlackboard().TryGet<UIPassData>();
                if (uiData && uiData->composited.IsValid())
                {
                    builder.Read(uiData->composited);
                }

                // Side-effect: writes to backbuffer / debug output.
                builder.SideEffect();
            },
            [executeFn, frameData](const RenderGraphResourceRegistry& registry)
            {
                if (executeFn)
                {
                    executeFn(registry, frameData);
                }
            });
    }

    // ========================================================================
    // Diagnostics
    // ========================================================================

    std::string StandardPipelineBuilder::GetPipelineSummary() const
    {
        std::ostringstream ss;
        ss << "=== StandardPipelineBuilder Config ===\n";
        ss << "  Resolution:   " << m_config.renderWidth << "x" << m_config.renderHeight;
        ss << " (scale=" << m_config.renderScale;
        ss << ", internal=" << m_config.GetScaledWidth() << "x" << m_config.GetScaledHeight() << ")\n";

        ss << "  Passes:       ";
        if (m_config.shadowsEnabled)
        {
            ss << "Shadow ";
        }
        if (m_config.deferredEnabled)
        {
            ss << "GBuffer ";
        }
        ss << "Lighting PostProcess ";
        if (m_config.uiEnabled)
        {
            ss << "UI ";
        }
        if (m_config.debugPassEnabled)
        {
            ss << "Debug ";
        }
        ss << "\n";

        ss << "  Shadows:      " << (m_config.shadowsEnabled ? "ON" : "OFF");
        if (m_config.shadowsEnabled)
        {
            ss << " (" << m_config.shadowMapSize << "x" << m_config.shadowMapSize;
            ss << ", " << m_config.shadowCascades << " cascades)";
        }
        ss << "\n";

        ss << "  HDR:          " << (m_config.hdrEnabled ? "ON" : "OFF") << "\n";
        ss << "  Bloom:        " << (m_config.bloomEnabled ? "ON" : "OFF") << "\n";
        ss << "  SSAO:         " << (m_config.ssaoEnabled ? "ON" : "OFF") << "\n";
        ss << "  TAA:          " << (m_config.taaEnabled ? "ON" : "OFF") << "\n";
        ss << "  Motion blur:  " << (m_config.motionBlurEnabled ? "ON" : "OFF") << "\n";

        return ss.str();
    }

} // namespace Spark::Graphics
