/**
 * @file HybridRTManager.h
 * @brief Hybrid ray tracing coordinator — Lumen-style SS → Software RT → Hardware RT
 * @author Spark Engine Team
 * @date 2026
 *
 * Central manager for the hybrid ray tracing system. Detects hardware capabilities
 * at initialization, selects the best backend (DXR 1.1, Vulkan RT, or SDFGI
 * software fallback), and orchestrates per-frame RT dispatch:
 *
 *   1. Receives GBuffer textures after the lighting pass
 *   2. Dispatches SDFGI sphere tracing OR DXR ray tracing for reflections/GI/shadows
 *   3. Composites RT results with screen-space effects (SSR/SSAO) via RTCompositor
 *   4. Outputs to the lighting buffer for post-processing
 *
 * ## Backend Selection (March 2026 spec)
 * - DX12 path: D3D12_FEATURE_DATA_D3D12_OPTIONS5 RaytracingTier >= D3D12_RAYTRACING_TIER_1_0
 * - Vulkan path: VK_KHR_ray_tracing_pipeline + VK_KHR_acceleration_structure + VK_KHR_ray_query
 * - DX11 fallback: always Software_SDFGI (CS 5.0 compute-based sphere tracing)
 * - No compute support: Disabled (screen-space only)
 *
 * ## VRS Integration (2026 hardware)
 * When Variable Rate Shading is available, the RT trace pass uses coarser shading
 * rates in areas of low visual importance (distant surfaces, high roughness).
 */

#pragma once

#include "HybridRTTypes.h"
#include "SDFSceneManager.h"
#include "RTCompositor.h"
#include "ProbeSystem.h"
#include "../RHI/RHITypes.h"
#include "../RHI/RHIResources.h"
#include "../ScreenSpaceEffects.h" // SSRSettings for compositor coordination

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <memory>
#include <string>
#include <vector>

namespace Spark::RHI
{
    class IRHIDevice;
    class IRHICommandList;
    class IRHIBuffer;
    class IRHITexture;
} // namespace Spark::RHI

#ifdef SPARK_PLATFORM_MACOS
namespace Spark::RHI::Metal
{
    class MetalRayTracingSystem;
}
#endif

namespace Spark::Graphics
{
    // Forward declare DXRManager for hardware path bridging
    class DXRManager;

    class HybridRTManager
    {
      public:
        HybridRTManager() = default;
        ~HybridRTManager();

        /// @brief Initialize the hybrid RT system
        /// @param device RHI device (queries capabilities for backend selection)
        /// @param width Render target width
        /// @param height Render target height
        /// @return true if at least software fallback is available
        bool Initialize(RHI::IRHIDevice* device, uint32_t width, uint32_t height);

        void Shutdown();

        /// @brief Resize intermediate RT textures
        void Resize(uint32_t width, uint32_t height);

        /// @brief Main per-frame entry point — call after LightingPass, before post-process
        /// @param ssrSettings Current SSR config for compositor blend weights
        void Execute(RHI::IRHICommandList* cmd, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj,
                     const DirectX::XMFLOAT3& cameraPos, const DirectX::XMFLOAT3& lightDir,
                     RHI::IRHITexture* gbufferNormals, RHI::IRHITexture* gbufferDepth, RHI::IRHITexture* gbufferAlbedo,
                     RHI::IRHITexture* ssReflections, RHI::IRHITexture* ssao, RHI::IRHITexture* lightingOutput,
                     const SSRSettings& ssrSettings);

        /// @brief Submit mesh SDF data for the current frame
        void SubmitSDFPrimitive(const SDFPrimitive& primitive);
        void ClearScene();
        void FlushScene();

        // ---- Settings ----
        void SetQuality(RHI::RayTracingQuality quality);
        void SetBackendOverride(RHI::RayTracingBackend backend);
        void SetEnabledEffects(RHI::RTEffect effects);

        RHI::RayTracingBackend GetActiveBackend() const { return m_activeBackend; }
        RHI::RayTracingQuality GetQuality() const { return m_quality; }
        RHI::RTEffect GetEnabledEffects() const { return m_enabledEffects; }

        // ---- Console ----
        std::string Console_GetStatus() const;

      private:
        RHI::RayTracingBackend DetectBestBackend() const;
        void ExecuteSDFGI(RHI::IRHICommandList* cmd, const RTFrameParams& params, RHI::IRHITexture* gbufferNormals,
                          RHI::IRHITexture* gbufferDepth, RHI::IRHITexture* gbufferAlbedo);

        RHI::IRHIDevice* m_device = nullptr;
        RHI::RayTracingBackend m_activeBackend = RHI::RayTracingBackend::Disabled;
        RHI::RayTracingBackend m_overrideBackend = RHI::RayTracingBackend::Disabled;
        RHI::RayTracingQuality m_quality = RHI::RayTracingQuality::Medium;
        RHI::RTEffect m_enabledEffects = RHI::RTEffect::All;

        std::unique_ptr<SDFSceneManager> m_sdfScene;
        std::unique_ptr<RTCompositor> m_compositor;
        std::unique_ptr<ProbeSystem> m_probes;

#ifdef SPARK_PLATFORM_MACOS
        // Metal hardware ray-tracing system. Instantiated only when the
        // detected backend is `HardwareMetalRT`. Currently a scaffold — its
        // DispatchFrame returns zero executed passes, so SDFGI runs as
        // fallback on every frame until the real trace pipelines land.
        std::unique_ptr<Spark::RHI::Metal::MetalRayTracingSystem> m_metalRT;
#endif

        // Intermediate RT output textures (owned by this manager)
        std::unique_ptr<RHI::IRHITexture> m_rtReflections;
        std::unique_ptr<RHI::IRHITexture> m_rtGI;
        std::unique_ptr<RHI::IRHITexture> m_rtShadows;

        // Scene accumulation for current frame
        std::vector<SDFPrimitive> m_pendingPrimitives;

        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_frameIndex = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
