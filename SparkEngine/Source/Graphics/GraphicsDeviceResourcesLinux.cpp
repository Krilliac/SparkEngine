/**
 * @file GraphicsDeviceResourcesLinux.cpp
 * @brief Linux RHI-bridge device and resource management for GraphicsEngine
 *
 * Linux counterpart lives in GraphicsDeviceResourcesWindows.cpp.
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

// ============================================================================
// Basic Shader System — Linux/RHI
// ============================================================================

HRESULT GraphicsEngine::InitializeBasicShaders()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    // Register basic shader pairs (HLSL for Windows, GLSL for Linux)
    rhi.bridge.RegisterShader("basic_vs", Spark::RHI::RHIShaderStage::Vertex, "Shaders/Basic.hlsl",
                              "Shaders/Basic.vert.glsl", "Shaders/Basic.vert.spv", "main");
    rhi.bridge.RegisterShader("basic_ps", Spark::RHI::RHIShaderStage::Pixel, "Shaders/Basic.hlsl",
                              "Shaders/Basic.frag.glsl", "Shaders/Basic.frag.spv", "main");

    // Verify shaders can be loaded
    Spark::RHI::IRHIShader* vs = rhi.bridge.GetShader("basic_vs");
    Spark::RHI::IRHIShader* ps = rhi.bridge.GetShader("basic_ps");

    if (!vs || !ps)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to load basic shaders via RHI");
        return E_FAIL;
    }

    return S_OK;
}

HRESULT GraphicsEngine::CompileShaderFromFile(const std::wstring& filename, const char* entryPoint,
                                              const char* /*shaderModel*/, ID3DBlob** /*blobOut*/)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    // Convert wide string to narrow for RHI
    std::string narrowPath(filename.begin(), filename.end());

    // Determine shader stage from entry point naming convention
    Spark::RHI::RHIShaderStage stage = Spark::RHI::RHIShaderStage::Vertex;
    std::string ep(entryPoint);
    if (ep.contains("PS") || ep.contains("pixel") || ep.contains("frag"))
    {
        stage = Spark::RHI::RHIShaderStage::Pixel;
    }
    else if (ep.contains("CS") || ep.contains("compute"))
    {
        stage = Spark::RHI::RHIShaderStage::Compute;
    }
    else if (ep.contains("GS") || ep.contains("geometry"))
    {
        stage = Spark::RHI::RHIShaderStage::Geometry;
    }

    Spark::RHI::ShaderCompileOptions options;
    options.stage = stage;
    options.sourceFile = narrowPath;
    options.entryPoint = entryPoint;
    options.targetBackend = rhi.bridge.GetActiveBackend();

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);
    if (!result.success)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Shader compile failed: %s", result.errorMessage.c_str());
        return E_FAIL;
    }

    return S_OK;
}

HRESULT GraphicsEngine::CreateBasicConstantBuffer()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    constexpr uint64_t CB_SIZE = 256;
    auto cb = rhi.bridge.CreateConstantBuffer(CB_SIZE);
    if (!cb)
        return E_FAIL;

    constexpr uint64_t FRAME_CB_SIZE = 256;
    auto frameCB = rhi.bridge.CreateConstantBuffer(FRAME_CB_SIZE);
    if (!frameCB)
        return E_FAIL;

    return S_OK;
}

HRESULT GraphicsEngine::CreateDefaultTexture()
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    const uint32_t whitePixel = 0xFFFFFFFF;
    auto defaultTex = rhi.bridge.CreateTexture2D(1, 1, Spark::RHI::PixelFormat::R8G8B8A8_UNORM,
                                                 Spark::RHI::RHITextureUsage::ShaderResource, &whitePixel);

    if (!defaultTex)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create default texture via RHI");
        return E_FAIL;
    }

    return S_OK;
}

HRESULT GraphicsEngine::CompileEmbeddedVertexShader(ID3DBlob** /*blobOut*/)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    static constexpr const char* embeddedVS = R"(
#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(binding = 0) uniform Transforms {
    mat4 world;
    mat4 view;
    mat4 projection;
};

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragWorldPos;

