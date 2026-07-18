/**
 * @file GraphicsDeviceResourcesLinux.cpp
 * @brief Linux RHI-bridge device and resource management for GraphicsEngine
 *
 * Device/render-target creation and pipeline setup. The basic shader system
 * lives in GraphicsDeviceResourcesLinuxShaders.cpp. Windows counterpart lives
 * in GraphicsDeviceResourcesWindows.cpp.
 */
#include "../Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "GraphicsEngineRHI.h"
#include "../Utils/Validate.h"

#include <string>
#include <cstring>

using namespace Spark::Graphics::Detail;

// ============================================================================
// Device/Resource Creation — Linux/RHI
// ============================================================================

HRESULT GraphicsEngine::CreateDeviceAndSwapChain(HWND hWnd)
{
    auto& rhi = GetRHI();
    if (rhi.initialized)
        return S_OK;

    Spark::RHI::GraphicsBackend backend = Spark::RHI::RHIBridge::GetRecommendedBackend();
    bool ok = rhi.bridge.Initialize(static_cast<void*>(hWnd), m_width, m_height, backend,
#ifndef NDEBUG
                                    true
#else
                                    false
#endif
    );

    if (!ok)
        return E_FAIL;

    rhi.initialized = true;
    rhi.width = m_width;
    rhi.height = m_height;
    return S_OK;
}

HRESULT GraphicsEngine::CreateDevice(HWND hwnd, uint32_t width, uint32_t height, bool fullscreen)
{
    m_width = width;
    m_height = height;
    m_windowWidth = width;
    m_windowHeight = height;
    m_fullscreen = fullscreen;

    auto& rhi = GetRHI();
    if (rhi.initialized)
        return S_OK;

    Spark::RHI::GraphicsBackend backend = Spark::RHI::RHIBridge::GetRecommendedBackend();
    bool ok = rhi.bridge.Initialize(static_cast<void*>(hwnd), width, height, backend,
#ifndef NDEBUG
                                    true
#else
                                    false
#endif
    );

    if (!ok)
        return E_FAIL;

    rhi.initialized = true;
    rhi.width = width;
    rhi.height = height;
    return S_OK;
}

HRESULT GraphicsEngine::CreateRenderTargetView()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    Spark::RHI::IRHITexture* backBuffer = rhi.bridge.GetBackBuffer();
    return backBuffer ? S_OK : E_FAIL;
}

HRESULT GraphicsEngine::CreateDepthStencilView()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    Spark::RHI::IRHITexture* depthBuffer = rhi.bridge.GetDepthBuffer();
    return depthBuffer ? S_OK : E_FAIL;
}

HRESULT GraphicsEngine::CreateRenderTargets()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    // Create the HDR render target through the RHI bridge
    auto hdrTarget = rhi.bridge.CreateRenderTarget(rhi.width, rhi.height, Spark::RHI::PixelFormat::R16G16B16A16_FLOAT);
    if (!hdrTarget)
        return E_FAIL;

    return S_OK;
}

HRESULT GraphicsEngine::CreateAdvancedRenderTargets()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    Spark::RHI::IRHIDevice* device = rhi.bridge.GetDevice();
    if (!device)
        return E_FAIL;

    // G-Buffer render targets for deferred rendering
    constexpr Spark::RHI::PixelFormat gBufferFormats[] = {
        Spark::RHI::PixelFormat::R8G8B8A8_UNORM,     // Albedo
        Spark::RHI::PixelFormat::R16G16B16A16_FLOAT, // Normals
        Spark::RHI::PixelFormat::R8G8B8A8_UNORM,     // Material (metallic, roughness, AO)
        Spark::RHI::PixelFormat::R16G16_FLOAT        // Motion vectors
    };

    for (const auto& format : gBufferFormats)
    {
        auto gBufferRT = rhi.bridge.CreateRenderTarget(rhi.width, rhi.height, format);
        if (!gBufferRT)
            return E_FAIL;
    }

    return S_OK;
}

