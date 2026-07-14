/**
 * @file TemporalEffects.h
 * @brief Temporal effects system: TAA, motion blur, temporal reprojection
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides frame-to-frame temporal rendering techniques including Temporal
 * Anti-Aliasing (TAA), per-object and camera motion blur, and temporal
 * reprojection utilities for stable rendering.
 *
 * ## Usage
 * @code
 *   TemporalEffects temporal;
 *   temporal.Initialize(width, height);
 *   temporal.SetTAAEnabled(true);
 *   temporal.SetMotionBlurEnabled(true);
 *
 *   // Each frame
 *   temporal.BeginFrame(viewMatrix, projMatrix, deltaTime);
 *   // ... render scene ...
 *   temporal.EndFrame();
 *
 *   auto jitter = temporal.GetJitterOffset(); // Apply to projection matrix
 *   float blendFactor = temporal.GetHistoryBlendFactor();
 * @endcode
 */

#pragma once

// Subsystem headers
#include "TemporalTypes.h"

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <string>
#include <chrono>
#include <cstring>

// =============================================================================
// Temporal Effects System
// =============================================================================

/**
 * @class TemporalEffects
 * @brief Manages TAA, motion blur, and frame history for temporal rendering
 *
 * Coordinates temporal anti-aliasing jitter, history buffer management,
 * motion vector calculation, and motion blur rendering. Integrates with
 * the rendering pipeline by providing jitter offsets and blend factors.
 */
class TemporalEffects
{
  public:
    TemporalEffects() = default;
    ~TemporalEffects() = default;

    /**
     * @brief Initialize temporal effects with render target dimensions
     * @param width  Render target width in pixels
     * @param height Render target height in pixels
     * @return true on success
     */
    bool Initialize(uint32_t width = 1920, uint32_t height = 1080)
    {
        m_width = width;
        m_height = height;
        m_frameHistory.Clear();
        m_frameIndex = 0;
        m_initialized = true;
        return true;
    }

    /** @brief Shutdown and release resources */
    void Shutdown()
    {
        m_frameHistory.Clear();
        m_initialized = false;
    }

    /**
     * @brief Begin a new frame - call before rendering
     *
     * Records view/projection matrices for motion vector computation
     * and generates the jitter offset for this frame.
     */
    void BeginFrame(const XMFLOAT4X4& viewMatrix, const XMFLOAT4X4& projMatrix, float deltaTime)
    {
        m_frameIndex++;

        // Calculate jitter for TAA
        if (m_taaSettings.enabled)
        {
            m_currentJitter = JitterGenerator::GetJitterOffset(m_frameIndex, m_taaSettings.jitterPattern,
                                                               m_taaSettings.jitterSequenceLength);

            // Scale jitter to NDC space
            m_currentJitterNDC = {m_currentJitter.x * 2.0f / static_cast<float>(m_width),
                                  m_currentJitter.y * 2.0f / static_cast<float>(m_height)};
        }
        else
        {
            m_currentJitter = {0.0f, 0.0f};
            m_currentJitterNDC = {0.0f, 0.0f};
        }

        // Store frame data in history
        FrameHistory::FrameData frameData;
        frameData.viewMatrix = viewMatrix;
        frameData.projMatrix = projMatrix;
        frameData.jitterOffset = m_currentJitter;
        frameData.frameIndex = m_frameIndex;
        frameData.deltaTime = deltaTime;

        // Compute viewProj and its inverse for reprojection
#ifdef SPARK_PLATFORM_WINDOWS
        {
            XMMATRIX vMat = XMLoadFloat4x4(&viewMatrix);
            XMMATRIX pMat = XMLoadFloat4x4(&projMatrix);
            XMMATRIX vp = XMMatrixMultiply(vMat, pMat);
            XMStoreFloat4x4(&frameData.viewProjMatrix, vp);
            XMVECTOR det;
            XMMATRIX invVP = XMMatrixInverse(&det, vp);
            XMStoreFloat4x4(&frameData.invViewProjMatrix, invVP);
        }
#else
        frameData.viewProjMatrix = viewMatrix;
        frameData.invViewProjMatrix = viewMatrix;
#endif

        m_frameHistory.PushFrame(frameData);
    }

    /**
     * @brief End the current frame - call after rendering
     *
     * Swaps history buffers and prepares for the next frame.
     */
    void EndFrame()
    {
        // History ping-pong is handled by PushFrame
    }

    /**
     * @brief Update temporal effects simulation
     * @param deltaTime Frame delta time in seconds
     */
    void Update(float deltaTime) { m_totalTime += deltaTime; }