void main() {
    vec4 worldPos = world * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    fragNormal = mat3(world) * inNormal;
    fragTexCoord = inTexCoord;
    gl_Position = projection * view * worldPos;
}
)";

    Spark::RHI::ShaderCompileOptions options;
    options.stage = Spark::RHI::RHIShaderStage::Vertex;
    options.sourceCode = embeddedVS;
    options.entryPoint = "main";
    options.sourceLanguage = Spark::RHI::ShaderLanguage::GLSL;
    options.targetBackend = rhi.bridge.GetActiveBackend();
    options.debugInfoEnabled = true;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);
    if (!result.success)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Embedded VS compile failed: %s", result.errorMessage.c_str());
        return E_FAIL;
    }

    Spark::RHI::IRHIDevice* device = rhi.bridge.GetDevice();
    if (device)
    {
        Spark::RHI::RHIShaderDesc desc;
        desc.stage = Spark::RHI::RHIShaderStage::Vertex;
        desc.bytecode = result.bytecode.data();
        desc.bytecodeSize = result.bytecode.size();
        desc.entryPoint = "main";
        desc.debugName = "EmbeddedBasicVS";
        auto shader = device->CreateShader(desc);
        if (!shader)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create embedded vertex shader");
            return E_FAIL;
        }
    }

    return S_OK;
}

HRESULT GraphicsEngine::CompileEmbeddedPixelShader(ID3DBlob** /*blobOut*/)
{
    auto& rhi = GetRHI();
    if (!rhi.initialized)
        return E_FAIL;

    static constexpr const char* embeddedPS = R"(
#version 450
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldPos;

layout(binding = 1) uniform sampler2D diffuseTexture;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 normal = normalize(fragNormal);
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diffuse = max(dot(normal, lightDir), 0.0);
    float ambient = 0.15;
    vec4 texColor = texture(diffuseTexture, fragTexCoord);
    outColor = vec4(texColor.rgb * (ambient + diffuse), texColor.a);
}
)";

    Spark::RHI::ShaderCompileOptions options;
    options.stage = Spark::RHI::RHIShaderStage::Pixel;
    options.sourceCode = embeddedPS;
    options.entryPoint = "main";
    options.sourceLanguage = Spark::RHI::ShaderLanguage::GLSL;
    options.targetBackend = rhi.bridge.GetActiveBackend();
    options.debugInfoEnabled = true;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);
    if (!result.success)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Embedded PS compile failed: %s", result.errorMessage.c_str());
        return E_FAIL;
    }

    Spark::RHI::IRHIDevice* device = rhi.bridge.GetDevice();
    if (device)
    {
        Spark::RHI::RHIShaderDesc desc;
        desc.stage = Spark::RHI::RHIShaderStage::Pixel;
        desc.bytecode = result.bytecode.data();
        desc.bytecodeSize = result.bytecode.size();
        desc.entryPoint = "main";
        desc.debugName = "EmbeddedBasicPS";
        auto shader = device->CreateShader(desc);
        if (!shader)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Failed to create embedded pixel shader");
            return E_FAIL;
        }
    }

    return S_OK;
}

void GraphicsEngine::SetBasicShaders()
{
    // On Linux, shader binding is handled through the RHI pipeline state objects.
}

void GraphicsEngine::UpdateBasicConstants(const DirectX::XMMATRIX& /*world*/, const DirectX::XMMATRIX& /*view*/,
                                          const DirectX::XMMATRIX& /*proj*/)
{
    // Constant buffer updates go through the RHI bridge on Linux.
}

void GraphicsEngine::UpdateFrameConstants(const XMMATRIX& view, const XMMATRIX& proj, const XMFLOAT3& cameraPos)
{
    // Store per-frame camera state for system queries (e.g. ClusteredLightCulling)
    m_frameViewMatrix = view;
    m_frameProjMatrix = proj;
    m_frameCameraPos = cameraPos;
    // Frame constants are managed per-subsystem through the RHI on Linux.
}

// --- W12 decor-instancing: the basic instanced draw path is D3D11-only for
//     now. The probe reports "unavailable" so callers (region decor) fall
//     back to their per-entity path automatically on Linux.
bool GraphicsEngine::HasInstancedBasicPipeline()
{
    return false;
}

void GraphicsEngine::SetBasicShadersInstanced()
{
    // No instanced basic pipeline on Linux yet (see HasInstancedBasicPipeline).
}

bool GraphicsEngine::DrawMeshInstanced(Mesh& /*mesh*/, const DirectX::XMFLOAT4X4* /*instanceWorlds*/,
                                       uint32_t /*instanceCount*/, uint32_t /*indexStart*/, uint32_t /*indexCount*/)
{
    return false;
}


#endif // !SPARK_PLATFORM_WINDOWS
