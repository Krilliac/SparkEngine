#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "RenderPipeline.h"
#include <algorithm>

namespace Spark::Graphics
{

    bool RenderPipeline::Initialize(RenderDevice* device)
    {
        m_device = device;
        m_renderGraph = std::make_unique<RenderGraph>("MainPipeline");
        BuildDefaultPasses();
        return true;
    }

    void RenderPipeline::Shutdown()
    {
        m_passes.clear();
        m_renderGraph.reset();
        m_device = nullptr;
    }

    void RenderPipeline::ExecuteFrame(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                                      const DirectX::XMFLOAT3& cameraPos)
    {
        if (!m_device || !m_renderGraph)
            return;

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

    void RenderPipeline::RegisterPass(const std::string& name, PassPhase phase, PassSetupFn setupFn)
    {
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

    void RenderPipeline::BuildDefaultPasses()
    {
        // Register default passes matching the existing GraphicsEngine pipeline.
        // Each pass is a no-op setup that will be filled in as the RHI migration progresses.

        RegisterPass("ShadowPass", PassPhase::Shadow,
                     [](RenderGraph& graph)
                     {
                         graph.AddPass(
                             "ShadowPass", RenderGraphPassType::Graphics,
                             [](RenderGraphBuilder& builder)
                             {
                                 // Shadow map generation
                             },
                             [](const RenderGraphResourceRegistry& /*resources*/)
                             {
                                 // Execute shadow rendering
                             });
                     });

        RegisterPass("GeometryPass", PassPhase::Geometry,
                     [](RenderGraph& graph)
                     {
                         graph.AddPass(
                             "GeometryPass", RenderGraphPassType::Graphics,
                             [](RenderGraphBuilder& builder)
                             {
                                 // G-buffer fill or forward geometry
                             },
                             [](const RenderGraphResourceRegistry& /*resources*/)
                             {
                                 // Execute geometry rendering
                             });
                     });

        RegisterPass("LightingPass", PassPhase::Lighting,
                     [](RenderGraph& graph)
                     {
                         graph.AddPass(
                             "LightingPass", RenderGraphPassType::Graphics,
                             [](RenderGraphBuilder& builder)
                             {
                                 // Deferred lighting resolve
                             },
                             [](const RenderGraphResourceRegistry& /*resources*/)
                             {
                                 // Execute lighting
                             });
                     });

        RegisterPass("PostProcessPass", PassPhase::PostProcess,
                     [](RenderGraph& graph)
                     {
                         graph.AddPass(
                             "PostProcessPass", RenderGraphPassType::Graphics,
                             [](RenderGraphBuilder& builder)
                             {
                                 // HDR, bloom, SSAO, TAA
                             },
                             [](const RenderGraphResourceRegistry& /*resources*/)
                             {
                                 // Execute post-processing
                             });
                     });
    }

    void RenderPipeline::SortPasses()
    {
        std::stable_sort(m_passes.begin(), m_passes.end(), [](const RegisteredPass& a, const RegisteredPass& b)
                         { return static_cast<int>(a.phase) < static_cast<int>(b.phase); });
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
