#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file PBRMaterialBinding.cpp
 * @brief Shader resource binding for PBR materials
 *
 * Contains BindToShader, CompileMaterial (D3D11 pipeline state creation),
 * texture slot unloading, and constant buffer creation and updates.
 * Materials do not read texture files: callers hand them shader resource
 * views through Material::SetTexture().
 * Core material state is in PBRMaterial.cpp.
 * Reflection and shader permutation logic is in PBRMaterialLighting.cpp.
 */

#include "MaterialSystem.h"
#include "../Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/LogMacros.h"
#include <cstring>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// MATERIAL CLASS — Texture Slots & Shader Binding (Windows)
// ============================================================================

void Material::UnloadTexture(MaterialTextureType type)
{
    auto it = m_textures.find(type);
    if (it != m_textures.end())
    {
        m_textures.erase(it);
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Material '" + m_name + "' does not have texture of type " +
                                                       std::to_string(static_cast<int>(type)) + " to unload");
    }
}

void Material::BindToShader(ID3D11DeviceContext* context) const
{
    if (!context)
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Null context in Material::BindToShader for material: " +
                                                       m_name);
        return;
    }

    // Define texture slot mapping for consistent shader binding
    static const std::unordered_map<MaterialTextureType, UINT> textureSlotMapping = {
        {MaterialTextureType::Albedo, 0},
        {MaterialTextureType::Normal, 1},
        {MaterialTextureType::Metallic, 2},
        {MaterialTextureType::Roughness, 3},
        {MaterialTextureType::Occlusion, 4},
        {MaterialTextureType::Emissive, 5},
        {MaterialTextureType::Height, 6},
        {MaterialTextureType::DetailAlbedo, 7},
        {MaterialTextureType::DetailNormal, 8},
        {MaterialTextureType::Subsurface, 9},
        {MaterialTextureType::Transmission, 10},
        {MaterialTextureType::Clearcoat, 11},
        {MaterialTextureType::ClearcoatRoughness, 12},
        {MaterialTextureType::Anisotropy, 13},
        {MaterialTextureType::Custom0, 14},
        {MaterialTextureType::Custom1, 15},
        {MaterialTextureType::Custom2, 16},
        {MaterialTextureType::Custom3, 17}};

    // Bind material textures to their designated slots
    std::vector<ID3D11ShaderResourceView*> srvArray(18, nullptr); // Max 18 texture slots
    std::vector<ID3D11SamplerState*> samplerArray(18, nullptr);

    int boundTextures = 0;
    for (const auto& texturePair : m_textures)
    {
        MaterialTextureType type = texturePair.first;
        const MaterialTexture& matTexture = texturePair.second;

        auto slotIt = textureSlotMapping.find(type);
        if (slotIt == textureSlotMapping.end())
        {
            continue; // Unknown texture type
        }

        UINT slot = slotIt->second;
        if (slot >= 18)
            continue; // Safety check

        if (matTexture.enabled && matTexture.texture)
        {
            srvArray[slot] = matTexture.texture.Get();

            // For now, we'll use a default sampler since we don't have access to MaterialSystem here
            // In a full implementation, you'd pass the sampler or get it from a global manager
            boundTextures++;
        }
    }

    SPARK_LOG_TRACE(Spark::LogCategory::Graphics, "Binding material '%s': %d textures to shader", m_name.c_str(),
                    boundTextures);

    // Bind all textures at once for efficiency
    if (boundTextures > 0)
    {
        context->PSSetShaderResources(0, 18, srvArray.data());

        // Note: Samplers would also be bound here if we had access to them
        // context->PSSetSamplers(0, 18, samplerArray.data());
    }

    // Bind the material constant buffer if compiled
    if (m_constantBuffer)
    {
        // Update constant buffer contents
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr) && mapped.pData)
        {
            MaterialConstants constants = {};
            constants.albedoColor = m_pbrProperties.albedoColor;
            constants.metallicFactor = m_pbrProperties.metallicFactor;
            constants.roughnessFactor = m_pbrProperties.roughnessFactor;
            constants.normalScale = m_pbrProperties.normalScale;
            constants.occlusionStrength = m_pbrProperties.occlusionStrength;
            constants.emissiveColor = m_pbrProperties.emissiveColor;
            constants.emissiveFactor = m_pbrProperties.emissiveFactor;
            constants.alphaCutoff = m_pbrProperties.alphaCutoff;
            constants.indexOfRefraction = m_pbrProperties.indexOfRefraction;
            constants.pad0 = 0.0f;
            constants.pad1 = 0.0f;
            std::memcpy(mapped.pData, &constants, sizeof(MaterialConstants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }

        ID3D11Buffer* cbuffers[] = {m_constantBuffer.Get()};
        context->PSSetConstantBuffers(1, 1, cbuffers);
    }

    // Set pipeline states if compiled
    if (m_blendState)
    {
        const FLOAT blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
    }

    if (m_depthStencilState)
    {
        context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
    }

    if (m_rasterizerState)
    {
        context->RSSetState(m_rasterizerState.Get());
    }

// Log binding for debugging in debug builds
#ifdef _DEBUG
    static int bindCount = 0;
    if (++bindCount % 100 == 0)
    { // Log every 100 binds to avoid spam
        Spark::SimpleConsole::GetInstance().LogInfo("Material '" + m_name + "' bound with " +
                                                    std::to_string(boundTextures) + " textures");
    }
#endif
}