HRESULT GraphicsEngine::CreateRenderStates()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    Spark::RHI::IRHIDevice* device = rhi.bridge.GetDevice();
    if (!device)
        return E_FAIL;

    // Pipeline states encapsulate rasterizer, blend, and depth-stencil state in the RHI.
    Spark::RHI::RHIPipelineStateDesc defaultPsoDesc;
    defaultPsoDesc.rasterizer.fillMode = Spark::RHI::RHIFillMode::Solid;
    defaultPsoDesc.rasterizer.cullMode = Spark::RHI::RHICullMode::Back;
    defaultPsoDesc.depthStencil.depthEnable = true;
    defaultPsoDesc.depthStencil.depthWrite = true;
    defaultPsoDesc.depthStencil.depthFunc = Spark::RHI::RHICompareOp::Less;
    defaultPsoDesc.blend.renderTargets[0].blendEnable = false;
    defaultPsoDesc.topology = Spark::RHI::RHIPrimitiveTopology::TriangleList;
    defaultPsoDesc.renderTargetFormats[0] = Spark::RHI::PixelFormat::R8G8B8A8_UNORM;
    defaultPsoDesc.numRenderTargets = 1;
    defaultPsoDesc.debugName = "DefaultPipelineState";

    return S_OK;
}

void GraphicsEngine::SetViewport()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    Spark::RHI::RHIViewport vp;
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(m_width);
    vp.height = static_cast<float>(m_height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    cmd->SetViewport(vp);

    Spark::RHI::RHIScissorRect sr;
    sr.left = 0;
    sr.top = 0;
    sr.right = static_cast<int32_t>(m_width);
    sr.bottom = static_cast<int32_t>(m_height);
    cmd->SetScissorRect(sr);
}

// ============================================================================
// Pipeline Setup — Linux/RHI
// ============================================================================

void GraphicsEngine::SetupDeferredPipeline()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    // Register deferred shaders with the shader cache
    rhi.bridge.RegisterShader("deferred_geometry_vs", Spark::RHI::RHIShaderStage::Vertex,
                              "Shaders/Deferred/GeometryPass.hlsl", "Shaders/Deferred/GeometryPass.vert.glsl");
    rhi.bridge.RegisterShader("deferred_geometry_ps", Spark::RHI::RHIShaderStage::Pixel,
                              "Shaders/Deferred/GeometryPass.hlsl", "Shaders/Deferred/GeometryPass.frag.glsl");
    rhi.bridge.RegisterShader("deferred_lighting_vs", Spark::RHI::RHIShaderStage::Vertex,
                              "Shaders/Deferred/LightingPass.hlsl", "Shaders/Deferred/LightingPass.vert.glsl");
    rhi.bridge.RegisterShader("deferred_lighting_ps", Spark::RHI::RHIShaderStage::Pixel,
                              "Shaders/Deferred/LightingPass.hlsl", "Shaders/Deferred/LightingPass.frag.glsl");

    // Create the G-Buffer render targets
    CreateAdvancedRenderTargets();
}

void GraphicsEngine::SetupForwardPlusPipeline()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return;

    // Register forward+ shaders
    rhi.bridge.RegisterShader("forwardplus_depth_vs", Spark::RHI::RHIShaderStage::Vertex,
                              "Shaders/ForwardPlus/DepthPrepass.hlsl", "Shaders/ForwardPlus/DepthPrepass.vert.glsl");
    rhi.bridge.RegisterShader("forwardplus_light_cull_cs", Spark::RHI::RHIShaderStage::Compute,
                              "Shaders/ForwardPlus/LightCull.hlsl", "Shaders/ForwardPlus/LightCull.comp.glsl");
    rhi.bridge.RegisterShader("forwardplus_shading_vs", Spark::RHI::RHIShaderStage::Vertex,
                              "Shaders/ForwardPlus/Shading.hlsl", "Shaders/ForwardPlus/Shading.vert.glsl");
    rhi.bridge.RegisterShader("forwardplus_shading_ps", Spark::RHI::RHIShaderStage::Pixel,
                              "Shaders/ForwardPlus/Shading.hlsl", "Shaders/ForwardPlus/Shading.frag.glsl");

    // Create depth pre-pass render target
    // Intentional: CreateDepthBuffer registers the resource internally; local handle unused
    [[maybe_unused]] auto depthPrePass = rhi.bridge.CreateDepthBuffer(rhi.width, rhi.height);
}

#endif // !SPARK_PLATFORM_WINDOWS
