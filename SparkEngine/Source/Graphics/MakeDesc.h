/**
 * @file MakeDesc.h
 * @brief Zero-init factory functions for D3D11 descriptor structs
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides inline factory functions that return fully initialized D3D11
 * descriptors with sensible defaults. Eliminates verbose boilerplate when
 * creating rasterizer, blend, depth-stencil, sampler, buffer, and texture
 * descriptors.
 *
 * ## Usage
 * @code
 *   auto rasterDesc = Spark::Graphics::Desc::Rasterizer();
 *   auto blendDesc  = Spark::Graphics::Desc::BlendAlpha();
 *   auto samplerDesc = Spark::Graphics::Desc::SamplerAnisotropic(8);
 *
 *   ComPtr<ID3D11RasterizerState> rasterState;
 *   device->CreateRasterizerState(&rasterDesc, &rasterState);
 * @endcode
 *
 * @see PipelineStateCache, GraphicsEngine
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <cstdint>
#include <cfloat>

namespace Spark::Graphics::Desc
{

// ============================================================================
// Rasterizer Descriptors
// ============================================================================

/** @brief Default rasterizer: solid fill, back-face culling, depth clip enabled */
inline D3D11_RASTERIZER_DESC Rasterizer()
{
    D3D11_RASTERIZER_DESC desc = {};
    desc.FillMode = D3D11_FILL_SOLID;
    desc.CullMode = D3D11_CULL_BACK;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = 0;
    desc.DepthBiasClamp = 0.0f;
    desc.SlopeScaledDepthBias = 0.0f;
    desc.DepthClipEnable = TRUE;
    desc.ScissorEnable = FALSE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;
    return desc;
}

/** @brief No culling: solid fill, both faces visible */
inline D3D11_RASTERIZER_DESC RasterizerNoCull()
{
    D3D11_RASTERIZER_DESC desc = Rasterizer();
    desc.CullMode = D3D11_CULL_NONE;
    return desc;
}

/** @brief Wireframe: wireframe fill, no culling */
inline D3D11_RASTERIZER_DESC RasterizerWireframe()
{
    D3D11_RASTERIZER_DESC desc = Rasterizer();
    desc.FillMode = D3D11_FILL_WIREFRAME;
    desc.CullMode = D3D11_CULL_NONE;
    return desc;
}

/** @brief Shadow map rasterizer: solid fill, front-face culling with depth bias */
inline D3D11_RASTERIZER_DESC RasterizerShadow(int32_t depthBias = 1000, float slopeScale = 1.0f)
{
    D3D11_RASTERIZER_DESC desc = Rasterizer();
    desc.CullMode = D3D11_CULL_FRONT;
    desc.DepthBias = depthBias;
    desc.SlopeScaledDepthBias = slopeScale;
    desc.DepthBiasClamp = 0.0f;
    return desc;
}

// ============================================================================
// Blend Descriptors
// ============================================================================

/** @brief Opaque: no blending, write all channels */
inline D3D11_BLEND_DESC BlendOpaque()
{
    D3D11_BLEND_DESC desc = {};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    desc.RenderTarget[0].BlendEnable = FALSE;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return desc;
}

/** @brief Alpha blending: SrcAlpha / InvSrcAlpha */
inline D3D11_BLEND_DESC BlendAlpha()
{
    D3D11_BLEND_DESC desc = {};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    desc.RenderTarget[0].BlendEnable = TRUE;
    desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return desc;
}

/** @brief Additive blending: One / One */
inline D3D11_BLEND_DESC BlendAdditive()
{
    D3D11_BLEND_DESC desc = {};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    desc.RenderTarget[0].BlendEnable = TRUE;
    desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return desc;
}

/** @brief Pre-multiplied alpha: One / InvSrcAlpha */
inline D3D11_BLEND_DESC BlendPremultiplied()
{
    D3D11_BLEND_DESC desc = {};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    desc.RenderTarget[0].BlendEnable = TRUE;
    desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return desc;
}

// ============================================================================
// Depth-Stencil Descriptors
// ============================================================================

/** @brief Default depth: enabled, write all, less comparison */
inline D3D11_DEPTH_STENCIL_DESC DepthDefault()
{
    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    desc.DepthFunc = D3D11_COMPARISON_LESS;
    desc.StencilEnable = FALSE;
    desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    return desc;
}

/** @brief Read-only depth: enabled, no writes, less comparison */
inline D3D11_DEPTH_STENCIL_DESC DepthReadOnly()
{
    D3D11_DEPTH_STENCIL_DESC desc = DepthDefault();
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    return desc;
}

/** @brief Disabled depth: no testing, no writing */
inline D3D11_DEPTH_STENCIL_DESC DepthDisabled()
{
    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = FALSE;
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.StencilEnable = FALSE;
    return desc;
}

