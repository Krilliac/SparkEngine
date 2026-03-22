/**
 * @file SDFSceneManager.h
 * @brief SDF scene representation and compute-based sphere tracing for software RT
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages the GPU-side SDF primitive buffer and dispatches sphere tracing
 * compute shaders for reflections, GI, and shadows. This is the primary
 * software fallback for the hybrid ray tracing system — works on DX11 CS 5.0+.
 *
 * The scene is represented as a structured buffer of SDF primitives (spheres,
 * boxes, capsules) approximating mesh bounding volumes. The SDFTrace.hlsl
 * compute shader sphere-traces through this representation.
 */

#pragma once

#include "HybridRTTypes.h"
#include "../RHI/RHITypes.h"
#include "../RHI/RHIResources.h"
#include "../ScreenSpaceEffects.h" // Reuse SSAOKernel noise generation for trace jitter

#include <vector>
#include <memory>

namespace Spark::RHI
{
    class IRHIDevice;
    class IRHICommandList;
    class IRHIBuffer;
    class IRHITexture;
    class IRHIShader;
    class RHIBridge;
} // namespace Spark::RHI

namespace Spark::Graphics
{

    class SDFSceneManager
    {
      public:
        static constexpr uint32_t kMaxPrimitives = 2048;

        SDFSceneManager() = default;
        ~SDFSceneManager();

        /// @brief Initialize GPU resources for SDF tracing
        /// @param device RHI device for resource creation
        /// @param width Render target width (trace output resolution)
        /// @param height Render target height
        /// @return true on success
        bool Initialize(RHI::IRHIDevice* device, uint32_t width, uint32_t height);

        void Shutdown();

        /// @brief Resize trace output textures
        void Resize(uint32_t width, uint32_t height);

        /// @brief Upload SDF primitives to the GPU structured buffer
        /// @param primitives Scene SDF primitives (max 2048)
        void UpdateScene(const std::vector<SDFPrimitive>& primitives);

        /// @brief Dispatch SDFGI sphere trace for reflections
        void TraceReflections(RHI::IRHICommandList* cmd, const SDFTraceConstants& constants,
                              RHI::IRHITexture* gbufferNormals, RHI::IRHITexture* gbufferDepth,
                              RHI::IRHITexture* output);

        /// @brief Dispatch SDFGI sphere trace for global illumination
        void TraceGI(RHI::IRHICommandList* cmd, const SDFTraceConstants& constants, RHI::IRHITexture* gbufferNormals,
                     RHI::IRHITexture* gbufferDepth, RHI::IRHITexture* gbufferAlbedo, RHI::IRHITexture* output);

        /// @brief Dispatch SDFGI sphere trace for soft shadows
        void TraceShadows(RHI::IRHICommandList* cmd, const SDFTraceConstants& constants,
                          RHI::IRHITexture* gbufferNormals, RHI::IRHITexture* gbufferDepth, RHI::IRHITexture* output);

        uint32_t GetPrimitiveCount() const { return m_primitiveCount; }
        uint32_t GetMaxPrimitives() const { return kMaxPrimitives; }

      private:
        void DispatchTrace(RHI::IRHICommandList* cmd, RHI::IRHIShader* shader, const SDFTraceConstants& constants,
                           RHI::IRHITexture** srvs, uint32_t srvCount, RHI::IRHITexture* output);

        RHI::IRHIDevice* m_device = nullptr;
        std::unique_ptr<RHI::IRHIBuffer> m_primitiveBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_traceConstantBuffer;
        std::unique_ptr<RHI::IRHIShader> m_traceReflectionsCS;
        std::unique_ptr<RHI::IRHIShader> m_traceGICS;
        std::unique_ptr<RHI::IRHIShader> m_traceShadowsCS;

        /// Noise data from ScreenSpaceEffects SSAOKernel for trace jitter
        std::vector<Spark::Graphics::SamplePoint> m_noiseData;

        uint32_t m_primitiveCount = 0;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