    /** @brief Set D3D11 device and context for GPU execution */
    void SetDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        m_device = device;
        m_context = context;
    }

    /** @brief Set the current frame's color SRV */
    void SetCurrentFrameSRV(ID3D11ShaderResourceView* srv) { m_currentFrameSRV = srv; }

    /** @brief Set the depth buffer SRV */
    void SetDepthSRV(ID3D11ShaderResourceView* srv) { m_depthSRV = srv; }

    /** @brief Set the motion vectors SRV (RG16F) */
    void SetMotionVectorsSRV(ID3D11ShaderResourceView* srv) { m_motionVectorsSRV = srv; }

    /** @brief Get the TAA-resolved output SRV */
    ID3D11ShaderResourceView* GetResolvedSRV() const { return m_historyColorSRVs[m_currentHistoryIndex]; }

    /** @brief Initialize GPU resources for temporal effects */
    bool InitializeGPU()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_device)
            return false;

        // Create history color buffers (ping-pong)
        for (int i = 0; i < 2; ++i)
        {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = m_width;
            desc.Height = m_height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_historyColorTextures[i]);
            if (FAILED(hr))
                return false;
            hr = m_device->CreateRenderTargetView(m_historyColorTextures[i].Get(), nullptr, &m_historyColorRTVs[i]);
            if (FAILED(hr))
                return false;
            hr = m_device->CreateShaderResourceView(m_historyColorTextures[i].Get(), nullptr, &m_historyColorSRVs[i]);
            if (FAILED(hr))
                return false;
        }

        // Create constant buffer
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(TemporalCB);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
        if (FAILED(hr))
            return false;

        // Create samplers
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = m_device->CreateSamplerState(&sampDesc, &m_linearSampler);
        if (FAILED(hr))
            return false;

        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        hr = m_device->CreateSamplerState(&sampDesc, &m_pointSampler);
        if (FAILED(hr))
            return false;

        // Compile TAA resolve shader
        CompileTAAShaders();
        CompileMotionBlurShaders();

        return true;
#else
        return false;
#endif
    }

    /** @brief Render/apply temporal effects (post-process pass) */
    void Render()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_context || !m_device)
            return;

        // TAA resolve pass
        if (m_taaSettings.enabled && m_taaResolvePS && m_frameHistory.HasSufficientHistory())
        {
            int prevHistory = 1 - m_currentHistoryIndex;
            int curHistory = m_currentHistoryIndex;

            // Set render target to current history buffer
            m_context->OMSetRenderTargets(1, &m_historyColorRTVs[curHistory], nullptr);

            // Compile CB
            TemporalCB cb = {};
            cb.screenSize = {static_cast<float>(m_width), static_cast<float>(m_height), 1.0f / m_width,
                             1.0f / m_height};
            cb.taaParams = {m_taaSettings.historyBlendFactor, m_taaSettings.varianceClipGamma, m_taaSettings.sharpness,
                            m_taaSettings.ghostingRejectionStrength};
            cb.jitterOffset = {m_currentJitter.x, m_currentJitter.y, m_currentJitterNDC.x, m_currentJitterNDC.y};
            cb.motionBlurParams = {m_motionBlurSettings.intensity, static_cast<float>(m_motionBlurSettings.sampleCount),
                                   m_motionBlurSettings.maxBlurRadius, m_motionBlurSettings.velocityScale};

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) &&
                mapped.pData)
            {
                memcpy(mapped.pData, &cb, sizeof(TemporalCB));
                m_context->Unmap(m_constantBuffer.Get(), 0);
            }

            m_context->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
            m_context->PSSetShader(m_taaResolvePS.Get(), nullptr, 0);
            m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

            // t0 = current frame, t1 = history, t2 = motion vectors, t3 = depth
            ID3D11ShaderResourceView* srvs[] = {m_currentFrameSRV, m_historyColorSRVs[prevHistory], m_motionVectorsSRV,
                                                m_depthSRV};
            m_context->PSSetShaderResources(0, 4, srvs);

            ID3D11SamplerState* samplers[] = {m_linearSampler.Get(), m_pointSampler.Get()};
            m_context->PSSetSamplers(0, 2, samplers);

            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->IASetInputLayout(nullptr);
            m_context->Draw(3, 0);

            // Swap history index for next frame
            m_currentHistoryIndex = 1 - m_currentHistoryIndex;
        }

        // Motion blur pass (after TAA resolve)
        if (m_motionBlurSettings.enabled && m_motionBlurPS)
        {
            // Motion blur reads from the TAA-resolved output
            // Output goes to the other history buffer or a separate target
        }
