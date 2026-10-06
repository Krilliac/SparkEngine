/**
 * @file GraphicsEngineRHI.h
 * @brief Internal header for Linux RHI state shared across GraphicsEngine translation units
 *
 * This header is only included on non-Windows platforms. It provides the shared
 * LinuxRHIState singleton used by all GraphicsEngine .cpp files that were split
 * from the original monolithic GraphicsEngine.cpp.
 */
#pragma once

#ifndef SPARK_PLATFORM_WINDOWS

#include "../Core/Platform.h"
#include "RHI/RHI.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace Spark::Graphics::Detail
{

    /// PerFrameConstants of Shaders/GLSL/BasicVS.glsl and BasicPS.glsl (std140, binding 0).
    /// Matrices are stored as DirectXMath row-major XMFLOAT4X4, which GLSL reads as the
    /// transposed column-major matrix: `M * v` in GLSL is `v * M` in DirectXMath.
    struct BasicFrameConstants
    {
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 projection;
        DirectX::XMFLOAT4X4 viewProjection;
        DirectX::XMFLOAT3 cameraPosition;
        float time;
        DirectX::XMFLOAT3 cameraDirection;
        float deltaTime;
        DirectX::XMFLOAT2 screenResolution;
        DirectX::XMFLOAT2 invScreenResolution;
        DirectX::XMFLOAT3 lightDirection;
        float lightIntensity;
        DirectX::XMFLOAT3 lightColor;
        float ambientIntensity;
        DirectX::XMFLOAT3 ambientColor;
        float padding;
    };
    static_assert(sizeof(BasicFrameConstants) == 288, "must match the std140 PerFrameConstants block");

    /// PerObjectConstants of BasicVS.glsl / BasicPS.glsl (std140, binding 1).
    struct BasicObjectConstants
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 worldViewProjection;
        DirectX::XMFLOAT4X4 worldInverseTranspose;
        DirectX::XMFLOAT4X4 previousWorld;
        DirectX::XMFLOAT3 objectPosition;
        float objectScale;
        DirectX::XMFLOAT4 objectColor;
        DirectX::XMFLOAT4 materialProperties; ///< x metallic, y roughness, z emissive, w alpha
        DirectX::XMFLOAT4 uvTiling;           ///< xy tiling, zw offset
    };
    static_assert(sizeof(BasicObjectConstants) == 320, "must match the std140 PerObjectConstants block");

    /// PerMaterialConstants of BasicPS.glsl (std140, binding 2).
    struct BasicMaterialConstants
    {
        DirectX::XMFLOAT4 albedoColor;
        float metallicFactor;
        float roughnessFactor;
        float normalScale;
        float occlusionStrength;
        float emissiveFactor;
        float alphaCutoff;
        float padding[2];
    };
    static_assert(sizeof(BasicMaterialConstants) == 48, "must match the std140 PerMaterialConstants block");

    /**
     * @brief Resources of the Linux/macOS forward draw-list pass (GraphicsEngine::ProcessDrawList).
     *
     * Contract:
     *   - Thread affinity: game thread only (the thread that records the frame).
     *   - Ownership: owned by LinuxRHIState; created by GraphicsEngine::InitializeBasicShaders
     *     after the bridge is up, released by GraphicsEngine::Shutdown before the bridge shuts down.
     *   - Allocation: none per draw. `objectConstants` holds one PerObjectConstants buffer per
     *     draw of the largest frame seen so far and grows only when a frame exceeds that count.
     *     One buffer per draw is required because Vulkan reads a constant buffer when the
     *     command buffer executes, so rewriting a single buffer between draws of one frame
     *     would give every draw the last transform.
     *   - Scalability: suitable for the basic forward path (hundreds of draws); instancing and
     *     GPU-driven submission are D3D11-only today.
     *
     * A draw is recorded only with `pipeline` bound. Without it ProcessDrawList rejects the
     * frame's draws, counts them in `rejectedDraws` and logs an error, and never records an
     * unbound draw. On NullRHI the pipeline is built without shaders (the null device records
     * no GPU work), so headless runs keep recording their draws.
     */
    struct BasicForwardPass
    {
        std::unique_ptr<Spark::RHI::IRHIPipelineState> pipeline;
        /// The same shaders and state for the R16G16B16A16_FLOAT scene target the tone-mapping
        /// pass reads (Vulkan requires the pipeline's colour format to match the attachment).
        std::unique_ptr<Spark::RHI::IRHIPipelineState> hdrPipeline;
        std::unique_ptr<Spark::RHI::IRHIBuffer> frameConstants;
        std::unique_ptr<Spark::RHI::IRHIBuffer> materialConstants;
        std::vector<std::unique_ptr<Spark::RHI::IRHIBuffer>> objectConstants;
        std::unique_ptr<Spark::RHI::IRHISampler> sampler;
        uint64_t rejectedDraws = 0;
    };

    /// PostProcessConstants of Shaders/GLSL/PostProcess.glsl (std140, binding 1).
    struct PostProcessConstants
    {
        DirectX::XMFLOAT2 screenSize;
        DirectX::XMFLOAT2 invScreenSize;
        float exposure;
        float gamma;
        float vignetteStrength;
        float vignetteRadius;
        float chromaticStrength;
        float grainStrength;
        float time;
        float saturation;
    };
    static_assert(sizeof(PostProcessConstants) == 48, "must match the std140 PostProcessConstants block");

    /**
     * @brief Resources of the Linux/macOS tone-mapping post pass (PostProcessPass::Tonemapping).
     *
     * When the PostProcessingPipeline has Tonemapping enabled with a variant the shipped
     * PostProcess shader implements (the ACES operator with contrast 1), BeginFrame routes the
     * scene into LinuxRHIState::hdrLighting and GraphicsEngine::RenderPostProcessing draws
     * FullscreenQuad + PostProcess from it into the back buffer, the way the D3D11 pipeline's
     * tonemap pass writes the frame. The shader's gamma is set to 1 because the D3D11 pass
     * applies none.
     *
     * Contract:
     *   - Thread affinity: game thread only.
     *   - Ownership: owned by LinuxRHIState; created by GraphicsEngine::InitializeBasicShaders,
     *     released by GraphicsEngine::Shutdown after the device is idle.
     *   - Allocation: none per frame (one constant buffer, rewritten once per frame).
     *
     * A frame that asks for tone mapping the pass cannot perform (unsupported operator or
     * contrast, or a missing pipeline or HDR target) renders straight to the back buffer, is
     * counted in `rejectedFrames` and logged; no pass is reported for it.
     */
    struct TonemapPass
    {
        std::unique_ptr<Spark::RHI::IRHIPipelineState> pipeline;
        std::unique_ptr<Spark::RHI::IRHIBuffer> constants;
        std::unique_ptr<Spark::RHI::IRHISampler> sampler;
        uint64_t rejectedFrames = 0;
    };

    struct LinuxRHIState
    {
        Spark::RHI::RHIBridge bridge;
        bool initialized = false;
        uint32_t width = 0;
        uint32_t height = 0;
        std::chrono::high_resolution_clock::time_point frameStart;
        uint32_t frameCount = 0;
        float accumulatedTime = 0.0f;
        uint32_t measuredFps = 0;

        // GBuffer + HDR + Depth textures owned by the Linux/macOS rendering
        // layer. Created after bridge init, registered into the bridge's
        // render-target registry so cross-platform code (HybridRT dispatch,
        // golden-image capture, debug viewers) can look them up by slot.
        //
        // Layout matches Windows: GBuffer[0]=Albedo (RGBA8),
        // [1]=Normals (R16G16B16A16F), [2]=Material (RGBA8), [3]=Motion (RG16F).
        // Depth is D24_UNORM_S8_UINT, HDR is R16G16B16A16F with UAV so the
        // RT pass can write to it.
        std::unique_ptr<Spark::RHI::IRHITexture> gBufferAlbedo;
        std::unique_ptr<Spark::RHI::IRHITexture> gBufferNormals;
        std::unique_ptr<Spark::RHI::IRHITexture> gBufferMaterial;
        std::unique_ptr<Spark::RHI::IRHITexture> gBufferMotion;
        std::unique_ptr<Spark::RHI::IRHITexture> depthStencil;
        std::unique_ptr<Spark::RHI::IRHITexture> hdrLighting;

        // Shared 1x1 white fallback used by the basic material path.
        std::unique_ptr<Spark::RHI::IRHITexture> defaultTexture;

        // Pipeline, constant buffers and sampler of the forward draw-list pass.
        BasicForwardPass basicForward;

        // Pipeline, constants and sampler of the tone-mapping post pass.
        TonemapPass tonemap;

        // Colour target the current frame's scene pass records into: the back buffer, or
        // hdrLighting when BeginFrame routed the frame through the tone-mapping pass. Set by
        // BeginFrame; non-owning.
        Spark::RHI::IRHITexture* sceneTarget = nullptr;
    };

    inline LinuxRHIState& GetRHI()
    {
        static LinuxRHIState s;
        return s;
    }

    // Platform render target helpers (Linux/macOS). Defined in
    // GraphicsEngineLinuxRenderTargets.cpp; called from the lifecycle TU
    // (GraphicsEngineLinux.cpp: Initialize / Shutdown / Resize).
    void CreatePlatformRenderTargets(uint32_t width, uint32_t height);
    void ReleasePlatformRenderTargets();

    /**
     * Builds the tone-mapping post pass (LinuxRHIState::tonemap) and the forward pipeline variant
     * that renders the scene into the HDR target it reads (BasicForwardPass::hdrPipeline).
     * @p forwardDesc, @p forwardVs and @p forwardPs are the basic forward pass's description and
     * shaders; @p headless builds the pipelines without shaders, as the NullRHI path does.
     * Defined in GraphicsRenderPipelinesLinux.cpp beside the pass it records; called once from
     * GraphicsEngine::InitializeBasicShaders.
     * @return false, with the pass left empty, when any part is missing.
     */
    bool CreateTonemapPass(LinuxRHIState& rhi, const Spark::RHI::RHIPipelineStateDesc& forwardDesc,
                           Spark::RHI::IRHIShader* forwardVs, Spark::RHI::IRHIShader* forwardPs, bool headless);

} // namespace Spark::Graphics::Detail

#endif // !SPARK_PLATFORM_WINDOWS
