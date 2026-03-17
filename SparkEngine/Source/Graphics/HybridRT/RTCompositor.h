/**
 * @file RTCompositor.h
 * @brief Composites screen-space and ray-traced results with temporal accumulation
 * @author Spark Engine Team
 * @date 2026
 *
 * Blends existing SSR/SSAO results with SDFGI/DXR outputs. Uses roughness-based
 * weighting (smooth surfaces prefer RT, rough surfaces use probes) and temporal
 * accumulation with motion rejection for stability.
 *
 * Uses SSRSettings from ScreenSpaceEffects.h to coordinate blend weights when
 * both screen-space and ray-traced reflections are active.
 */

#pragma once

#include "HybridRTTypes.h"
#include "../RHI/RHITypes.h"
#include "../RHI/RHIResources.h"
#include "../ScreenSpaceEffects.h" // SSRSettings for SS/RT blend coordination

#include <cstdint>

namespace Spark::RHI
{
    class IRHIDevice;
    class IRHICommandList;
    class IRHIBuffer;
    class IRHITexture;
    class IRHIShader;
} // namespace Spark::RHI

namespace Spark::Graphics
{

    /// @brief Constant buffer layout for RTComposite.hlsl (matches HLSL cbuffer)
    struct alignas(16) CompositeConstants
    {
        float screenWidth = 1920.0f;
        float screenHeight = 1080.0f;
        float temporalBlend = 0.9f;
        float reflectionStrength = 1.0f;
        float giStrength = 1.0f;
        float shadowStrength = 1.0f;
        float ssrFallbackWeight = 0.5f;
        float frameIndex = 0.0f;
    };

    class RTCompositor
    {
      public:
        RTCompositor() = default;
        ~RTCompositor();

        bool Initialize(RHI::IRHIDevice* device, uint32_t width, uint32_t height);
        void Shutdown();
        void Resize(uint32_t width, uint32_t height);

        /// @brief Composite all RT + SS results into the final lighting buffer
        /// @param ssrSettings Current SSR config — used to set fallback weight
        void Composite(RHI::IRHICommandList* cmd, RHI::IRHITexture* rtReflections, RHI::IRHITexture* rtGI,
                       RHI::IRHITexture* rtShadows, RHI::IRHITexture* ssReflections, RHI::IRHITexture* ssao,
                       RHI::IRHITexture* lightingBuffer, RHI::IRHITexture* gbufferNormals,
                       RHI::IRHITexture* gbufferAlbedo, RHI::IRHITexture* output, const SSRSettings& ssrSettings,
                       float frameIndex);

        void SetStrengths(float reflections, float gi, float shadows);
        void SetTemporalBlend(float blend);

      private:
        RHI::IRHIDevice* m_device = nullptr;
        RHI::IRHIShader* m_compositeCS = nullptr;
        RHI::IRHIBuffer* m_constantBuffer = nullptr;
        RHI::IRHITexture* m_historyBuffer = nullptr; ///< Previous frame for temporal accumulation

        CompositeConstants m_constants;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