/** @brief Reverse-Z depth: enabled, write all, greater comparison */
inline D3D11_DEPTH_STENCIL_DESC DepthReverseZ()
{
    D3D11_DEPTH_STENCIL_DESC desc = DepthDefault();
    desc.DepthFunc = D3D11_COMPARISON_GREATER;
    return desc;
}

// ============================================================================
// Sampler Descriptors
// ============================================================================

/** @brief Trilinear filtering with wrap addressing */
inline D3D11_SAMPLER_DESC SamplerLinearWrap()
{
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.MipLODBias = 0.0f;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = FLT_MAX;
    return desc;
}

/** @brief Trilinear filtering with clamp addressing */
inline D3D11_SAMPLER_DESC SamplerLinearClamp()
{
    D3D11_SAMPLER_DESC desc = SamplerLinearWrap();
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    return desc;
}

/** @brief Point (nearest-neighbor) filtering with clamp addressing */
inline D3D11_SAMPLER_DESC SamplerPointClamp()
{
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MipLODBias = 0.0f;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = FLT_MAX;
    return desc;
}

/** @brief Point filtering with wrap addressing */
inline D3D11_SAMPLER_DESC SamplerPointWrap()
{
    D3D11_SAMPLER_DESC desc = SamplerPointClamp();
    desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    return desc;
}

/** @brief Anisotropic filtering with wrap addressing */
inline D3D11_SAMPLER_DESC SamplerAnisotropic(uint32_t maxAniso = 16)
{
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_ANISOTROPIC;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.MipLODBias = 0.0f;
    desc.MaxAnisotropy = maxAniso;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = FLT_MAX;
    return desc;
}

/** @brief Shadow map comparison sampler (PCF-friendly) */
inline D3D11_SAMPLER_DESC SamplerShadow()
{
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    desc.MipLODBias = 0.0f;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    desc.BorderColor[0] = 1.0f;
    desc.BorderColor[1] = 1.0f;
    desc.BorderColor[2] = 1.0f;
    desc.BorderColor[3] = 1.0f;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = 0.0f;
    return desc;
}

// ============================================================================
// Buffer Descriptors
// ============================================================================

/** @brief Generic buffer descriptor */
inline D3D11_BUFFER_DESC Buffer(uint32_t byteWidth, UINT bindFlags, D3D11_USAGE usage = D3D11_USAGE_DEFAULT)
{
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = byteWidth;
    desc.Usage = usage;
    desc.BindFlags = bindFlags;
    desc.CPUAccessFlags = (usage == D3D11_USAGE_DYNAMIC) ? D3D11_CPU_ACCESS_WRITE : 0;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    return desc;
}

/** @brief Vertex buffer descriptor */
inline D3D11_BUFFER_DESC VertexBuffer(uint32_t byteWidth, D3D11_USAGE usage = D3D11_USAGE_DEFAULT)
{
    return Buffer(byteWidth, D3D11_BIND_VERTEX_BUFFER, usage);
}

/** @brief Index buffer descriptor */
inline D3D11_BUFFER_DESC IndexBuffer(uint32_t byteWidth, D3D11_USAGE usage = D3D11_USAGE_DEFAULT)
{
    return Buffer(byteWidth, D3D11_BIND_INDEX_BUFFER, usage);
}

/** @brief Constant buffer descriptor */
inline D3D11_BUFFER_DESC ConstantBuffer(uint32_t byteWidth, D3D11_USAGE usage = D3D11_USAGE_DYNAMIC)
{
    return Buffer(byteWidth, D3D11_BIND_CONSTANT_BUFFER, usage);
}

/** @brief Structured buffer with SRV access */
inline D3D11_BUFFER_DESC StructuredBuffer(uint32_t elementCount, uint32_t stride,
                                           D3D11_USAGE usage = D3D11_USAGE_DEFAULT)
{
    D3D11_BUFFER_DESC desc = Buffer(elementCount * stride, D3D11_BIND_SHADER_RESOURCE, usage);
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = stride;
    return desc;
}

// ============================================================================
// Texture Descriptors
// ============================================================================

/** @brief 2D texture: single mip, single array slice, no MSAA */
inline D3D11_TEXTURE2D_DESC Texture2D(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                       UINT bindFlags = D3D11_BIND_SHADER_RESOURCE)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    return desc;
}

/** @brief Render target texture */
inline D3D11_TEXTURE2D_DESC RenderTarget(uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    return Texture2D(width, height, format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
}

/** @brief Depth-stencil texture */
inline D3D11_TEXTURE2D_DESC DepthStencil(uint32_t width, uint32_t height,
                                          DXGI_FORMAT format = DXGI_FORMAT_D24_UNORM_S8_UINT)
{
    return Texture2D(width, height, format, D3D11_BIND_DEPTH_STENCIL);
}

} // namespace Spark::Graphics::Desc

#endif // SPARK_PLATFORM_WINDOWS
