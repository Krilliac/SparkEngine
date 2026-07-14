/**
 * @file RenderPipeline.h
 * @brief Frame graph-based render pipeline (R3.1, R3.3 — Architecture Analysis)
 *
 * Extracted from GraphicsEngine to separate pipeline management from device
 * and scene logic. RenderPipeline owns the RenderGraph and orchestrates
 * the frame's render passes.
 *
 * @threadsafety Main thread only.
 */

#pragma once

#include "../Core/Platform.h"
#include "RenderGraph.h"
#include "RenderDevice.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <memory>
#include <string>
#include <vector>
#include <functional>

// Forward declarations
class GraphicsEngine;

namespace Spark::Graphics
{

    /**
     * @brief Render pass configuration for the render graph.
     */
    struct RenderPassConfig
    {
        std::string name;    ///< Pass name for debugging
        bool enabled = true; ///< Whether this pass executes
        int priority = 0;    ///< Execution priority (lower = earlier)
    };

    /**
     * @brief Frame graph-based render pipeline manager.
     *
     * RenderPipeline integrates the existing RenderGraph system to provide
     * a declarative, DAG-based rendering pipeline. Each render technique
     * (G-buffer, lighting, post-process, UI) is a graph node with automatic
     * resource lifetime management.
     *
     * ### RenderGraph Integration (R3.3)
     * Instead of hardcoded RenderForward()/RenderDeferred() calls, the
     * pipeline constructs a RenderGraph each frame:
     * 1. AddPass() declares resource reads/writes for each technique
     * 2. Compile() performs topological sort and dead-pass elimination
     * 3. Execute() runs passes in dependency order
     *
     * ### Pass Registration
     * @code
     *   pipeline.RegisterPass("GBuffer", Phase::Geometry, [](RenderGraphBuilder& builder) {
     *       auto albedo = builder.CreateTexture("GBuffer_Albedo", ...);
     *       auto normal = builder.CreateTexture("GBuffer_Normal", ...);
     *       builder.SetRenderFunc([=](const RenderGraphResources& res) {
     *           // Render geometry to G-buffers
     *       });
     *   });
     * @endcode
     */
    class RenderPipeline
    {
      public:
        /**
         * @brief Render pipeline phases (execution order).
         */
        enum class PassPhase
        {
            Shadow,      ///< Shadow map generation
            Geometry,    ///< G-buffer fill / forward geometry
            Lighting,    ///< Deferred lighting resolve
            Transparent, ///< Transparent / alpha-blended objects
            PostProcess, ///< HDR tonemapping, bloom, SSAO, etc.
            UI,          ///< ImGui / HUD overlay
            Debug,       ///< Debug visualization

            Count
        };

        RenderPipeline() = default;
        ~RenderPipeline() = default;

        /**
         * @brief Initialize the pipeline with a render device.
         * @param device  Non-owning pointer to the RenderDevice.
         * @return true on success.
         */
        bool Initialize(RenderDevice* device);

        /**
         * @brief Set the graphics engine for pass delegation.
         *
         * Passes delegate rendering work to GraphicsEngine methods
         * (ProcessDrawList, FillGBuffer, LightingPass, etc.) rather
         * than reimplementing rendering logic.
         *
         * @param engine  Non-owning pointer to the GraphicsEngine.
         */
        void SetGraphicsEngine(GraphicsEngine* engine) { m_graphicsEngine = engine; }

        /**
         * @brief Shut down and release all pipeline resources.
         */
        void Shutdown();

        /**
         * @brief Build and execute the frame's render graph.
         *
         * Constructs the graph from registered passes, compiles it (sorts,
         * culls dead passes, allocates transient resources), and executes
         * all passes in dependency order.
         *
         * @param viewMatrix  Camera view matrix.
         * @param projMatrix  Camera projection matrix.
         * @param cameraPos   Camera world-space position.
         */
        void ExecuteFrame(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
                          const DirectX::XMFLOAT3& cameraPos);

        /**
         * @brief Register a custom render pass.
         *
         * @param name     Unique pass name.
         * @param phase    Execution phase determining pass order.
         * @param setupFn  Setup function called during graph construction.
         */
        using PassSetupFn = std::function<void(RenderGraph&)>;
        void RegisterPass(const std::string& name, PassPhase phase, PassSetupFn setupFn);

        /**
         * @brief Remove a registered pass by name.
         */
        void RemovePass(const std::string& name);

        /**
         * @brief Enable or disable a pass by name.
         */
        void SetPassEnabled(const std::string& name, bool enabled);

        /**
         * @brief Get the underlying render graph for diagnostics.
         */
        const RenderGraph* GetRenderGraph() const { return m_renderGraph.get(); }

        /**
         * @brief Get pipeline statistics from the last frame.
         */
        std::string Console_GetPipelineStats() const;

      private:
        struct RegisteredPass
        {
            std::string name;
            PassPhase phase;
            PassSetupFn setupFn;
            bool enabled = true;
        };

        void BuildDefaultPasses();
        void SortPasses();

        RenderDevice* m_device = nullptr;
        GraphicsEngine* m_graphicsEngine = nullptr; ///< Non-owning; for pass delegation
        std::unique_ptr<RenderGraph> m_renderGraph;
        std::vector<RegisteredPass> m_passes;
        bool m_passesDirty = true;

        /// Per-frame camera state, stored by ExecuteFrame for use by pass lambdas.
        DirectX::XMMATRIX m_frameView = DirectX::XMMatrixIdentity();
        DirectX::XMMATRIX m_frameProj = DirectX::XMMatrixIdentity();
        DirectX::XMFLOAT3 m_frameCameraPos = {0.0f, 0.0f, 0.0f};
    };

} // namespace Spark::Graphics