#endif
    }

    /**
     * @brief Handle render target resize
     */
    void Resize(uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;
        m_frameHistory.Clear(); // Invalidate history on resize
    }

    // ---- TAA Accessors ----

    /** @brief Get the current frame's jitter offset in pixel space */
    XMFLOAT2 GetJitterOffset() const { return m_currentJitter; }

    /** @brief Get the jitter offset in NDC space (for projection matrix) */
    XMFLOAT2 GetJitterOffsetNDC() const { return m_currentJitterNDC; }

    /** @brief Get the TAA history blend factor */
    float GetHistoryBlendFactor() const
    {
        if (!m_frameHistory.HasSufficientHistory())
            return 0.0f;
        return m_taaSettings.historyBlendFactor;
    }

    /** @brief Check if TAA is active and has history */
    bool IsTAAActive() const { return m_taaSettings.enabled && m_frameHistory.HasSufficientHistory(); }

    // ---- Motion Blur Accessors ----

    /** @brief Check if motion blur is active */
    bool IsMotionBlurActive() const { return m_motionBlurSettings.enabled; }

    /** @brief Get the motion blur sample count */
    int GetMotionBlurSamples() const { return m_motionBlurSettings.sampleCount; }

    /** @brief Get the maximum blur radius in pixels */
    float GetMaxBlurRadius() const { return m_motionBlurSettings.maxBlurRadius; }

    // ---- Settings ----

    TAASettings& GetTAASettings() { return m_taaSettings; }
    const TAASettings& GetTAASettings() const { return m_taaSettings; }

    MotionBlurSettings& GetMotionBlurSettings() { return m_motionBlurSettings; }
    const MotionBlurSettings& GetMotionBlurSettings() const { return m_motionBlurSettings; }

    void SetTAAEnabled(bool enabled) { m_taaSettings.enabled = enabled; }
    void SetMotionBlurEnabled(bool enabled) { m_motionBlurSettings.enabled = enabled; }

    /** @brief Get the frame history for temporal reprojection */
    const FrameHistory& GetFrameHistory() const { return m_frameHistory; }

    /** @brief Get the current frame index */
    uint32_t GetFrameIndex() const { return m_frameIndex; }

    /** @brief Check if the system is initialized */
    bool IsInitialized() const { return m_initialized; }

    // ---- Console Integration ----

    /** @brief Get a summary string of temporal effects state */
    std::string Console_GetStatus() const
    {
        std::string status = "Temporal Effects:\n";
        status += "  TAA: " + std::string(m_taaSettings.enabled ? "ON" : "OFF");
        status += " (blend=" + std::to_string(m_taaSettings.historyBlendFactor) + ")\n";
        status += "  Motion Blur: " + std::string(m_motionBlurSettings.enabled ? "ON" : "OFF");
        status += " (intensity=" + std::to_string(m_motionBlurSettings.intensity) + ")\n";
        status += "  History frames: " + std::to_string(m_frameHistory.GetFrameCount()) + "\n";
        status += "  Frame index: " + std::to_string(m_frameIndex) + "\n";
        return status;
    }

  private:
    // ---- Shader Compilation ----

    void CompileTAAShaders()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        // Fullscreen vertex shader
        const char* vsSource = R"(
            struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
            VSOutput main(uint id : SV_VertexID) {
                VSOutput o;
                o.uv = float2((id << 1) & 2, id & 2);
                o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1);
                return o;
            }
        )";
        ComPtr<ID3DBlob> vsBlob, errBlob;
        if (SUCCEEDED(D3DCompile(vsSource, strlen(vsSource), "TemporalVS", nullptr, nullptr, "main", "vs_5_0", 0, 0,
                                 &vsBlob, &errBlob)))
        {
            m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_fullscreenVS);
        }

        // TAA Resolve pixel shader with variance clipping and YCoCg
        const char* taaPS = R"(
            Texture2D currentFrame : register(t0);
            Texture2D historyFrame : register(t1);
            Texture2D motionVectors : register(t2);
            Texture2D depthBuffer : register(t3);
            SamplerState linearSamp : register(s0);
            SamplerState pointSamp : register(s1);

            cbuffer TemporalCB : register(b0) {
                float4 screenSize;      // xy=size, zw=1/size
                float4 taaParams;       // x=blendFactor, y=varianceGamma, z=sharpness, w=ghostRejection
                float4 jitterOffset;    // xy=pixel, zw=ndc
                float4 motionBlurParams;
            };

            float3 RGBToYCoCg(float3 rgb) {
                return float3(
                    0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b,
                    0.5 * rgb.r - 0.5 * rgb.b,
                    -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b
                );
            }
            float3 YCoCgToRGB(float3 ycocg) {
                float y = ycocg.x, co = ycocg.y, cg = ycocg.z;
                return float3(y + co - cg, y + cg, y - co - cg);
            }

            float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                // Remove jitter from UV for unjittered sampling
                float2 unjitteredUV = uv - jitterOffset.zw * 0.5;

                // Sample motion vector and reproject
                float2 motion = motionVectors.Sample(pointSamp, uv).rg;
                float2 historyUV = uv - motion;

                // Current frame sample (unjittered)
                float3 currentColor = currentFrame.Sample(pointSamp, unjitteredUV).rgb;

                // Check if history UV is valid
                if (any(historyUV < 0) || any(historyUV > 1)) {
                    return float4(currentColor, 1);
                }

                // History sample
                float3 historyColor = historyFrame.Sample(linearSamp, historyUV).rgb;

                // Neighborhood clamping in YCoCg space
                float3 neighborMin = float3(1e10, 1e10, 1e10);
                float3 neighborMax = float3(-1e10, -1e10, -1e10);
                float3 neighborMean = float3(0, 0, 0);
                float3 neighborM2 = float3(0, 0, 0);

                [unroll]
                for (int y = -1; y <= 1; y++) {
                    [unroll]
                    for (int x = -1; x <= 1; x++) {
                        float2 offset = float2(x, y) * screenSize.zw;
                        float3 s = currentFrame.Sample(pointSamp, unjitteredUV + offset).rgb;
                        float3 sYCoCg = RGBToYCoCg(s);
                        neighborMin = min(neighborMin, sYCoCg);
                        neighborMax = max(neighborMax, sYCoCg);
                        neighborMean += sYCoCg;
                        neighborM2 += sYCoCg * sYCoCg;
                    }
                }

                neighborMean /= 9.0;
                neighborM2 /= 9.0;
                float3 neighborStdDev = sqrt(abs(neighborM2 - neighborMean * neighborMean));

                // Variance clip history
                float3 histYCoCg = RGBToYCoCg(historyColor);
                float gamma = taaParams.y;
                float3 clipMin = neighborMean - neighborStdDev * gamma;
                float3 clipMax = neighborMean + neighborStdDev * gamma;
                histYCoCg = clamp(histYCoCg, clipMin, clipMax);
                historyColor = YCoCgToRGB(histYCoCg);

                // Blend
                float blendFactor = taaParams.x;

                // Reduce blend when motion is high (less history reliance)
                float motionMag = length(motion * screenSize.xy);
                blendFactor *= saturate(1.0 - motionMag * 0.1);

                float3 resolved = lerp(currentColor, historyColor, blendFactor);

                // Optional sharpening
                if (taaParams.z > 0.001) {
                    float3 blur = neighborMean;
                    float3 diff = RGBToYCoCg(resolved) - blur;
                    resolved = YCoCgToRGB(RGBToYCoCg(resolved) + diff * taaParams.z);
                }

                return float4(max(resolved, 0), 1);
            }
        )";

        ComPtr<ID3DBlob> psBlob;
        if (SUCCEEDED(D3DCompile(taaPS, strlen(taaPS), "TAAResolve", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob,
                                 &errBlob)))
        {
            m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_taaResolvePS);
        }
