/**
 * @file ProbeSystem.h
 * @brief Irradiance probe grid for cached global illumination
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages a 3D grid of irradiance probes that cache indirect lighting via
 * spherical harmonics (L0 + L1 = 4 SH coefficients per RGB channel).
 * Probes are updated incrementally via SDFGI sphere tracing and provide
 * temporally stable GI that fills in where per-pixel tracing misses.
 *
 * Uses SSAOKernel::GenerateKernel() from ScreenSpaceEffects for hemisphere
 * sampling directions during probe update rays.
 */

#pragma once

#include "HybridRTTypes.h"
#include "../RHI/RHITypes.h"
#include "../RHI/RHIResources.h"
#include "../ScreenSpaceEffects.h" // SamplePoint for hemisphere directions

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <memory>
#include <vector>

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

    /// @brief GPU probe data layout (matches HLSL ProbeData struct)
    struct alignas(16) ProbeData
    {
        DirectX::XMFLOAT3 position = {0, 0, 0};
        float padding = 0;
        DirectX::XMFLOAT4 shCoeffs[3] = {}; ///< RGB x 4 SH coefficients
    };

    /// @brief Probe grid configuration
    struct ProbeGridConfig
    {
        DirectX::XMFLOAT3 origin = {0, 0, 0}; ///< World-space grid origin
        float spacing = 2.0f;                 ///< Distance between probes (meters)
        int32_t dimensionsX = 16;             ///< Probes in X
        int32_t dimensionsY = 8;              ///< Probes in Y
        int32_t dimensionsZ = 16;             ///< Probes in Z
        int32_t raysPerProbe = 32;            ///< Rays traced per probe update
        float hysteresis = 0.97f;             ///< Temporal smoothing factor
    };

    class ProbeSystem
    {
      public:
        ProbeSystem() = default;
        ~ProbeSystem();

        bool Initialize(RHI::IRHIDevice* device, const ProbeGridConfig& config);
        void Shutdown();

        /// @brief Update probe grid origin to follow camera
        void UpdateGridOrigin(const DirectX::XMFLOAT3& cameraPos);

        /// @brief Dispatch probe update compute shader (traces rays from probes)
        void UpdateProbes(RHI::IRHICommandList* cmd, const DirectX::XMFLOAT3& lightDir, float lightIntensity);

        /// @brief Dispatch probe interpolation for per-pixel GI
        void InterpolateProbes(RHI::IRHICommandList* cmd, RHI::IRHITexture* gbufferNormals,
                               RHI::IRHITexture* gbufferDepth, const DirectX::XMFLOAT4X4& invViewProj,
                               float giIntensity, uint32_t screenWidth, uint32_t screenHeight,
                               RHI::IRHITexture* output);

        const ProbeGridConfig& GetConfig() const { return m_config; }
        uint32_t GetProbeCount() const;

      private:
        void BuildProbeGrid();

        RHI::IRHIDevice* m_device = nullptr;
        std::unique_ptr<RHI::IRHIBuffer> m_probeBuffer; ///< RW structured buffer of ProbeData
        std::unique_ptr<RHI::IRHIBuffer> m_updateConstants;
        std::unique_ptr<RHI::IRHIBuffer> m_interpConstants;
        std::unique_ptr<RHI::IRHIShader> m_updateCS;
        std::unique_ptr<RHI::IRHIShader> m_interpolateCS;

        ProbeGridConfig m_config;

        /// Hemisphere sample directions from SSAOKernel for probe ray generation
        std::vector<SamplePoint> m_sampleDirections;

        bool m_initialized = false;
    };

} // namespace Spark::Graphics
