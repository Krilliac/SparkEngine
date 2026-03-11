/**
 * @file RenderGraphBuilder.h
 * @brief High-level helper that constructs a RenderGraph from the engine's
 *        standard rendering passes (R3.3 -- Activate RenderGraph pipeline)
 * @author Spark Engine Team
 * @date 2026
 *
 * StandardPipelineBuilder populates a RenderGraph with the canonical pass
 * sequence used by SparkEngine's deferred renderer:
 *
 *   Shadow --> GBuffer --> Lighting --> PostProcess --> UI
 *
 * Each pass declares its transient resource dependencies through the
 * RenderGraphBuilder DSL (defined in RenderGraph.h).  The resulting graph is
 * compiled once (topological sort, dead-code elimination, aliasing) and then
 * executed each frame.
 *
 * Usage:
 * @code
 *   StandardPipelineBuilder pipelineBuilder;
 *   pipelineBuilder.Configure(config);
 *
 *   // Each frame:
 *   RenderGraph graph("MainFrame", d3dDevice);
 *   pipelineBuilder.Build(graph);
 *   graph.Compile();
 *   graph.Execute();
 * @endcode
 *
 * @see RenderGraph, RenderPipeline, RHIAdapter
 */

#pragma once

#include "../RenderGraph.h"
#include "../RenderTarget.h"
#include "../RHI/RHIAdapter.h"
#include "../../Core/Platform.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
class GraphicsEngine;

namespace Spark::Graphics
{

    // ========================================================================
    // Pipeline configuration
    // ========================================================================

    /**
     * @brief Per-frame camera / scene data injected into the pipeline builder.
     */
    struct PipelineFrameData
    {
        XMMATRIX viewMatrix = XMMatrixIdentity();
        XMMATRIX projMatrix = XMMatrixIdentity();
        XMFLOAT3 cameraPosition = {0.0f, 0.0f, 0.0f};
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        float deltaTime = 0.0f;
    };

    /**
     * @brief Configuration that controls which passes are enabled and at what
     *        resolution they operate.
     */
    struct PipelineConfig
    {
        // -- Resolution -------------------------------------------------------
        uint32_t renderWidth = 1920;
        uint32_t renderHeight = 1080;
        float renderScale = 1.0f; ///< Internal resolution multiplier

        // -- Shadow pass ------------------------------------------------------
        bool shadowsEnabled = true;
        uint32_t shadowMapSize = 2048;
        uint32_t shadowCascades = 3;

        // -- GBuffer pass -----------------------------------------------------
        bool deferredEnabled = true;
        uint32_t gBufferCount = 4; ///< Albedo, Normal, Material, Motion

        // -- Lighting pass ----------------------------------------------------
        bool hdrEnabled = true;
        RenderTargetFormat hdrFormat = RenderTargetFormat::RGBA16_FLOAT;

        // -- Post-process pass ------------------------------------------------
        bool bloomEnabled = true;
        bool ssaoEnabled = false;
        bool taaEnabled = false;
        bool motionBlurEnabled = false;

        // -- UI pass ----------------------------------------------------------
        bool uiEnabled = true;

        // -- Debug pass -------------------------------------------------------
        bool debugPassEnabled = false;

        /**
         * @brief Compute effective internal resolution with render scale.
         */
        uint32_t GetScaledWidth() const { return static_cast<uint32_t>(static_cast<float>(renderWidth) * renderScale); }

        uint32_t GetScaledHeight() const
        {
            return static_cast<uint32_t>(static_cast<float>(renderHeight) * renderScale);
        }
    };

    // ========================================================================
    // Blackboard data structures shared between passes
    // ========================================================================

    /**
     * @brief Shadow pass outputs placed on the blackboard.
     */
    struct ShadowPassData
    {
        RenderGraphResource shadowAtlas;
        uint32_t cascadeCount = 0;
    };

    /**
     * @brief GBuffer pass outputs placed on the blackboard.
     */
    struct GBufferPassData
    {
        RenderGraphResource albedo;
        RenderGraphResource normals;
        RenderGraphResource material; ///< Roughness / metalness / AO
        RenderGraphResource motion;   ///< Screen-space motion vectors
        RenderGraphResource depth;
    };

    /**
     * @brief Lighting pass outputs placed on the blackboard.
     */
    struct LightingPassData
    {
        RenderGraphResource hdrColor;
    };

    /**
     * @brief Post-process pass outputs placed on the blackboard.
     */
    struct PostProcessPassData
    {
        RenderGraphResource ldrColor;
        RenderGraphResource bloom;
        RenderGraphResource ssao;
    };

    /**
     * @brief UI pass outputs placed on the blackboard.
     */
    struct UIPassData
    {
        RenderGraphResource composited; ///< Final composited output
    };