#endif
    }

    void CompileMotionBlurShaders()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        const char* mbPS = R"(
            Texture2D sceneTexture : register(t0);
            Texture2D motionVectors : register(t1);
            Texture2D depthBuffer : register(t2);
            SamplerState linearSamp : register(s0);
            SamplerState pointSamp : register(s1);

            cbuffer TemporalCB : register(b0) {
                float4 screenSize;
                float4 taaParams;
                float4 jitterOffset;
                float4 motionBlurParams; // x=intensity, y=samples, z=maxRadius, w=velScale
            };

            float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
                float2 velocity = motionVectors.Sample(pointSamp, uv).rg * motionBlurParams.w;
                float velMag = length(velocity * screenSize.xy);

                // Skip if motion is too small
                if (velMag < 0.5)
                    return sceneTexture.Sample(linearSamp, uv);

                // Clamp velocity to max blur radius
                float maxRad = motionBlurParams.z * screenSize.z;
                velocity = clamp(velocity, -maxRad, maxRad) * motionBlurParams.x;

                int samples = (int)motionBlurParams.y;
                float3 color = sceneTexture.Sample(linearSamp, uv).rgb;
                float totalWeight = 1.0;
                float centerDepth = depthBuffer.Sample(pointSamp, uv).r;

                for (int i = 1; i < samples && i < 16; i++) {
                    float t = (float)i / (float)(samples - 1) - 0.5;
                    float2 sampleUV = uv + velocity * t;
                    float sampleDepth = depthBuffer.Sample(pointSamp, sampleUV).r;

                    // Depth-aware weighting: don't blur background over foreground
                    float depthDiff = centerDepth - sampleDepth;
                    float w = (depthDiff > 0.01) ? 0.2 : 1.0;

                    color += sceneTexture.Sample(linearSamp, sampleUV).rgb * w;
                    totalWeight += w;
                }

                return float4(color / totalWeight, 1);
            }
        )";

        ComPtr<ID3DBlob> psBlob, errBlob;
        if (SUCCEEDED(D3DCompile(mbPS, strlen(mbPS), "MotionBlur", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob,
                                 &errBlob)))
        {
            m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_motionBlurPS);
        }