HRESULT Material::CompileMaterial(ID3D11Device* device)
{
    if (!device)
    {
        Spark::SimpleConsole::GetInstance().LogError("CompileMaterial: device is null for material '" + m_name + "'");
        return E_INVALIDARG;
    }

    HRESULT hr = S_OK;

    // ---- Blend state ----
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    auto& rt = blendDesc.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    switch (m_renderState.blendMode)
    {
    case BlendMode::Opaque:
        rt.BlendEnable = FALSE;
        break;
    case BlendMode::AlphaTest:
        rt.BlendEnable = FALSE; // Alpha test handled in pixel shader via alphaCutoff
        break;
    case BlendMode::Transparent:
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        break;
    case BlendMode::Additive:
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_ONE;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        break;
    case BlendMode::Multiply:
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_DEST_COLOR;
        rt.DestBlend = D3D11_BLEND_ZERO;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_DEST_ALPHA;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        break;
    case BlendMode::Screen:
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        break;
    }

    hr = device->CreateBlendState(&blendDesc, &m_blendState);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("CompileMaterial: failed to create blend state for '" + m_name +
                                                     "'");
        return hr;
    }

    // ---- Depth stencil state ----
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = m_renderState.depthTest ? TRUE : FALSE;
    dsDesc.DepthWriteMask = m_renderState.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
    dsDesc.StencilEnable = FALSE;

    hr = device->CreateDepthStencilState(&dsDesc, &m_depthStencilState);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("CompileMaterial: failed to create depth stencil state for '" +
                                                     m_name + "'");
        return hr;
    }

    // ---- Rasterizer state ----
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.FrontCounterClockwise = FALSE;
    rsDesc.DepthBias = 0;
    rsDesc.DepthBiasClamp = 0.0f;
    rsDesc.SlopeScaledDepthBias = 0.0f;
    rsDesc.DepthClipEnable = TRUE;
    rsDesc.ScissorEnable = FALSE;
    rsDesc.MultisampleEnable = FALSE;
    rsDesc.AntialiasedLineEnable = FALSE;

    if (m_renderState.doubleSided || m_renderState.cullMode == CullMode::None)
    {
        rsDesc.CullMode = D3D11_CULL_NONE;
    }
    else if (m_renderState.cullMode == CullMode::Front)
    {
        rsDesc.CullMode = D3D11_CULL_FRONT;
    }
    else
    {
        rsDesc.CullMode = D3D11_CULL_BACK;
    }

    hr = device->CreateRasterizerState(&rsDesc, &m_rasterizerState);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("CompileMaterial: failed to create rasterizer state for '" +
                                                     m_name + "'");
        return hr;
    }

    // ---- Material constant buffer ----
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(MaterialConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    MaterialConstants constants = {};
    constants.albedoColor = m_pbrProperties.albedoColor;
    constants.metallicFactor = m_pbrProperties.metallicFactor;
    constants.roughnessFactor = m_pbrProperties.roughnessFactor;
    constants.normalScale = m_pbrProperties.normalScale;
    constants.occlusionStrength = m_pbrProperties.occlusionStrength;
    constants.emissiveColor = m_pbrProperties.emissiveColor;
    constants.emissiveFactor = m_pbrProperties.emissiveFactor;
    constants.alphaCutoff = m_pbrProperties.alphaCutoff;
    constants.indexOfRefraction = m_pbrProperties.indexOfRefraction;
    constants.pad0 = 0.0f;
    constants.pad1 = 0.0f;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = &constants;

    hr = device->CreateBuffer(&cbDesc, &initData, &m_constantBuffer);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("CompileMaterial: failed to create constant buffer for '" +
                                                     m_name + "'");
        return hr;
    }

    m_compiled = true;
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Material '%s' compiled successfully (blend=%d, cull=%d)",
                   m_name.c_str(), static_cast<int>(m_renderState.blendMode), static_cast<int>(m_renderState.cullMode));
    Spark::SimpleConsole::GetInstance().LogInfo("Material '" + m_name + "' compiled successfully");
    return S_OK;
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"

// ============================================================================
// Material (Linux) — Texture Slots & Shader Binding
// ============================================================================

void Material::UnloadTexture(MaterialTextureType type)
{
    m_textures.erase(type);
}

void Material::BindToShader(ID3D11DeviceContext* /*context*/) const
{
    // No-op on Linux - no GPU binding available
}

HRESULT Material::CompileMaterial(ID3D11Device* /*device*/)
{
    // No GPU pipeline state on Linux
    m_compiled = true;
    return S_OK;
}

#endif // SPARK_PLATFORM_WINDOWS
