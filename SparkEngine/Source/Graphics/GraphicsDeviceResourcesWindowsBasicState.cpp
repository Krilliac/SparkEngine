/**
 * @file GraphicsDeviceResourcesWindowsBasicState.cpp
 * @brief D3D11 basic shader binding, constants, and basic material cache
 *
 * Basic shader binding, per-object/per-frame constant updates, blend/depth
 * mode state and the basic material JSON cache, split out of
 * GraphicsDeviceResourcesWindows.cpp. Linux counterpart lives in
 * GraphicsDeviceResourcesLinuxShaders.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "Shader.h"
#include "../Utils/LogMacros.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>

#include <string>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

void GraphicsEngine::SetBasicShaders()
{
    if (!m_context)
    {
        return;
    }

    // Set shaders
    m_context->VSSetShader(m_basicVertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_basicPixelShader.Get(), nullptr, 0);

    // Set input layout
    m_context->IASetInputLayout(m_basicInputLayout.Get());

    // Set constant buffers (per-object at slot 0, per-frame at slot 1)
    m_context->VSSetConstantBuffers(0, 1, m_basicConstantBuffer.GetAddressOf());
    m_context->VSSetConstantBuffers(1, 1, m_basicFrameConstantBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_basicConstantBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(1, 1, m_basicFrameConstantBuffer.GetAddressOf());

    // Set sampler state and default texture
    m_context->PSSetSamplers(0, 1, m_basicSamplerState.GetAddressOf());
    if (m_defaultSRV)
    {
        m_context->PSSetShaderResources(0, 1, m_defaultSRV.GetAddressOf());
    }

    // Default-bind the flat normal (t1) and fully-rough roughness (t2) so
    // every existing basic draw keeps its exact current look without any
    // call-site changes; callers with real maps opt in per-draw via
    // SetBasicMaterialTextures(). Also resets the slots at the start of each
    // basic batch in case another pass left stale SRVs bound there.
    SetBasicMaterialTextures(nullptr, nullptr);
}

void GraphicsEngine::SetBasicMaterialTextures(ID3D11ShaderResourceView* normalSrv,
                                              ID3D11ShaderResourceView* roughnessSrv)
{
    if (!m_context)
        return;

    EnsureDefaultMaterialTextures();
    ID3D11ShaderResourceView* views[2] = {normalSrv ? normalSrv : m_defaultNormalSRV.Get(),
                                          roughnessSrv ? roughnessSrv : m_defaultRoughnessSRV.Get()};
    m_context->PSSetShaderResources(1, 2, views);
}

void GraphicsEngine::UpdateBasicConstants(const XMMATRIX& world, const XMMATRIX& view, const XMMATRIX& proj)
{
    if (!m_basicConstantBuffer || !m_context)
    {
        return;
    }

    PerObjectConstants constants = {};
    constants.WorldMatrix = XMMatrixTranspose(world);
    constants.WorldViewProjectionMatrix = XMMatrixTranspose(world * view * proj);
    constants.WorldInverseTransposeMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
    constants.ObjectColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    constants.MaterialProperties = XMFLOAT4(0.0f, 0.5f, 0.0f, 1.0f); // Default material
    constants.UVTiling = XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f);           // Default UV tiling

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_context->Map(m_basicConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr))
    {
        memcpy(mappedResource.pData, &constants, sizeof(PerObjectConstants));
        m_context->Unmap(m_basicConstantBuffer.Get(), 0);
    }
}

void GraphicsEngine::UpdateFrameConstants(const XMMATRIX& view, const XMMATRIX& proj, const XMFLOAT3& cameraPos)
{
    // Store per-frame camera state for system queries (e.g. ClusteredLightCulling)
    m_frameViewMatrix = view;
    m_frameProjMatrix = proj;
    m_frameCameraPos = cameraPos;

    if (!m_basicFrameConstantBuffer || !m_context)
    {
        return;
    }

    PerFrameConstants frameConstants = {};
    frameConstants.ViewMatrix = XMMatrixTranspose(view);
    frameConstants.ProjectionMatrix = XMMatrixTranspose(proj);
    frameConstants.ViewProjectionMatrix = XMMatrixTranspose(view * proj);
    frameConstants.CameraPosition = cameraPos;
    frameConstants.Time = 0.0f;        // You could track actual time here
    frameConstants.DeltaTime = 0.016f; // Approximate 60 FPS
    frameConstants.ScreenResolution = XMFLOAT2(static_cast<float>(m_windowWidth), static_cast<float>(m_windowHeight));
    frameConstants.InvScreenResolution = XMFLOAT2((m_windowWidth > 0) ? (1.0f / m_windowWidth) : 0.0f,
                                                  (m_windowHeight > 0) ? (1.0f / m_windowHeight) : 0.0f);

    // Basic directional + ambient lighting. Ambient is deliberately strong so
    // surfaces facing away from the sun (character fronts, building walls) stay
    // readable instead of crushing to near-black — the basic PS is
    // ambient + N.L*directional with no other fill, so a low ambient made every
    // vertical face nearly black.
    // W12 day/night lane: once SetEnvironmentLighting has been called the module
    // override drives the frame; the legacy constants stay the no-module default.
    // The night ambient floor (>= 0.25) is enforced by the caller (TFDayNight).
    frameConstants.DirectionalLightDir = m_envLightingSet ? m_envLightDir : XMFLOAT3(0.35f, -0.8f, 0.45f);
    frameConstants.DirectionalLightIntensity = m_envLightingSet ? m_envLightIntensity : 1.0f;
    frameConstants.DirectionalLightColor = m_envLightingSet ? m_envLightColor : XMFLOAT3(1.0f, 0.97f, 0.9f);
    frameConstants.AmbientIntensity = m_envLightingSet ? m_envAmbientIntensity : 0.6f;
    frameConstants.AmbientColor = m_envLightingSet ? m_envAmbientColor : XMFLOAT3(0.52f, 0.5f, 0.46f);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_context->Map(m_basicFrameConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr))
    {
        memcpy(mappedResource.pData, &frameConstants, sizeof(PerFrameConstants));
        m_context->Unmap(m_basicFrameConstantBuffer.Get(), 0);
    }
}

void GraphicsEngine::UpdateBasicConstants(const XMMATRIX& world, const XMMATRIX& view, const XMMATRIX& proj,
                                          const XMFLOAT4& color, const XMFLOAT2& uvTiling)
{
    if (!m_basicConstantBuffer || !m_context)
    {
        return;
    }

    PerObjectConstants constants = {};
    constants.WorldMatrix = XMMatrixTranspose(world);
    constants.WorldViewProjectionMatrix = XMMatrixTranspose(world * view * proj);
    constants.WorldInverseTransposeMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
    constants.ObjectColor = color;
    constants.MaterialProperties = XMFLOAT4(0.0f, 0.5f, 0.0f, 1.0f);
    constants.UVTiling = XMFLOAT4(uvTiling.x, uvTiling.y, 0.0f, 0.0f);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_context->Map(m_basicConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr))
    {
        memcpy(mappedResource.pData, &constants, sizeof(PerObjectConstants));
        m_context->Unmap(m_basicConstantBuffer.Get(), 0);
    }
}

void GraphicsEngine::UpdateBasicConstants(const XMMATRIX& world, const XMMATRIX& view, const XMMATRIX& proj,
                                          const XMFLOAT4& color, const XMFLOAT2& uvTiling, float emissive, float alpha,
                                          const XMFLOAT2& uvOffset)
{
    if (!m_basicConstantBuffer || !m_context)
        return;

    PerObjectConstants constants = {};
    constants.WorldMatrix = XMMatrixTranspose(world);
    constants.WorldViewProjectionMatrix = XMMatrixTranspose(world * view * proj);
    constants.WorldInverseTransposeMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
    constants.ObjectColor = color;
    // z: emissive glow, w: alpha — read by the basic PS (both default to no-op).
    constants.MaterialProperties = XMFLOAT4(0.0f, 0.5f, emissive, alpha);
    constants.UVTiling = XMFLOAT4(uvTiling.x, uvTiling.y, uvOffset.x, uvOffset.y);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_context->Map(m_basicConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr))
    {
        memcpy(mappedResource.pData, &constants, sizeof(PerObjectConstants));
        m_context->Unmap(m_basicConstantBuffer.Get(), 0);
    }
}

void GraphicsEngine::SetBasicBlendMode(BasicBlendMode mode)
{
    if (!m_context || !m_device)
        return;

    // Lazily create the three blend states on first use.
    auto ensure = [this](ComPtr<ID3D11BlendState>& state, bool enable, D3D11_BLEND src, D3D11_BLEND dst)
    {
        if (state)
            return;
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = enable;
        desc.RenderTarget[0].SrcBlend = src;
        desc.RenderTarget[0].DestBlend = dst;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        m_device->CreateBlendState(&desc, &state);
    };

    ID3D11BlendState* bound = nullptr;
    switch (mode)
    {
    case BasicBlendMode::Alpha:
        ensure(m_blendAlpha, true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
        bound = m_blendAlpha.Get();
        break;
    case BasicBlendMode::Additive:
        ensure(m_blendAdditive, true, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE);
        bound = m_blendAdditive.Get();
        break;
    case BasicBlendMode::Opaque:
    default:
        ensure(m_blendOpaque, false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO);
        bound = m_blendOpaque.Get();
        break;
    }
    const float factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_context->OMSetBlendState(bound, factor, 0xffffffff);
}

void GraphicsEngine::SetBasicDepthMode(BasicDepthMode mode)
{
    if (!m_context || !m_device)
        return;

    // Lazily create the depth-stencil states on first use (mirrors
    // SetBasicBlendMode). Default recreates the exact desc CreateRenderStates()
    // uses for m_defaultDepthState (LESS, write ALL), so restoring Default is
    // identical to the frame baseline ApplyGraphicsState()/
    // ApplyBasicRenderStates() bind — and still works in attached mode if the
    // shared state object was never created.
    auto ensure =
        [this](ComPtr<ID3D11DepthStencilState>& state, D3D11_DEPTH_WRITE_MASK writeMask, D3D11_COMPARISON_FUNC func)
    {
        if (state)
            return;
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = writeMask;
        desc.DepthFunc = func;
        desc.StencilEnable = FALSE;
        m_device->CreateDepthStencilState(&desc, &state);
    };

    ID3D11DepthStencilState* bound = nullptr;
    switch (mode)
    {
    case BasicDepthMode::ReadOnly:
        // LESS_EQUAL (not LESS) so transparent/FX surfaces coplanar with
        // already-written opaque geometry still pass the test; write mask ZERO
        // is the whole point — transparent draws must not occlude later draws.
        ensure(m_depthReadOnly, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_LESS_EQUAL);
        bound = m_depthReadOnly.Get();
        break;
    case BasicDepthMode::Default:
    default:
        ensure(m_defaultDepthState, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS);
        bound = m_defaultDepthState.Get();
        break;
    }
    m_context->OMSetDepthStencilState(bound, 0);
}

void GraphicsEngine::SetBasicTexture(ID3D11ShaderResourceView* srv)
{
    if (!m_context)
        return;
    ID3D11ShaderResourceView* bind = srv ? srv : m_defaultSRV.Get();
    m_context->PSSetShaderResources(0, 1, &bind);
}

// Minimal extraction of "albedo"/"normal" (strings), "roughness" (string or
// scalar) and "tiling" ([x, y]) from the small material JSON files under
// Assets/Materials. Not a general JSON parser.
const GraphicsEngine::BasicMaterial* GraphicsEngine::GetOrLoadBasicMaterial(const std::string& jsonPath)
{
    if (jsonPath.empty())
        return nullptr;

    auto it = m_basicMaterialCache.find(jsonPath);
    if (it != m_basicMaterialCache.end())
        return &it->second;

    std::ifstream file(jsonPath);
    if (!file.is_open())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "GetOrLoadBasicMaterial: cannot open '%s'", jsonPath.c_str());
        return nullptr;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string content = ss.str();

    BasicMaterial mat{};

    // "albedo" : "path"
    auto findStringValue = [&content](const char* key) -> std::string
    {
        size_t kp = content.find(std::string("\"") + key + "\"");
        if (kp == std::string::npos)
            return {};
        size_t colon = content.find(':', kp);
        if (colon == std::string::npos)
            return {};
        size_t q1 = content.find('"', colon + 1);
        if (q1 == std::string::npos)
            return {};
        size_t q2 = content.find('"', q1 + 1);
        if (q2 == std::string::npos)
            return {};
        return content.substr(q1 + 1, q2 - q1 - 1);
    };

    // Material texture paths are relative to Assets/ unless already prefixed
    auto prefixAssets = [](std::string p) -> std::string
    {
        if (p.rfind("Assets/", 0) != 0 && p.rfind("Assets\\", 0) != 0)
            p = "Assets/" + p;
        return p;
    };

    std::string albedo = findStringValue("albedo");
    if (!albedo.empty())
        mat.srv = GetOrLoadTextureSRV(prefixAssets(albedo));

    // "normal" : "path" — tangent-space normal map, bound at t1 by
    // SetBasicMaterialTextures. Always a string in the shipped materials.
    std::string normalPath = findStringValue("normal");
    if (!normalPath.empty())
        mat.normalSrv = GetOrLoadTextureSRV(prefixAssets(normalPath));

    // "roughness" : "path" OR scalar (e.g. 0.3) — the shipped materials use
    // both forms. findStringValue would misparse a scalar (it grabs the NEXT
    // key's opening quote), so peek at the first non-whitespace character
    // after the colon to disambiguate. Scalars get a cached 1x1 texture so
    // the pixel shader has a single t2 sampling path.
    size_t rp = content.find("\"roughness\"");
    if (rp != std::string::npos)
    {
        size_t colon = content.find(':', rp);
        size_t vp = (colon == std::string::npos) ? std::string::npos : content.find_first_not_of(" \t\r\n", colon + 1);
        if (vp != std::string::npos)
        {
            if (content[vp] == '"')
            {
                size_t q2 = content.find('"', vp + 1);
                if (q2 != std::string::npos && q2 > vp + 1)
                    mat.roughnessSrv = GetOrLoadTextureSRV(prefixAssets(content.substr(vp + 1, q2 - vp - 1)));
            }
            else
            {
                float r = 0.0f;
                if (sscanf_s(content.c_str() + vp, "%f", &r) == 1)
                    mat.roughnessSrv = GetOrCreateScalarRoughnessSRV(r);
            }
        }
    }

    // "tiling" : [x, y]
    size_t tp = content.find("\"tiling\"");
    if (tp != std::string::npos)
    {
        size_t open = content.find('[', tp);
        if (open != std::string::npos)
        {
            float tx = 1.0f, ty = 1.0f;
            if (sscanf_s(content.c_str() + open, "[ %f , %f", &tx, &ty) == 2 ||
                sscanf_s(content.c_str() + open, "[%f,%f", &tx, &ty) == 2)
            {
                mat.tiling = {tx, ty};
            }
            else
            {
                // Tolerate newlines/whitespace between numbers
                std::string arr = content.substr(open + 1, content.find(']', open) - open - 1);
                for (char& c : arr)
                    if (c == ',')
                        c = ' ';
                std::istringstream as(arr);
                if (as >> tx >> ty)
                    mat.tiling = {tx, ty};
            }
        }
    }

    auto [ins, ok] = m_basicMaterialCache.emplace(jsonPath, std::move(mat));
    return &ins->second;
}

// Save-time invalidation for the editor's Basic Materials panel. Erasing one
// unordered_map node never moves the other cached BasicMaterials, and the only
// consumer (WorldBasicRenderer) re-fetches the pointer every draw, so dropping
// an entry mid-frame is safe.
bool GraphicsEngine::InvalidateBasicMaterial(const std::string& jsonPath)
{
    return m_basicMaterialCache.erase(jsonPath) != 0;
}
#endif // SPARK_PLATFORM_WINDOWS
