/**
 * @file BloomEffect.h
 * @brief HDR bloom post-processing with progressive downsample/upsample chain
 * @author Spark Engine Team
 * @date 2025
 *
 * Multi-pass bloom: threshold → progressive downsample → progressive upsample
 * with additive blending. Supports anamorphic stretch and scatter control.
 *
 * @see PostProcessingPipeline.h, VolumeSystem.h
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

#include <array>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    // =========================================================================
    // Bloom Settings
    // =========================================================================

    struct BloomSettings
    {
        bool enabled = false;
        float intensity = 1.0f;       ///< Final bloom intensity multiplier
        float threshold = 0.9f;       ///< Luminance threshold for bright pixels
        float softKnee = 0.5f;        ///< Soft knee for gradual threshold [0=hard, 1=soft]
        float scatter = 0.7f;         ///< How much bloom spreads (mix factor per level)
        float anamorphicRatio = 0.0f; ///< Anamorphic stretch [-1=vertical, 0=none, 1=horizontal]
        int maxIterations = 6;        ///< Max downsample/upsample passes (clamped to kMaxMips)
        float clampMax = 65472.0f;    ///< Max HDR value to prevent fireflies
    };

    // =========================================================================
    // Bloom Effect
    // =========================================================================

    /**
     * @brief Multi-pass HDR bloom using progressive Gaussian downsample/upsample
     *
     * Pipeline: Scene HDR → Prefilter (threshold) → Downsample chain → Upsample chain → Composite
     * Uses 13-tap downsampling filter to prevent aliasing and firefly artifacts.
     */
    class BloomEffect
    {
      public:
        static constexpr int kMaxMips = 8;

        BloomEffect() = default;
        ~BloomEffect() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height);
        void Shutdown();
        void Resize(uint32_t width, uint32_t height);

        /**
         * @brief Execute bloom on the input HDR texture
         * @param inputSRV  HDR scene texture
         * @return SRV containing the bloom result to composite with the scene
         */
        ID3D11ShaderResourceView* Execute(ID3D11ShaderResourceView* inputSRV);

        BloomSettings& GetSettings() { return m_settings; }
        const BloomSettings& GetSettings() const { return m_settings; }

        float GetLastGpuTimeMs() const { return m_lastGpuTimeMs; }

      private:
        bool CreateResources();
        bool CompileShaders();

        void PrefilterPass(ID3D11ShaderResourceView* input);
        void DownsamplePass(int srcMip);
        void UpsamplePass(int dstMip);

        // GPU constant buffer for bloom parameters
        struct BloomCB
        {
            XMFLOAT4 thresholdParams; // x=threshold, y=threshold-knee, z=2*knee, w=0.25/knee
            XMFLOAT4 texelSize;       // x=1/width, y=1/height, z=scatter, w=intensity
            XMFLOAT4 params;          // x=clampMax, y=currentMip, z=anamorphicRatio, w=unused
        };

        BloomSettings m_settings;

        // Device references
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;

        // Dimensions
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        int m_mipCount = 0;

        // Mip chain textures (downsample then upsample reuses these)
        struct MipLevel
        {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11ShaderResourceView> srv;
            uint32_t width = 0;
            uint32_t height = 0;
        };
        std::array<MipLevel, kMaxMips> m_mipChain;

        // Upsample results (separate from downsample so we can blend)
        std::array<MipLevel, kMaxMips> m_upsampleChain;

        // Prefilter result
        MipLevel m_prefilterTarget;

        // Shaders
        ComPtr<ID3D11VertexShader> m_fullscreenVS;
        ComPtr<ID3D11PixelShader> m_prefilterPS;
        ComPtr<ID3D11PixelShader> m_downsamplePS;
        ComPtr<ID3D11PixelShader> m_upsamplePS;

        // GPU resources
        ComPtr<ID3D11Buffer> m_constantBuffer;
        ComPtr<ID3D11SamplerState> m_linearClampSampler;

        float m_lastGpuTimeMs = 0.0f;
        bool m_initialized = false;
    };

} // namespace Spark::Graphics
