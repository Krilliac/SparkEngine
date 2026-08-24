/**
 * @file GraphicsDeviceResourcesLinuxShaders.cpp
 * @brief Linux RHI-bridge basic shader system for GraphicsEngine
 *
 * Basic shader / constant buffer / default texture / basic material path split
 * out of GraphicsDeviceResourcesLinux.cpp (which keeps device and render-target
 * creation plus pipeline setup). Windows counterpart lives in
 * GraphicsDeviceResourcesWindows.cpp.
 */
#include "../Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "GraphicsEngineRHI.h"
#include "ProjectAssetPath.h"
#include "RHI/RHI.h"
#include "../Utils/Validate.h"

#include <string>
#include <cstring>

using namespace Spark::Graphics::Detail;

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

void GraphicsEngine::UpdateBasicConstants(const DirectX::XMMATRIX& world, const DirectX::XMMATRIX& view,
                                          const DirectX::XMMATRIX& proj, const DirectX::XMFLOAT4& /*color*/,
                                          const DirectX::XMFLOAT2& /*uvTiling*/)
{
    UpdateBasicConstants(world, view, proj);
}

void GraphicsEngine::UpdateBasicConstants(const DirectX::XMMATRIX& world, const DirectX::XMMATRIX& view,
                                          const DirectX::XMMATRIX& proj, const DirectX::XMFLOAT4& /*color*/,
                                          const DirectX::XMFLOAT2& /*uvTiling*/, float /*emissive*/, float /*alpha*/,
                                          const DirectX::XMFLOAT2& /*uvOffset*/)
{
    UpdateBasicConstants(world, view, proj);
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

// --- Basic-material texture path: the Windows implementation decodes via WIC
//     into a D3D11 SRV. That path is D3D11-only; on Linux real texturing goes
//     through the RHI bridge, so no basic-path SRV is produced. Every caller
//     already treats a null return as "no texture" (SetBasicTexture binds the
//     default, and the editor/game guards skip the bind), so returning nullptr
//     is the correct non-Windows behavior rather than a hard failure.
ID3D11ShaderResourceView* GraphicsEngine::GetOrLoadTextureSRV(const std::string& /*path*/)
{
    return nullptr;
}

void GraphicsEngine::SetBasicTexture(ID3D11ShaderResourceView* /*srv*/)
{
    // Basic D3D11 SRVs are not part of the non-Windows RHI path.
}

const GraphicsEngine::BasicMaterial* GraphicsEngine::GetOrLoadBasicMaterial(const std::string& /*jsonPath*/,
                                                                            std::string_view /*projectRootUtf8*/)
{
    return nullptr;
}

bool GraphicsEngine::InvalidateBasicTexture(const std::string& path)
{
    const auto canonical = Spark::CanonicalizeFilesystemPath(path);
    if (!canonical)
        return false;
    const size_t erasedSuccess = m_basicTextureCache.erase(canonical->cacheKey);
    const size_t erasedFailure = m_failedBasicTexturePaths.erase(canonical->cacheKey);
    return erasedSuccess != 0 || erasedFailure != 0;
}

bool GraphicsEngine::InvalidateBasicMaterial(const std::string& jsonPath, std::string_view projectRootUtf8)
{
    const auto canonical = Spark::ResolveProjectAssetPath(projectRootUtf8, jsonPath);
    const auto root = Spark::CanonicalizeFilesystemPath(projectRootUtf8);
    if (!canonical || !root)
        return false;

    const std::string materialCacheKey = root->cacheKey + '\n' + canonical->cacheKey;
    bool erased = m_failedBasicMaterialPaths.erase(materialCacheKey) != 0;
    for (auto it = m_basicMaterialCache.begin(); it != m_basicMaterialCache.end();)
    {
        if (it->second.jsonPath == canonical->cacheKey)
        {
            it = m_basicMaterialCache.erase(it);
            erased = true;
        }
        else
        {
            ++it;
        }
    }
    for (auto it = m_basicMaterialAliases.begin(); it != m_basicMaterialAliases.end();)
    {
        if (!m_basicMaterialCache.contains(it->second))
            it = m_basicMaterialAliases.erase(it);
        else
            ++it;
    }
    return erased;
}

#endif // !SPARK_PLATFORM_WINDOWS
