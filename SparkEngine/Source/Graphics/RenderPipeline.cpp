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

    /// Registers placeholder passes for the standard rendering pipeline stages.
    /// These no-op passes define the phase ordering and will be replaced with real
    /// implementations as the RHI migration progresses.
    void RenderPipeline::BuildDefaultPasses()
    {

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

    /// Stable-sorts passes by phase enum value, preserving registration order within the same phase.
    void RenderPipeline::SortPasses()
    {
        std::stable_sort(m_passes.begin(), m_passes.end(), [](const RegisteredPass& a, const RegisteredPass& b)
                         { return static_cast<int>(a.phase) < static_cast<int>(b.phase); });
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
