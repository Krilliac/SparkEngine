/**
 * @file TextureSystemWindowsTexture.cpp
 * @brief Texture class implementation for Windows (D3D11 + WIC image loading)
 *
 * Split from TextureSystemWindows.cpp, which keeps the TextureSystem manager,
 * console operations, and format utility functions. The Linux counterpart
 * lives in TextureSystemLinuxTexture.cpp.
 */

#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "TextureSystem.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <string>
#include <vector>

using namespace DirectX;

// ============================================================================
// TEXTURE CLASS IMPLEMENTATION
// ============================================================================

Texture::Texture(const std::string& name, const TextureDesc& desc) : m_name(name), m_desc(desc) {}

HRESULT Texture::CreateFromFile(const std::string& filePath, ID3D11Device* device)
{
    if (!device)
        return E_INVALIDARG;

    // Load image using WIC — ComPtr handles Release() on all exit paths
    HRESULT hr = S_OK;
    ComPtr<IWICImagingFactory> pFactory;
    ComPtr<IWICBitmapDecoder> pDecoder;
    ComPtr<IWICBitmapFrameDecode> pFrame;
    ComPtr<IWICFormatConverter> pConverter;
    UINT width, height;

    // Create WIC factory
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr))
        return hr;

    // Create decoder
    hr = pFactory->CreateDecoderFromFilename(std::wstring(filePath.begin(), filePath.end()).c_str(), nullptr,
                                             GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr))
        return hr;

    // Get frame
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr))
        return hr;

    pFrame->GetSize(&width, &height);
    m_desc.width = width;
    m_desc.height = height;

    // Convert to RGBA
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr))
        return hr;

    hr = pConverter->Initialize(pFrame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0f,
                                WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr))
        return hr;

    // Validate dimensions before allocation to prevent integer overflow
    if (width == 0 || height == 0)
        return E_INVALIDARG;

    // Check for multiplication overflow: width * 4 * height
    constexpr UINT kMaxTextureBytes = 256u * 1024u * 1024u; // 256 MB sanity limit
    if (width > kMaxTextureBytes / 4 || (width * 4) > kMaxTextureBytes / height)
        return E_OUTOFMEMORY;

    // Read pixel data
    UINT stride = width * 4;
    UINT bufferSize = stride * height;
    std::vector<BYTE> buffer(bufferSize);

    hr = pConverter->CopyPixels(nullptr, stride, bufferSize, buffer.data());
    if (SUCCEEDED(hr))
    {
        hr = CreateFromData(buffer.data(), bufferSize, device);
    }

    if (SUCCEEDED(hr))
    {
        m_loaded = true;
        m_memoryUsage = bufferSize;
    }

    return hr;
}

HRESULT Texture::CreateFromData(const void* data, size_t dataSize, ID3D11Device* device)
{
    if (!device || !data || dataSize == 0)
        return E_INVALIDARG;

    if (m_desc.width == 0 || m_desc.height == 0)
        return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_desc.width;
    texDesc.Height = m_desc.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = GetDXGIFormat(m_desc.format);
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = data;
    initData.SysMemPitch = m_desc.width * 4;

    HRESULT hr = device->CreateTexture2D(&texDesc, &initData, m_texture.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        hr = CreateViews(device);
        m_loaded = true;
        m_memoryUsage = dataSize;
    }

    return hr;
}

HRESULT Texture::CreateRenderTarget(ID3D11Device* device)
{
    if (!device)
        return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_desc.width;
    texDesc.Height = m_desc.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = GetDXGIFormat(m_desc.format);
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, m_texture.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        // Create render target view
        hr = device->CreateRenderTargetView(m_texture.Get(), nullptr, m_rtv.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            hr = CreateViews(device);
            m_loaded = true;
            m_memoryUsage = static_cast<size_t>(m_desc.width * m_desc.height * 4);
        }
    }

    return hr;
}

HRESULT Texture::CreateDepthStencil(ID3D11Device* device)
{
    if (!device)
        return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_desc.width;
    texDesc.Height = m_desc.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, m_texture.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        hr = device->CreateDepthStencilView(m_texture.Get(), nullptr, m_dsv.GetAddressOf());
        m_loaded = true;
        m_memoryUsage = static_cast<size_t>(m_desc.width * m_desc.height * 4);
    }

    return hr;
}

void Texture::Release()
{
    m_srv.Reset();
    m_uav.Reset();
    m_rtv.Reset();
    m_dsv.Reset();
    m_texture.Reset();
    m_loaded = false;
    m_memoryUsage = 0;
}

void Texture::Bind(ID3D11DeviceContext* context, uint32_t slot)
{
    if (context && m_srv)
    {
        ID3D11ShaderResourceView* srv = m_srv.Get();
        context->PSSetShaderResources(slot, 1, &srv);
    }
}

void Texture::UnBind(ID3D11DeviceContext* context, uint32_t slot)
{
    if (context)
    {
        ID3D11ShaderResourceView* nullSRV = nullptr;
        context->PSSetShaderResources(slot, 1, &nullSRV);
    }
}

HRESULT Texture::CreateViews(ID3D11Device* device)
{
    HRESULT hr = S_OK;

    // Create Shader Resource View
    D3D11_TEXTURE2D_DESC texDesc;
    m_texture->GetDesc(&texDesc);

    if (texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;

        if (texDesc.ArraySize > 1)
        {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.MipLevels = texDesc.MipLevels;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
            srvDesc.Texture2DArray.ArraySize = texDesc.ArraySize;
        }
        else
        {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
        }

        hr = device->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_srv.GetAddressOf());
        if (FAILED(hr))
            return hr;
    }

    // Create Unordered Access View if requested
    if (texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = texDesc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;

        hr = device->CreateUnorderedAccessView(m_texture.Get(), &uavDesc, m_uav.GetAddressOf());
        if (FAILED(hr))
            return hr;
    }

    return hr;
}

DXGI_FORMAT Texture::GetDXGIFormat(TextureFormat format) const
{
    switch (format)
    {
    case TextureFormat::R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::R8G8B8A8_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case TextureFormat::R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::R32G32B32A32_FLOAT:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case TextureFormat::BC1_UNORM:
        return DXGI_FORMAT_BC1_UNORM;
    case TextureFormat::BC1_SRGB:
        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case TextureFormat::BC3_UNORM:
        return DXGI_FORMAT_BC3_UNORM;
    case TextureFormat::BC3_SRGB:
        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case TextureFormat::BC7_UNORM:
        return DXGI_FORMAT_BC7_UNORM;
    case TextureFormat::BC7_SRGB:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    case TextureFormat::D24_UNORM_S8_UINT:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case TextureFormat::R16_FLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    case TextureFormat::R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

#endif // SPARK_PLATFORM_WINDOWS
