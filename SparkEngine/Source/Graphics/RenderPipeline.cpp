#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file RenderPipeline.cpp
 * @brief Render graph-based pipeline that manages ordered render passes
 *
 * Orchestrates the frame rendering pipeline through registered passes sorted
 * by phase (Shadow -> Geometry -> Lighting -> Transparent -> PostProcess -> UI -> Debug).
 * Each pass contributes to a RenderGraph that is compiled (topological sort,
 * dead-pass elimination, resource aliasing) and executed each frame.
 */

#include "RenderPipeline.h"
#include "GraphicsEngine.h"
#include "RenderGraph/RenderGraphBuilder.h"
#include "ShadowAtlas.h"
#include "../Utils/Validate.h"
#include <algorithm>

namespace Spark::Graphics
{

    /// Creates the render graph and registers default passes matching the GraphicsEngine pipeline.
    bool RenderPipeline::Initialize(RenderDevice* device)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, device, false);
        m_device = device;
        m_renderGraph = std::make_unique<RenderGraph>("MainPipeline");
        BuildDefaultPasses();
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "RenderPipeline initialized with %zu default passes",
                       m_passes.size());
        return true;
    }

    void RenderPipeline::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "RenderPipeline shutting down (%zu passes)", m_passes.size());
        m_passes.clear();
        m_renderGraph.reset();
        m_device = nullptr;
    }

    /// Executes a single frame: re-sorts passes if dirty, builds the render graph
    /// from enabled passes, compiles it (dependency sort + dead-pass culling), then executes.
    void RenderPipeline::ExecuteFrame(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                                      const DirectX::XMFLOAT3& cameraPos)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_device);
        if (!m_renderGraph)
            return;

        // Store per-frame camera state for pass lambdas
        m_frameView = viewMatrix;
        m_frameProj = projMatrix;
        m_frameCameraPos = cameraPos;

        // Sort passes if registrations changed
        if (m_passesDirty)
        {
            SortPasses();
            m_passesDirty = false;
        }

        // Build the render graph for this frame
        m_renderGraph->Clear();

        for (const auto& pass : m_passes)
        {
            if (pass.enabled && pass.setupFn)
            {
                pass.setupFn(*m_renderGraph);
            }
        }

        // Compile: topological sort, dead-pass elimination, resource aliasing
        m_renderGraph->Compile();

        // Execute all passes in dependency order
        m_renderGraph->Execute();
    }

    /// Registers a named render pass at a given phase. Replaces any existing pass with the same name.
    /// Marks the pass list dirty so it will be re-sorted before the next frame execution.
    void RenderPipeline::RegisterPass(const std::string& name, PassPhase phase, PassSetupFn setupFn)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Graphics, name);
        SPARK_LOG_DEBUG(Spark::LogCategory::Graphics, "Registering render pass '%s'", name.c_str());
        // Remove existing pass with same name
        RemovePass(name);

        RegisteredPass pass;
        pass.name = name;
        pass.phase = phase;
        pass.setupFn = std::move(setupFn);
        pass.enabled = true;

        m_passes.push_back(std::move(pass));
        m_passesDirty = true;
    }

    void RenderPipeline::RemovePass(const std::string& name)
    {
        auto it = std::remove_if(m_passes.begin(), m_passes.end(),
                                 [&name](const RegisteredPass& p) { return p.name == name; });
        if (it != m_passes.end())
        {
            m_passes.erase(it, m_passes.end());
            m_passesDirty = true;
        }
    }

    void RenderPipeline::SetPassEnabled(const std::string& name, bool enabled)
    {
        for (auto& pass : m_passes)
        {
            if (pass.name == name)
            {
                pass.enabled = enabled;
                return;
            }
        }
    }

    std::string RenderPipeline::Console_GetPipelineStats() const
    {
        std::string result = "Render Pipeline Passes:\n";
        static constexpr const char* phaseNames[] = {"Shadow",      "Geometry", "Lighting", "Transparent",
                                                     "PostProcess", "UI",       "Debug"};

        for (const auto& pass : m_passes)
        {
            size_t phaseIdx = static_cast<size_t>(pass.phase);
            const char* phaseName = (phaseIdx < 7) ? phaseNames[phaseIdx] : "Unknown";
            result += "  [" + std::string(phaseName) + "] " + pass.name +
                      (pass.enabled ? " (enabled)" : " (disabled)") + "\n";
        }

        if (m_renderGraph)
        {
            result += m_renderGraph->Console_GetGraphStats();
        }

        return result;
    }

    /// Registers the standard rendering pipeline passes that delegate to
    /// GraphicsEngine's existing render methods. Each pass declares its
    /// transient resource dependencies via RenderGraphBuilder and calls
    /// the corresponding GraphicsEngine sub-pass in its execute lambda.
    void RenderPipeline::BuildDefaultPasses()
    {
        // ================================================================
        // Shadow pass — generates the shadow atlas / cascade maps
        // ================================================================
        RegisterPass("ShadowPass", PassPhase::Shadow,
                     [this](RenderGraph& graph)
                     {
                         graph.AddPass(
                             "ShadowPass", RenderGraphPassType::Graphics,
                             [&graph](RenderGraphBuilder& builder)
                             {
                                 auto& data = graph.GetBlackboard().Add<ShadowPassData>();

                                 // Create shadow atlas depth target
                                 RenderGraphTextureDesc shadowDesc{};
                                 shadowDesc.width = 2048;
                                 shadowDesc.height = 2048;
                                 shadowDesc.depth = 1;
                                 shadowDesc.arraySize = 3; // 3 cascades
                                 shadowDesc.mipLevels = 1;
                                 shadowDesc.sampleCount = 1;
                                 shadowDesc.format = RenderTargetFormat::D32_FLOAT;
                                 shadowDesc.usage = RenderTargetUsage::DepthStencil | RenderTargetUsage::ShaderResource;
                                 shadowDesc.clearDepth = 1.0f;

                                 data.shadowAtlas = builder.Create("ShadowAtlas", shadowDesc);
                                 data.shadowAtlas = builder.Write(data.shadowAtlas);
                                 data.cascadeCount = 3;

                                 // Side-effect: even without downstream readers, shadows
                                 // are consumed by the GPU lighting shader directly.
                                 builder.SideEffect();
                             },
                             [this](const RenderGraphResourceRegistry& /*resources*/)
                             {
                                 if (!m_graphicsEngine)
                                     return;

                                 // ShadowAtlas tile updates are driven by the lighting system.
                                 // The atlas itself is a persistent resource managed by GraphicsEngine;
                                 // this pass ensures the graph accounts for shadow work ordering.
                                 auto* shadowAtlas = m_graphicsEngine->GetShadowAtlas();
                                 if (shadowAtlas)
                                 {
                                     shadowAtlas->BeginFrame();
                                 }
                             });
                     });

        // ================================================================
        // Geometry pass — fills the G-buffer (deferred) or renders
        // forward geometry + processes the ECS draw list
        // ================================================================
        RegisterPass("GeometryPass", PassPhase::Geometry,
                     [this](RenderGraph& graph)
                     {
                         graph.AddPass(
                             "GeometryPass", RenderGraphPassType::Graphics,
                             [&graph](RenderGraphBuilder& builder)
                             {
                                 auto& data = graph.GetBlackboard().Add<GBufferPassData>();

                                 // Albedo (RGBA8)
                                 RenderGraphTextureDesc colorDesc{};
                                 colorDesc.depth = 1;
                                 colorDesc.arraySize = 1;
                                 colorDesc.mipLevels = 1;
                                 colorDesc.sampleCount = 1;
                                 colorDesc.usage = RenderTargetUsage::RenderTarget | RenderTargetUsage::ShaderResource;
                                 colorDesc.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};

                                 colorDesc.width = 1920;
                                 colorDesc.height = 1080;
                                 colorDesc.format = RenderTargetFormat::RGBA8_UNORM;
                                 data.albedo = builder.Create("GBuffer_Albedo", colorDesc);
                                 data.albedo = builder.Write(data.albedo);

                                 // Normals (RGBA16F)
                                 colorDesc.format = RenderTargetFormat::RGBA16_FLOAT;
                                 data.normals = builder.Create("GBuffer_Normals", colorDesc);
                                 data.normals = builder.Write(data.normals);

                                 // Material properties (RGBA8)
                                 colorDesc.format = RenderTargetFormat::RGBA8_UNORM;
                                 data.material = builder.Create("GBuffer_Material", colorDesc);
                                 data.material = builder.Write(data.material);

                                 // Motion vectors (RG16F)
                                 colorDesc.format = RenderTargetFormat::RG16_FLOAT;
                                 data.motion = builder.Create("GBuffer_Motion", colorDesc);
                                 data.motion = builder.Write(data.motion);

                                 // Depth (D32)
                                 RenderGraphTextureDesc depthDesc{};
                                 depthDesc.width = 1920;
                                 depthDesc.height = 1080;
                                 depthDesc.depth = 1;
                                 depthDesc.arraySize = 1;
                                 depthDesc.mipLevels = 1;
                                 depthDesc.sampleCount = 1;
                                 depthDesc.format = RenderTargetFormat::D32_FLOAT;
                                 depthDesc.usage = RenderTargetUsage::DepthStencil | RenderTargetUsage::ShaderResource;
                                 depthDesc.clearDepth = 1.0f;
                                 data.depth = builder.Create("GBuffer_Depth", depthDesc);
                                 data.depth = builder.Write(data.depth);

                                 // Read shadow atlas if available
                                 auto* shadowData = graph.GetBlackboard().TryGet<ShadowPassData>();
                                 if (shadowData && shadowData->shadowAtlas.IsValid())
                                 {
                                     builder.Read(shadowData->shadowAtlas);
                                 }

                                 builder.SideEffect();
                             },
                             [this](const RenderGraphResourceRegistry& /*resources*/)
                             {
                                 if (!m_graphicsEngine)
                                     return;

                                 // Fill G-buffer using GraphicsEngine's deferred geometry pass.
                                 // FillGBuffer renders all opaque objects into the MRT layout.
                                 std::vector<GameObject*> emptyObjects;
                                 m_graphicsEngine->FillGBuffer(emptyObjects, m_frameView, m_frameProj);

                                 // Process ECS-submitted mesh draws (SubmitMeshForRendering)
                                 m_graphicsEngine->ProcessDrawList(m_frameView, m_frameProj);
                             });
                     });

        // ================================================================
        // Lighting pass — deferred lighting resolve (full-screen quad)
        // ================================================================
        RegisterPass("LightingPass", PassPhase::Lighting,
                     [this](RenderGraph& graph)
                     {
                         graph.AddPass(
                             "LightingPass", RenderGraphPassType::Graphics,
                             [&graph](RenderGraphBuilder& builder)
                             {
                                 auto& data = graph.GetBlackboard().Add<LightingPassData>();

                                 // HDR colour output
                                 RenderGraphTextureDesc hdrDesc{};
                                 hdrDesc.width = 1920;
                                 hdrDesc.height = 1080;
                                 hdrDesc.depth = 1;
                                 hdrDesc.arraySize = 1;
                                 hdrDesc.mipLevels = 1;
                                 hdrDesc.sampleCount = 1;
                                 hdrDesc.format = RenderTargetFormat::RGBA16_FLOAT;
                                 hdrDesc.usage = RenderTargetUsage::RenderTarget | RenderTargetUsage::ShaderResource;
                                 hdrDesc.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
                                 data.hdrColor = builder.Create("HDR_Color", hdrDesc);
                                 data.hdrColor = builder.Write(data.hdrColor);

                                 // Read G-buffer outputs
                                 auto* gbufferData = graph.GetBlackboard().TryGet<GBufferPassData>();
                                 if (gbufferData)
                                 {
                                     if (gbufferData->albedo.IsValid())
                                         builder.Read(gbufferData->albedo);
                                     if (gbufferData->normals.IsValid())
                                         builder.Read(gbufferData->normals);
                                     if (gbufferData->material.IsValid())
                                         builder.Read(gbufferData->material);
                                     if (gbufferData->depth.IsValid())
                                         builder.Read(gbufferData->depth);
                                 }

                                 // Read shadow atlas
                                 auto* shadowData = graph.GetBlackboard().TryGet<ShadowPassData>();
                                 if (shadowData && shadowData->shadowAtlas.IsValid())
                                 {
                                     builder.Read(shadowData->shadowAtlas);
                                 }

                                 builder.SideEffect();
                             },
                             [this](const RenderGraphResourceRegistry& /*resources*/)
                             {
                                 if (!m_graphicsEngine)
                                     return;

                                 // Resolve accumulated lighting against the G-buffer
                                 m_graphicsEngine->LightingPass(m_frameView, m_frameProj);
                             });
                     });

        // ================================================================
        // Post-process pass — HDR tonemapping, bloom, SSAO, SSR
        // ================================================================
        RegisterPass("PostProcessPass", PassPhase::PostProcess,
                     [this](RenderGraph& graph)
                     {
                         graph.AddPass(
                             "PostProcessPass", RenderGraphPassType::Graphics,
                             [&graph](RenderGraphBuilder& builder)
                             {
                                 auto& data = graph.GetBlackboard().Add<PostProcessPassData>();

                                 // LDR output after tonemapping
                                 RenderGraphTextureDesc ldrDesc{};
                                 ldrDesc.width = 1920;
                                 ldrDesc.height = 1080;
                                 ldrDesc.depth = 1;
                                 ldrDesc.arraySize = 1;
                                 ldrDesc.mipLevels = 1;
                                 ldrDesc.sampleCount = 1;
                                 ldrDesc.format = RenderTargetFormat::RGBA8_UNORM;
                                 ldrDesc.usage = RenderTargetUsage::RenderTarget | RenderTargetUsage::ShaderResource;
                                 ldrDesc.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
                                 data.ldrColor = builder.Create("LDR_Color", ldrDesc);
                                 data.ldrColor = builder.Write(data.ldrColor);

                                 // Read HDR colour from lighting pass
                                 auto* lightingData = graph.GetBlackboard().TryGet<LightingPassData>();
                                 if (lightingData && lightingData->hdrColor.IsValid())
                                 {
                                     builder.Read(lightingData->hdrColor);
                                 }

                                 // Read depth and motion vectors for SSAO / TAA / motion blur
                                 auto* gbufferData = graph.GetBlackboard().TryGet<GBufferPassData>();
                                 if (gbufferData)
                                 {
                                     if (gbufferData->depth.IsValid())
                                         builder.Read(gbufferData->depth);
                                     if (gbufferData->motion.IsValid())
                                         builder.Read(gbufferData->motion);
                                 }

                                 builder.SideEffect();
                             },
                             [this](const RenderGraphResourceRegistry& /*resources*/)
                             {
                                 if (!m_graphicsEngine)
                                     return;

                                 // Delegate to GraphicsEngine's post-processing pipeline
                                 m_graphicsEngine->RenderPostProcessing();
                             });
                     });

        // ================================================================
        // UI pass — ImGui / HUD overlay, presents to swap chain
        // ================================================================
        RegisterPass("UIPass", PassPhase::UI,
                     [this](RenderGraph& graph)
                     {
                         graph.AddPass(
                             "UIPass", RenderGraphPassType::Graphics,
                             [&graph](RenderGraphBuilder& builder)
                             {
                                 auto& data = graph.GetBlackboard().Add<UIPassData>();

                                 // Read LDR output from post-processing and composite UI on top
                                 auto* postData = graph.GetBlackboard().TryGet<PostProcessPassData>();
                                 if (postData && postData->ldrColor.IsValid())
                                 {
                                     data.composited = builder.Read(postData->ldrColor);
                                     data.composited = builder.Write(postData->ldrColor);
                                 }

                                 // UI writes to the swap chain — must not be culled
                                 builder.SideEffect();
                             },
                             [this](const RenderGraphResourceRegistry& /*resources*/)
                             {
                                 // UI rendering (ImGui) is handled by the editor/game layer
                                 // after EndFrame(). This pass ensures correct ordering in the
                                 // graph and that the LDR target is available for compositing.
                             });
                     });
    }

    /// Stable-sorts passes by phase enum value, preserving registration order within the same phase.
    void RenderPipeline::SortPasses()
    {
        std::stable_sort(m_passes.begin(), m_passes.end(), [](const RegisteredPass& a, const RegisteredPass& b)
                         { return static_cast<int>(a.phase) < static_cast<int>(b.phase); });
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
