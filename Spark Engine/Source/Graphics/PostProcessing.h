/**
 * @file PostProcessing.h
 * @brief Advanced post-processing pipeline with HDR, bloom, and tone mapping
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides a comprehensive post-processing system supporting HDR
 * rendering, bloom effects, tone mapping, color grading, and various visual
 * effects for AAA-quality visuals.
 *
 * @warning This header defines 'class PostProcessingSystem' which conflicts
 * with the canonical definition in PostProcessingSystem.h. Do NOT include
 * both headers in the same translation unit. Prefer PostProcessingSystem.h
 * for the actively-maintained version implemented in PostProcessingSystem.cpp.
 * This header is retained for reference and will be merged in a future refactor.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

/**
 * @brief Post-processing effect types
 */
enum class PostProcessEffect
{
    None,
    Bloom,              ///< Bloom effect
    ToneMapping,        ///< HDR tone mapping
    ColorGrading,       ///< Color grading/correction
    FXAA,              ///< Fast Approximate Anti-Aliasing
    TAA,               ///< Temporal Anti-Aliasing
    SSAO,              ///< Screen Space Ambient Occlusion
    SSR,               ///< Screen Space Reflections
    MotionBlur,        ///< Motion blur
    DepthOfField,      ///< Depth of field
    Vignette,          ///< Vignette effect
    ChromaticAberration, ///< Chromatic aberration
    FilmGrain,         ///< Film grain
    LensDistortion,    ///< Lens distortion
    LightShafts,       ///< Volumetric light shafts
    LensFlare          ///< Lens flare
};

/**
 * @brief Tone mapping operators
 */
enum class ToneMappingOperator
{
    None,              ///< No tone mapping
    Reinhard,          ///< Reinhard tone mapping
    ReinhardJodie,     ///< Reinhard-Jodie tone mapping
    Uncharted2,        ///< Uncharted 2 tone mapping
    ACES,              ///< ACES filmic tone mapping
    AgX,               ///< AgX tone mapping
    FilmicALU,         ///< Filmic ALU tone mapping
    Custom             ///< Custom tone mapping curve
};

/**
 * @brief HDR post-processing pipeline
 */
class PostProcessingSystem
{
public:
    /**
     * @brief Bloom settings
     */
    struct BloomSettings
    {
        bool enabled = true;
        float threshold = 1.0f;
        float intensity = 1.0f;
        float radius = 1.0f;
        float softKnee = 0.5f;
        int iterations = 6;
        XMFLOAT3 tint = {1.0f, 1.0f, 1.0f};
    };

    /**
     * @brief Tone mapping settings
     */
    struct ToneMappingSettings
    {
        ToneMappingOperator operator_ = ToneMappingOperator::ACES;
        float exposure = 1.0f;
        float gamma = 2.2f;
        float whitePoint = 11.2f;
        XMFLOAT3 colorBalance = {1.0f, 1.0f, 1.0f};
    };

    /**
     * @brief Color grading settings
     */
    struct ColorGradingSettings
    {
        bool enabled = false;
        float temperature = 0.0f;
        float tint = 0.0f;
        float contrast = 1.0f;
        float brightness = 0.0f;
        float saturation = 1.0f;
        XMFLOAT3 lift = {1.0f, 1.0f, 1.0f};
        XMFLOAT3 gamma = {1.0f, 1.0f, 1.0f};
        XMFLOAT3 gain = {1.0f, 1.0f, 1.0f};
    };

    /**
     * @brief Metrics structure
     */
    struct PostProcessMetrics
    {
        float totalRenderTime = 0.0f;
        float bloomTime = 0.0f;
        float toneMappingTime = 0.0f;
        float colorGradingTime = 0.0f;
        uint32_t activeEffects = 0;
        float memoryUsage = 0.0f;
    };

    PostProcessingSystem();
    ~PostProcessingSystem();

    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height);
    void Shutdown();
    HRESULT Resize(uint32_t width, uint32_t height);

    void Process(ID3D11ShaderResourceView* sceneTexture, ID3D11RenderTargetView* finalTarget);

    // Settings accessors
    BloomSettings& GetBloomSettings() { return m_bloomSettings; }
    ToneMappingSettings& GetToneMappingSettings() { return m_toneMappingSettings; }
    ColorGradingSettings& GetColorGradingSettings() { return m_colorGradingSettings; }

    void EnableHDR(bool enabled) { m_hdrEnabled = enabled; }
    bool IsHDREnabled() const { return m_hdrEnabled; }

    // Console integration
    PostProcessMetrics Console_GetMetrics() const;
    std::string Console_ListEffects() const;
    void Console_EnableEffect(const std::string& effectName, bool enabled);
    void Console_SetExposure(float exposure);
    void Console_SetBloomParams(float threshold, float intensity, float radius);

private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    bool m_hdrEnabled = true;
    uint32_t m_width = 0, m_height = 0;

    BloomSettings m_bloomSettings;
    ToneMappingSettings m_toneMappingSettings;
    ColorGradingSettings m_colorGradingSettings;

    // Render targets
    ComPtr<ID3D11Texture2D> m_hdrTexture;
    ComPtr<ID3D11RenderTargetView> m_hdrRTV;
    ComPtr<ID3D11ShaderResourceView> m_hdrSRV;

    std::vector<ComPtr<ID3D11Texture2D>> m_bloomTextures;
    std::vector<ComPtr<ID3D11RenderTargetView>> m_bloomRTVs;
    std::vector<ComPtr<ID3D11ShaderResourceView>> m_bloomSRVs;

    // Shaders
    ComPtr<ID3D11VertexShader> m_fullscreenVS;
    ComPtr<ID3D11PixelShader> m_bloomExtractPS;
    ComPtr<ID3D11PixelShader> m_bloomBlurPS;
    ComPtr<ID3D11PixelShader> m_bloomCombinePS;
    ComPtr<ID3D11PixelShader> m_toneMappingPS;
    ComPtr<ID3D11PixelShader> m_finalPS;

    // Constant buffers
    ComPtr<ID3D11Buffer> m_bloomCB;
    ComPtr<ID3D11Buffer> m_toneMappingCB;

    // Samplers
    ComPtr<ID3D11SamplerState> m_linearSampler;
    ComPtr<ID3D11SamplerState> m_pointSampler;

    PostProcessMetrics m_metrics;

    HRESULT CreateRenderTargets();
    HRESULT CreateShaders();
    HRESULT CreateConstantBuffers();

    void RenderBloom(ID3D11ShaderResourceView* input);
    void RenderToneMapping(ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output);
};