#endif
    }

    // ---- GPU Constant Buffer (mapped per-frame) ----
    struct TemporalCB
    {
        XMFLOAT4 screenSize;       ///< (width, height, 1/width, 1/height).
        XMFLOAT4 taaParams;        ///< (blendFactor, varianceClipGamma, reserved, reserved).
        XMFLOAT4 jitterOffset;     ///< (jitterX, jitterY, prevJitterX, prevJitterY) in pixels.
        XMFLOAT4 motionBlurParams; ///< (intensity, maxSamples, velocityScale, reserved).
    };

    // ---- Pipeline state ----
    bool m_initialized = false;    ///< Whether Initialize() completed successfully.
    uint32_t m_width = 1920;       ///< Current render target width (pixels).
    uint32_t m_height = 1080;      ///< Current render target height (pixels).
    uint32_t m_frameIndex = 0;     ///< Monotonic frame counter (drives Halton jitter sequence).
    float m_totalTime = 0.0f;      ///< Accumulated wall-clock time (seconds).
    int m_currentHistoryIndex = 0; ///< Ping-pong index into history color buffers (0 or 1).

    // Per-effect settings
    TAASettings m_taaSettings;
    MotionBlurSettings m_motionBlurSettings;

    // Frame history for reprojection
    FrameHistory m_frameHistory;

    // Sub-pixel jitter for TAA (Halton 2,3 sequence)
    XMFLOAT2 m_currentJitter = {0.0f, 0.0f};    ///< Jitter offset in pixels.
    XMFLOAT2 m_currentJitterNDC = {0.0f, 0.0f}; ///< Jitter offset in NDC [-1, 1] space.

    // GPU resources (non-owning pointers to GraphicsEngine-owned objects)
    ID3D11Device* m_device = nullptr;                       ///< D3D11 device for resource creation.
    ID3D11DeviceContext* m_context = nullptr;               ///< Immediate context for draw calls.
    ID3D11ShaderResourceView* m_currentFrameSRV = nullptr;  ///< Current frame's HDR color input.
    ID3D11ShaderResourceView* m_depthSRV = nullptr;         ///< Scene depth buffer for reprojection.
    ID3D11ShaderResourceView* m_motionVectorsSRV = nullptr; ///< Per-pixel motion vectors (G-buffer output).

    // History color buffers (ping-pong for temporal accumulation)
    ComPtr<ID3D11Texture2D> m_historyColorTextures[2];    ///< Backing textures for TAA history.
    ID3D11RenderTargetView* m_historyColorRTVs[2] = {};   ///< RTVs for writing resolved TAA output.
    ID3D11ShaderResourceView* m_historyColorSRVs[2] = {}; ///< SRVs for reading previous frame's result.

    // Per-pass shaders
    ComPtr<ID3D11VertexShader> m_fullscreenVS; ///< Shared fullscreen triangle vertex shader.
    ComPtr<ID3D11PixelShader> m_taaResolvePS;  ///< TAA resolve: blend current + history with neighborhood clamp.
    ComPtr<ID3D11PixelShader> m_motionBlurPS;  ///< Per-pixel motion blur using motion vectors.

    // Shared GPU resources
    ComPtr<ID3D11Buffer> m_constantBuffer;      ///< TemporalCB constant buffer.
    ComPtr<ID3D11SamplerState> m_linearSampler; ///< Bilinear sampler for history reprojection.
    ComPtr<ID3D11SamplerState> m_pointSampler;  ///< Point sampler for exact-texel velocity reads.
};