    // ========================================================================
    // Pass execute callbacks
    // ========================================================================

    /**
     * @brief User-supplied rendering callbacks for each standard pass.
     *
     * The builder wires up resource dependencies automatically; the callbacks
     * perform the actual GPU work using the resolved resources.
     */
    struct PipelineCallbacks
    {
        using ExecuteFn = std::function<void(const RenderGraphResourceRegistry&, const PipelineFrameData&)>;

        ExecuteFn shadowExecute;
        ExecuteFn gBufferExecute;
        ExecuteFn lightingExecute;
        ExecuteFn postProcessExecute;
        ExecuteFn uiExecute;
        ExecuteFn debugExecute;
    };

    // ========================================================================
    // StandardPipelineBuilder
    // ========================================================================

    /**
     * @brief Builds the engine's standard deferred rendering pipeline as a
     *        RenderGraph with proper resource dependencies between passes.
     *
     * The builder is stateless between frames: call Build() each frame with a
     * fresh (or cleared) RenderGraph.  The graph owns all transient resources.
     *
     * ### Pass dependency chain
     *
     * ```
     *   ShadowPass
     *       |
     *       v
     *   GBufferPass --+
     *       |         |
     *       v         v
     *   LightingPass (reads GBuffer + Shadow)
     *       |
     *       v
     *   PostProcessPass (reads HDR color, motion, depth)
     *       |
     *       v
     *   UIPass (reads LDR color, writes composited output) [side-effect]
     *       |
     *       v
     *   DebugPass (optional, reads depth) [side-effect]
     * ```
     */
    class StandardPipelineBuilder
    {
      public:
        StandardPipelineBuilder() = default;
        ~StandardPipelineBuilder() = default;

        // Non-copyable, movable
        StandardPipelineBuilder(const StandardPipelineBuilder&) = delete;
        StandardPipelineBuilder& operator=(const StandardPipelineBuilder&) = delete;
        StandardPipelineBuilder(StandardPipelineBuilder&&) noexcept = default;
        StandardPipelineBuilder& operator=(StandardPipelineBuilder&&) noexcept = default;

        // -- Configuration ----------------------------------------------------

        /**
         * @brief Set the pipeline configuration.
         *
         * Call before Build(). Can be changed between frames (e.g. when
         * the player changes quality settings).
         */
        void Configure(const PipelineConfig& config);

        /**
         * @brief Get the current configuration (read-only).
         */
        const PipelineConfig& GetConfig() const { return m_config; }

        /**
         * @brief Set the per-frame camera/scene data.
         */
        void SetFrameData(const PipelineFrameData& frameData);

        /**
         * @brief Get the current frame data.
         */
        const PipelineFrameData& GetFrameData() const { return m_frameData; }

        /**
         * @brief Register callbacks that perform the actual GPU work.
         */
        void SetCallbacks(const PipelineCallbacks& callbacks);

        // -- Graph construction -----------------------------------------------

        /**
         * @brief Populate the given RenderGraph with the standard pass set.
         *
         * Adds Shadow, GBuffer, Lighting, PostProcess, and UI passes (plus
         * optional Debug) with all resource dependencies wired up.  The graph
         * must be empty or freshly Clear()'d.
         *
         * After this call the graph is ready for Compile() + Execute().
         *
         * @param graph  The render graph to populate.
         */
        void Build(RenderGraph& graph);

        // -- Diagnostic -------------------------------------------------------

        /**
         * @brief Get a summary of the last Build() for debug display.
         */
        std::string GetPipelineSummary() const;

      private:
        // -- Individual pass builders -----------------------------------------

        void AddShadowPass(RenderGraph& graph);
        void AddGBufferPass(RenderGraph& graph);
        void AddLightingPass(RenderGraph& graph);
        void AddPostProcessPass(RenderGraph& graph);
        void AddUIPass(RenderGraph& graph);
        void AddDebugPass(RenderGraph& graph);

        // -- Helpers ----------------------------------------------------------

        /**
         * @brief Create a RenderGraphTextureDesc for a full-resolution colour target.
         */
        RenderGraphTextureDesc MakeColorDesc(RenderTargetFormat format) const;

        /**
         * @brief Create a RenderGraphTextureDesc for the depth buffer.
         */
        RenderGraphTextureDesc MakeDepthDesc() const;

        /**
         * @brief Create a RenderGraphTextureDesc for a shadow map.
         */
        RenderGraphTextureDesc MakeShadowDesc() const;

        // -- State ------------------------------------------------------------

        PipelineConfig m_config{};
        PipelineFrameData m_frameData{};
        PipelineCallbacks m_callbacks{};
    };

} // namespace Spark::Graphics
