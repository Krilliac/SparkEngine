/**
 * @file GraphicsDeviceResourcesWindowsTextures.cpp
 * @brief D3D11 default, procedural, and file-loaded textures for the basic path
 *
 * Default white/normal/roughness textures, the procedural blob-shadow disc,
 * scalar roughness SRVs and WIC-based texture loading, split out of
 * GraphicsDeviceResourcesWindows.cpp. Linux counterpart lives in
 * GraphicsDeviceResourcesLinuxShaders.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"

#include <windows.h>
#include <d3d11_1.h>
#include <DirectXPackedVector.h> // XMConvertFloatToHalf for the fp16 1x1 material defaults
#include <wrl.h>

#include <string>
#include <cstdint>
#include <cstdio>
#include <cmath> // sqrtf (GetOrCreateSoftCircleShadowSRV radial falloff)
#include <vector>
#include <wincodec.h>

using Microsoft::WRL::ComPtr;

void GraphicsEngine::EnsureDefaultMaterialTextures()
{
    if (!m_device || (m_defaultNormalSRV && m_defaultRoughnessSRV))
        return;

    // Flat tangent-space normal (0.5, 0.5, 1). FLOAT16 format on purpose:
    //  - the shader decodes n = sample * 2 - 1, and 0.5 / 1.0 are exact
    //    powers of two in half-float, so nTS comes out EXACTLY (0, 0, 1).
    //    An RGBA8 texel of 128 decodes to 128/255*2-1 = +0.0039, which would
    //    lean every default-lit normal ~0.2 degrees off geometric.
    //  - fp16 linear filtering is mandatory from feature level 10.0 up
    //    (fp32 filtering is optional), and the basic sampler is trilinear.
    // Half-float bit patterns: 0.5f == 0x3800, 1.0f == 0x3C00.
    if (!m_defaultNormalSRV)
    {
        const uint16_t flatNormal[4] = {0x3800, 0x3800, 0x3C00, 0x3C00}; // (0.5, 0.5, 1, 1)
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = flatNormal;
        data.SysMemPitch = sizeof(flatNormal);

        if (SUCCEEDED(m_device->CreateTexture2D(&desc, &data, &m_defaultNormalTexture)))
            m_device->CreateShaderResourceView(m_defaultNormalTexture.Get(), nullptr, &m_defaultNormalSRV);
        if (!m_defaultNormalSRV)
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default flat normal texture", L"ERROR");
    }

    // Fully-rough default (1.0), NOT mid-gray: the specular term is scaled by
    // (1 - roughness), so 1.0 makes it exactly zero and draws that never bind
    // a roughness map render bit-identical to the pre-specular shader.
    if (!m_defaultRoughnessSRV)
    {
        const uint16_t fullyRough = 0x3C00; // 1.0f as half
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = &fullyRough;
        data.SysMemPitch = sizeof(fullyRough);

        if (SUCCEEDED(m_device->CreateTexture2D(&desc, &data, &m_defaultRoughnessTexture)))
            m_device->CreateShaderResourceView(m_defaultRoughnessTexture.Get(), nullptr, &m_defaultRoughnessSRV);
        if (!m_defaultRoughnessSRV)
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default roughness texture", L"ERROR");
    }
}

ID3D11ShaderResourceView* GraphicsEngine::GetOrCreateScalarRoughnessSRV(float roughness)
{
    if (!m_device)
        return nullptr;

    // Explicit clamp (windows.h min/max macros make std::clamp hazardous here)
    if (roughness < 0.0f)
        roughness = 0.0f;
    else if (roughness > 1.0f)
        roughness = 1.0f;

    // Cache alongside file-loaded textures under a synthetic key ('#' cannot
    // start a real path) so repeated materials share one 1x1 texture.
    char key[48];
    (void)sprintf_s(key, "#roughness:%.4f", roughness);

    auto it = m_basicTextureCache.find(key);
    if (it != m_basicTextureCache.end())
        return it->second.Get();

    // fp16 like the defaults (guaranteed filterable); ~3-decimal precision is
    // plenty for a roughness scalar.
    const uint16_t half = DirectX::PackedVector::XMConvertFloatToHalf(roughness);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = &half;
    data.SysMemPitch = sizeof(half);

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(m_device->CreateTexture2D(&desc, &data, &tex)))
        return nullptr;

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
        return nullptr;

    auto& slot = m_basicTextureCache[key];
    slot = srv;
    return slot.Get();
}

// ============================================================================
// Blob shadow support (W13 shadow-polish lane)
// ----------------------------------------------------------------------------
// The basic render path (SetBasicShaders / this file) has no shadow term at
// all: the embedded basic PS below is ambient + N.L directional + a view fill
// + opt-in specular, and DrawSceneObjects never binds a depth/shadow map for
// it. ShadowAtlas.h/.cpp DOES exist engine-side, but it is wired only into the
// deferred/PBR path (RenderPipeline.cpp, RenderGraph/RenderGraphBuilder.cpp,
// LightingSystem*) -- SceneRenderer.cpp (the other candidate consumer) never
// references it either. Splicing real shadow-map sampling into the basic PS
// would need a shadow depth pass, atlas slot allocation for the basic path,
// and a new sampler/texture seam through SetBasicShaders() -- real work, not
// a scoped pass.
//
// Instead: a cheap ground-projected "blob shadow" (soft dark alpha disc under
// pawns/vehicles/deployables) reads well at MMOFPS scale and needs zero
// shadow-map plumbing. This is the only new engine-side piece it needs: a
// cached 32x32 procedural radial-falloff alpha texture. The draw itself
// (flat CreatePlane() quad, scaled to the blob radius, alpha-blended, depth
// READ-ONLY) lives client-side in Game/TFBlobShadows.cpp and uses only the
// existing SetBasicShaders/UpdateBasicConstants/SetBasicBlendMode/
// SetBasicDepthMode surface -- ObjectColor.rgb is driven to (0,0,0) so the
// basic PS's lighting/specular terms multiply out to exactly black
// regardless of the sun angle (finalColor.rgb = texColor.rgb *
// ObjectColor.rgb * lighting + specular; specular is 0 because the draw
// never binds a roughness map, so the default fully-rough t2 binding zeroes
// gloss), and MaterialProperties.w (the alpha param) carries the
// per-instance height/distance fade.
// ============================================================================
ID3D11ShaderResourceView* GraphicsEngine::GetOrCreateSoftCircleShadowSRV()
{
    if (!m_device)
        return nullptr;
    if (m_blobShadowSRV)
        return m_blobShadowSRV.Get();

    constexpr int kDim = 32;
    constexpr float kCenter = (kDim - 1) * 0.5f;
    constexpr float kMaxDist = kDim * 0.5f; // texel-space radius to the edge

    uint8_t pixels[kDim * kDim * 4];
    for (int y = 0; y < kDim; ++y)
    {
        for (int x = 0; x < kDim; ++x)
        {
            const float dx = static_cast<float>(x) - kCenter;
            const float dy = static_cast<float>(y) - kCenter;
            const float dist = sqrtf(dx * dx + dy * dy);

            // Explicit clamps (windows.h min/max macros make std::clamp
            // hazardous here -- same reasoning as GetOrCreateScalarRoughnessSRV).
            float t = dist / kMaxDist; // 0 at center .. ~1 at the corner
            if (t < 0.0f)
                t = 0.0f;
            else if (t > 1.0f)
                t = 1.0f;

            // Solid-ish core (kInnerT) softening to fully transparent at the
            // rim (kOuterT) via smoothstep, so the disc has no hard ring edge.
            constexpr float kInnerT = 0.35f;
            constexpr float kOuterT = 1.0f;
            float edge = (t - kInnerT) / (kOuterT - kInnerT);
            if (edge < 0.0f)
                edge = 0.0f;
            else if (edge > 1.0f)
                edge = 1.0f;
            const float smoothEdge = edge * edge * (3.0f - 2.0f * edge); // smoothstep
            const float alpha = 1.0f - smoothEdge;

            uint8_t* px = &pixels[(y * kDim + x) * 4];
            px[0] = 0; // R -- unused; the caller drives ObjectColor.rgb to 0,
            px[1] = 0; // G    so texColor.rgb is multiplied out regardless.
            px[2] = 0; // B
            px[3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
        }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = kDim;
    desc.Height = kDim;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = pixels;
    data.SysMemPitch = kDim * 4;

    if (FAILED(m_device->CreateTexture2D(&desc, &data, &m_blobShadowTexture)))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create blob-shadow circle texture", L"ERROR");
        return nullptr;
    }
    if (FAILED(m_device->CreateShaderResourceView(m_blobShadowTexture.Get(), nullptr, &m_blobShadowSRV)))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create blob-shadow circle SRV", L"ERROR");
        return nullptr;
    }
    return m_blobShadowSRV.Get();
}

ID3D11ShaderResourceView* GraphicsEngine::GetOrLoadTextureSRV(const std::string& path)
{
    if (path.empty() || !m_device || !m_context)
        return nullptr;

    auto it = m_basicTextureCache.find(path);
    if (it != m_basicTextureCache.end())
        return it->second.Get();

    // Insert negative-cache entry up front so repeated failures don't re-hit disk
    ComPtr<ID3D11ShaderResourceView>& slot = m_basicTextureCache[path];

    // WIC decode to RGBA8
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        return nullptr;

    std::wstring wpath(path.begin(), path.end());
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                            &decoder);
    if (FAILED(hr))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "GetOrLoadTextureSRV: cannot open '%s' (HR=0x%08lX)", path.c_str(),
                       static_cast<long>(hr));
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return nullptr;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)))
        return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0f,
                                     WICBitmapPaletteTypeMedianCut)))
        return nullptr;

    UINT width = 0, height = 0;
    frame->GetSize(&width, &height);
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
        return nullptr;

    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return nullptr;

    // Create with a full mip chain and auto-generate mips (the basic sampler
    // is trilinear; without mips a 64x-tiled terrain shimmers badly).
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 0; // full chain
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    ComPtr<ID3D11Texture2D> tex;
    hr = m_device->CreateTexture2D(&desc, nullptr, &tex);
    if (FAILED(hr))
        return nullptr;

    m_context->UpdateSubresource(tex.Get(), 0, nullptr, pixels.data(), width * 4, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = static_cast<UINT>(-1);

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = m_device->CreateShaderResourceView(tex.Get(), &srvDesc, &srv);
    if (FAILED(hr))
        return nullptr;

    m_context->GenerateMips(srv.Get());

    slot = srv;
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "GetOrLoadTextureSRV: loaded '%s' (%ux%u)", path.c_str(), width,
                   height);
    return slot.Get();
}

HRESULT GraphicsEngine::CreateDefaultTexture()
{
    // Create a 1x1 white texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // White pixel data
    UINT32 whitePixel = 0xFFFFFFFF;
    D3D11_SUBRESOURCE_DATA texData = {};
    texData.pSysMem = &whitePixel;
    texData.SysMemPitch = sizeof(UINT32);

    HRESULT hr = m_device->CreateTexture2D(&texDesc, &texData, &m_defaultTexture);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default texture", L"ERROR");
        return hr;
    }

    // Create shader resource view
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = m_device->CreateShaderResourceView(m_defaultTexture.Get(), &srvDesc, &m_defaultSRV);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default texture SRV", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Default white texture created successfully", L"SUCCESS");
    return S_OK;
}
#endif // SPARK_PLATFORM_WINDOWS
