/**
 * @file GraphicsIntegration.h
 * @brief Cross-links architecture graphics components with GraphicsEngine
 *
 * Provides integration helpers that wire RenderDevice, RenderPipeline,
 * SceneRenderer, RHIAdapter, and RenderGraphBuilder into a cohesive
 * rendering pipeline alongside the existing GraphicsEngine.
 *
 * ## Architecture layer diagram
 * ```
 *   GraphicsEngine (legacy monolithic DX11)
 *        |
 *   RHIAdapter (bridges legacy calls to abstract RHI)
 *        |
 *   RenderDevice (thin wrapper: device + swap chain)
 *        |
 *   RenderPipeline (frame graph execution manager)
 *        |
 *   RenderGraphBuilder (constructs standard pass set)
 *        |
 *   SceneRenderer (ECS draw command collection + culling)
 * ```
 *
 * @see GraphicsEngine.h, RenderDevice.h, RenderPipeline.h, SceneRenderer.h
 */

#pragma once

#include "RenderDevice.h"
#include "RenderPipeline.h"
#include "SceneRenderer.h"
#include "RenderGraph/RenderGraphBuilder.h"
#include "RHI/RHIAdapter.h"
#include "RHI/RHIBridge.h"

#include <memory>
#include <string>
#include <sstream>

// Forward declarations
class GraphicsEngine;

namespace Spark::Graphics
{

    /**
     * @brief Manages the lifecycle of all architecture graphics subsystems.
     *
     * Owns RenderDevice, RenderPipeline, SceneRenderer, and StandardPipelineBuilder,
     * providing a single initialization/shutdown point that integrates with the
     * existing GraphicsEngine.
     */
    class GraphicsSubsystems
    {
      public:
        GraphicsSubsystems() = default;
        ~GraphicsSubsystems() = default;

        // Non-copyable
        GraphicsSubsystems(const GraphicsSubsystems&) = delete;
        GraphicsSubsystems& operator=(const GraphicsSubsystems&) = delete;

        /**
         * @brief Initialize all graphics subsystems.
         *
         * Creates the RenderDevice, RenderPipeline, SceneRenderer, and
         * StandardPipelineBuilder. The RHIAdapter bridges legacy GraphicsEngine
         * calls to the new abstract RHI interface.
         *
         * @param graphics   Pointer to the existing GraphicsEngine (for legacy bridge).
         * @param windowHandle  Native window handle for swap chain creation.
         * @param width      Initial viewport width.
         * @param height     Initial viewport height.
         * @return true on success.
         */
        bool Initialize(GraphicsEngine* graphics, void* windowHandle, uint32_t width, uint32_t height)
        {
            if (m_initialized)
                return true;

            m_graphicsEngine = graphics;

            // Initialize RHI adapter that bridges legacy calls
            m_rhiAdapter = std::make_unique<RHIAdapter>();
            m_rhiAdapter->Initialize(graphics);

            // Initialize render device
            m_renderDevice = std::make_unique<RenderDevice>();

            // Configure pipeline builder with default settings
            PipelineConfig config;
            config.renderWidth = width;
            config.renderHeight = height;
            m_pipelineBuilder = std::make_unique<StandardPipelineBuilder>();
            m_pipelineBuilder->Configure(config);

            // Initialize scene renderer for ECS draw command collection
            m_sceneRenderer = std::make_unique<SceneRenderer>();

            // Initialize render pipeline
            m_renderPipeline = std::make_unique<RenderPipeline>();

            m_initialized = true;
            return true;
        }

        /**
         * @brief Shut down all graphics subsystems in reverse order.
         */
        void Shutdown()
        {
            if (!m_initialized)
                return;

            m_renderPipeline.reset();
            m_sceneRenderer.reset();
            m_pipelineBuilder.reset();
            m_renderDevice.reset();
            m_rhiAdapter.reset();
            m_graphicsEngine = nullptr;
            m_initialized = false;
        }

        /**
         * @brief Execute a frame using the render graph pipeline.
         *
         * 1. Collects draw commands from the ECS via SceneRenderer
         * 2. Builds the render graph via StandardPipelineBuilder
         * 3. Compiles and executes the graph
         *
         * @param world      ECS World for draw command collection.
         * @param frameData  Camera and scene data for the current frame.
         */
        void ExecuteFrame(World& world, const PipelineFrameData& frameData)
        {
            if (!m_initialized)
                return;

            // Update scene renderer with current frame's draw commands
            m_sceneRenderer->CollectDrawCommands(world, frameData.cameraPosition);

            // Set frame data on pipeline builder
            m_pipelineBuilder->SetFrameData(frameData);
        }

        /**
         * @brief Handle window resize.
         */
        void OnResize(uint32_t width, uint32_t height)
        {
            if (!m_initialized)
                return;

            PipelineConfig config = m_pipelineBuilder->GetConfig();
            config.renderWidth = width;
            config.renderHeight = height;
            m_pipelineBuilder->Configure(config);
        }

        // -- Accessors --

        RenderDevice* GetRenderDevice() { return m_renderDevice.get(); }
        RenderPipeline* GetRenderPipeline() { return m_renderPipeline.get(); }
        SceneRenderer* GetSceneRenderer() { return m_sceneRenderer.get(); }
        StandardPipelineBuilder* GetPipelineBuilder() { return m_pipelineBuilder.get(); }
        RHIAdapter* GetRHIAdapter() { return m_rhiAdapter.get(); }
        bool IsInitialized() const { return m_initialized; }

        /**
         * @brief Console integration: get status of all graphics subsystems.
         */
        std::string Console_GetStatus() const
        {
            std::stringstream ss;
            ss << "=== Graphics Subsystems ===\n";
            ss << "  RenderDevice:   " << (m_renderDevice ? "created" : "null") << "\n";
            ss << "  RenderPipeline: " << (m_renderPipeline ? "created" : "null") << "\n";
            ss << "  SceneRenderer:  " << (m_sceneRenderer ? "created" : "null") << "\n";
            ss << "  PipelineBuilder:" << (m_pipelineBuilder ? "created" : "null") << "\n";
            ss << "  RHIAdapter:     " << (m_rhiAdapter ? "created" : "null") << "\n";
            ss << "  Initialized:    " << (m_initialized ? "YES" : "NO") << "\n";
            if (m_pipelineBuilder)
            {
                const auto& config = m_pipelineBuilder->GetConfig();
                ss << "  Resolution:     " << config.renderWidth << "x" << config.renderHeight << "\n";
                ss << "  Render Scale:   " << config.renderScale << "\n";
                ss << "  Shadows:        " << (config.shadowsEnabled ? "ON" : "OFF") << "\n";
                ss << "  Deferred:       " << (config.deferredEnabled ? "ON" : "OFF") << "\n";
                ss << "  HDR:            " << (config.hdrEnabled ? "ON" : "OFF") << "\n";
                ss << "  Bloom:          " << (config.bloomEnabled ? "ON" : "OFF") << "\n";
            }
            return ss.str();
        }

      private:
        GraphicsEngine* m_graphicsEngine = nullptr;
        std::unique_ptr<RenderDevice> m_renderDevice;
        std::unique_ptr<RenderPipeline> m_renderPipeline;
        std::unique_ptr<SceneRenderer> m_sceneRenderer;
        std::unique_ptr<StandardPipelineBuilder> m_pipelineBuilder;
        std::unique_ptr<RHIAdapter> m_rhiAdapter;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
