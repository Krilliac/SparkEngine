/**
 * @file GraphicsDeviceResourcesWindows.cpp
 * @brief D3D11 device creation, render targets, render states, and shader system
 *
 * Contains D3D11 device/swap chain creation, render target and depth stencil creation,
 * render state objects, pipeline setup, and the basic shader system.
 * Linux counterpart lives in GraphicsDeviceResourcesLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "Shader.h"
#include "LightingSystem.h"
#include "MaterialSystem.h"
#include "Mesh.h" // W12 decor-instancing: DrawMeshInstanced binds Mesh buffers directly
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h> // XMConvertFloatToHalf for the fp16 1x1 material defaults
#include <wrl.h>
#include <d3dcompiler.h>

#include <string>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <wincodec.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ============================================================================
// DEVICE CREATION METHODS
// ============================================================================

HRESULT GraphicsEngine::CreateDeviceAndSwapChain(HWND hWnd)
{
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL featureLevel;
    ComPtr<ID3D11Device> baseDevice;
    ComPtr<ID3D11DeviceContext> baseContext;

    SPARK_LOG_INFO("Graphics", "Creating D3D11 device (flags=0x%X)...", createDeviceFlags);

    HRESULT hr =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevels,
                          ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &baseDevice, &featureLevel, &baseContext);

    if (FAILED(hr))
    {
        SPARK_LOG_FATAL("Graphics",
                        "D3D11CreateDevice failed with HR=0x%08lX. "
                        "Check GPU driver installation and DirectX 11 support.",
                        static_cast<long>(hr));
        return hr;
    }

    // Log the feature level we got
    const char* featureLevelStr = "Unknown";
    switch (featureLevel)
    {
    case D3D_FEATURE_LEVEL_11_1:
        featureLevelStr = "11.1";
        break;
    case D3D_FEATURE_LEVEL_11_0:
        featureLevelStr = "11.0";
        break;
    case D3D_FEATURE_LEVEL_10_1:
        featureLevelStr = "10.1";
        break;
    case D3D_FEATURE_LEVEL_10_0:
        featureLevelStr = "10.0";
        break;
    default:
        break;
    }
    SPARK_LOG_INFO("Graphics", "D3D11 device created -- Feature Level %s", featureLevelStr);

    // Query for ID3D11Device1 interface
    hr = baseDevice.As(&m_device);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to query ID3D11Device1", L"ERROR");
        return hr;
    }

    hr = baseContext.As(&m_context);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to query ID3D11DeviceContext1", L"ERROR");
        return hr;
    }

    // Create DXGI factory
    ComPtr<IDXGIFactory1> dxgiFactory;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"CreateDXGIFactory1 failed", L"ERROR");
        return hr;
    }

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 1;
    swapChainDesc.BufferDesc.Width = m_windowWidth;
    swapChainDesc.BufferDesc.Height = m_windowHeight;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = hWnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> tempSwapChain;
    hr = dxgiFactory->CreateSwapChain(m_device.Get(), &swapChainDesc, &tempSwapChain);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"CreateSwapChain failed", L"ERROR");
        return hr;
    }

    hr = tempSwapChain.As(&m_swapChain);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to query IDXGISwapChain1", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"DirectX device and swap chain created successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CreateRenderTargetView()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to get back buffer", L"ERROR");
        return hr;
    }

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTargetView);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create render target view", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Render target view created successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CreateDepthStencilView()
{
    // Use typeless format so we can create both DSV and SRV views.
    // The SRV is needed by GPUDrivenRenderer for HiZ mip chain construction.
    D3D11_TEXTURE2D_DESC depthStencilDesc = {};
    depthStencilDesc.Width = m_windowWidth;
    depthStencilDesc.Height = m_windowHeight;
    depthStencilDesc.MipLevels = 1;
    depthStencilDesc.ArraySize = 1;
    depthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depthStencilDesc.SampleDesc.Count = 1;
    depthStencilDesc.SampleDesc.Quality = 0;
    depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
    depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    depthStencilDesc.CPUAccessFlags = 0;
    depthStencilDesc.MiscFlags = 0;

    HRESULT hr = m_device->CreateTexture2D(&depthStencilDesc, nullptr, &m_depthStencilTexture);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create depth stencil texture", L"ERROR");
        return hr;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = {};
    depthStencilViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    depthStencilViewDesc.Texture2D.MipSlice = 0;

    hr = m_device->CreateDepthStencilView(m_depthStencilTexture.Get(), &depthStencilViewDesc, &m_depthStencilView);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create depth stencil view", L"ERROR");
        return hr;
    }

    // Create SRV for depth reads (HiZ construction, post-processing)
    D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc = {};
    depthSRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depthSRVDesc.Texture2D.MipLevels = 1;
    depthSRVDesc.Texture2D.MostDetailedMip = 0;

    hr = m_device->CreateShaderResourceView(m_depthStencilTexture.Get(), &depthSRVDesc, &m_depthStencilSRV);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create depth stencil SRV", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Depth stencil view created successfully", L"SUCCESS");
    return S_OK;
}

void GraphicsEngine::SetViewport()
{
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_windowWidth);
    viewport.Height = static_cast<float>(m_windowHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    m_context->RSSetViewports(1, &viewport);
}

// ============================================================================
// ADVANCED RENDER TARGET CREATION
// ============================================================================

HRESULT GraphicsEngine::CreateAdvancedRenderTargets()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Creating advanced render targets", L"INFO");

    if (m_hdrEnabled)
    {
        D3D11_TEXTURE2D_DESC hdrDesc = {};
        hdrDesc.Width = m_windowWidth;
        hdrDesc.Height = m_windowHeight;
        hdrDesc.MipLevels = 1;
        hdrDesc.ArraySize = 1;
        hdrDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hdrDesc.SampleDesc.Count = m_settings.msaaSamples;
        hdrDesc.SampleDesc.Quality = 0;
        hdrDesc.Usage = D3D11_USAGE_DEFAULT;
        hdrDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hdrDesc.CPUAccessFlags = 0;
        hdrDesc.MiscFlags = 0;

        HRESULT hr = m_device->CreateTexture2D(&hdrDesc, nullptr, &m_hdrTexture);
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create HDR texture", L"ERROR");
            return hr;
        }

        hr = m_device->CreateRenderTargetView(m_hdrTexture.Get(), nullptr, &m_hdrRTV);
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create HDR RTV", L"ERROR");
            return hr;
        }

        hr = m_device->CreateShaderResourceView(m_hdrTexture.Get(), nullptr, &m_hdrSRV);
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create HDR SRV", L"ERROR");
            return hr;
        }

        LOG_TO_CONSOLE_IMMEDIATE(L"HDR render targets created successfully", L"SUCCESS");
    }

    if (m_currentPipeline == RenderingPipeline::Deferred)
    {
        DXGI_FORMAT gBufferFormats[4] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                                         DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT};

        for (int i = 0; i < 4; i++)
        {
            D3D11_TEXTURE2D_DESC gBufferDesc = {};
            gBufferDesc.Width = m_windowWidth;
            gBufferDesc.Height = m_windowHeight;
            gBufferDesc.MipLevels = 1;
            gBufferDesc.ArraySize = 1;
            gBufferDesc.Format = gBufferFormats[i];
            gBufferDesc.SampleDesc.Count = m_settings.msaaSamples;
            gBufferDesc.SampleDesc.Quality = 0;
            gBufferDesc.Usage = D3D11_USAGE_DEFAULT;
            gBufferDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            gBufferDesc.CPUAccessFlags = 0;
            gBufferDesc.MiscFlags = 0;

            HRESULT hr = m_device->CreateTexture2D(&gBufferDesc, nullptr, &m_gBufferTextures[i]);
            if (FAILED(hr))
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create G-Buffer texture " + std::to_wstring(i), L"ERROR");
                return hr;
            }

            hr = m_device->CreateRenderTargetView(m_gBufferTextures[i].Get(), nullptr, &m_gBufferRTVs[i]);
            if (FAILED(hr))
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create G-Buffer RTV " + std::to_wstring(i), L"ERROR");
                return hr;
            }

            hr = m_device->CreateShaderResourceView(m_gBufferTextures[i].Get(), nullptr, &m_gBufferSRVs[i]);
            if (FAILED(hr))
            {
                LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create G-Buffer SRV " + std::to_wstring(i), L"ERROR");
                return hr;
            }
        }

        LOG_TO_CONSOLE_IMMEDIATE(L"G-Buffer render targets created successfully", L"SUCCESS");
    }

    return S_OK;
}

HRESULT GraphicsEngine::CreateRenderStates()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Creating advanced render states", L"INFO");

    // Rasterizer states (solid + wireframe). Shared between the windowed
    // Initialize(hWnd) path and the device-attach InitializeFromDevice() path
    // so ApplyGraphicsState()/ApplyBasicRenderStates() have real state objects
    // to bind in BOTH modes instead of only when a swapchain exists.
    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_BACK;
    rastDesc.FrontCounterClockwise = FALSE;
    rastDesc.DepthBias = 0;
    rastDesc.DepthBiasClamp = 0.0f;
    rastDesc.SlopeScaledDepthBias = 0.0f;
    rastDesc.DepthClipEnable = TRUE;
    rastDesc.ScissorEnable = FALSE;
    rastDesc.MultisampleEnable = FALSE;
    rastDesc.AntialiasedLineEnable = FALSE;

    HRESULT hrRast = m_device->CreateRasterizerState(&rastDesc, &m_solidRasterState);
    if (FAILED(hrRast))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create solid rasterizer state", L"ERROR");
        return hrRast;
    }

    rastDesc.FillMode = D3D11_FILL_WIREFRAME;
    hrRast = m_device->CreateRasterizerState(&rastDesc, &m_wireframeRasterState);
    if (FAILED(hrRast))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create wireframe rasterizer state", L"ERROR");
        return hrRast;
    }

    D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;
    depthStencilDesc.StencilEnable = FALSE;

    HRESULT hr = m_device->CreateDepthStencilState(&depthStencilDesc, &m_defaultDepthState);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default depth state", L"ERROR");
        return hr;
    }

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = m_device->CreateBlendState(&blendDesc, &m_defaultBlendState);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default blend state", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Advanced render states created successfully", L"SUCCESS");
    return S_OK;
}

// ============================================================================
// PIPELINE SETUP METHODS
// ============================================================================

void GraphicsEngine::SetupDeferredPipeline()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Setting up deferred rendering pipeline", L"INFO");
    m_currentPipeline = RenderingPipeline::Deferred;

    // Create G-Buffer render targets if not already created
    if (!m_gBufferTextures[0])
    {
        HRESULT hr = CreateAdvancedRenderTargets();
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create G-Buffer render targets", L"ERROR");
        }
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Deferred pipeline setup complete", L"SUCCESS");
}

void GraphicsEngine::SetupForwardPlusPipeline()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Setting up Forward+ rendering pipeline", L"INFO");

    uint32_t tileSize = 16;
    uint32_t tilesX = (m_windowWidth + tileSize - 1) / tileSize;
    uint32_t tilesY = (m_windowHeight + tileSize - 1) / tileSize;

    LOG_TO_CONSOLE_IMMEDIATE(L"Forward+ pipeline setup: " + std::to_wstring(tilesX) + L"x" + std::to_wstring(tilesY) +
                                 L" tiles (" + std::to_wstring(tileSize) + L"x" + std::to_wstring(tileSize) + L" each)",
                             L"INFO");

    m_currentPipeline = RenderingPipeline::ForwardPlus;
    LOG_TO_CONSOLE_IMMEDIATE(L"Forward+ pipeline setup complete", L"SUCCESS");
}

// ============================================================================
// BASIC SHADER SYSTEM IMPLEMENTATION
// ============================================================================

HRESULT GraphicsEngine::InitializeBasicShaders()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Initializing basic shader system", L"INFO");

    // Create constant buffer first
    HRESULT hr = CreateBasicConstantBuffer();
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create basic constant buffer", L"ERROR");
        return hr;
    }

    // Try to compile from file first, then fall back to embedded shaders
    ComPtr<ID3DBlob> vsBlob;
    hr = CompileShaderFromFile(L"Shaders/HLSL/BasicVertex.hlsl", "main", "vs_5_0", &vsBlob);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Falling back to embedded vertex shader", L"WARNING");
        hr = CompileEmbeddedVertexShader(&vsBlob);
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to compile embedded vertex shader", L"ERROR");
            return hr;
        }
    }

    // Create vertex shader
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                      &m_basicVertexShader);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create vertex shader", L"ERROR");
        return hr;
    }

    // Create input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}};

    hr = m_device->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                     &m_basicInputLayout);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create input layout", L"ERROR");
        return hr;
    }

    // Try to compile pixel shader from file, then fall back to embedded
    ComPtr<ID3DBlob> psBlob;
    hr = CompileShaderFromFile(L"Shaders/HLSL/BasicPixel.hlsl", "main", "ps_5_0", &psBlob);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Falling back to embedded pixel shader", L"WARNING");
        hr = CompileEmbeddedPixelShader(&psBlob);
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to compile embedded pixel shader", L"ERROR");
            return hr;
        }
    }

    // Create pixel shader
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_basicPixelShader);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create pixel shader", L"ERROR");
        return hr;
    }

    // Create basic sampler state
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = m_device->CreateSamplerState(&samplerDesc, &m_basicSamplerState);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create sampler state", L"ERROR");
        return hr;
    }

    // Create default 1x1 white texture
    hr = CreateDefaultTexture();
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default texture", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Basic shader system initialized successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CreateBasicConstantBuffer()
{
    // Create constant buffer for per-object rendering constants
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.ByteWidth = sizeof(PerObjectConstants);
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bufferDesc.MiscFlags = 0;

    HRESULT hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_basicConstantBuffer);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create basic constant buffer", L"ERROR");
        return hr;
    }

    // Create constant buffer for per-frame constants
    bufferDesc.ByteWidth = sizeof(PerFrameConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_basicFrameConstantBuffer);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create frame constant buffer", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Basic constant buffers created successfully", L"SUCCESS");
    return S_OK;
}

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

HRESULT GraphicsEngine::CompileShaderFromFile(const std::wstring& filename, const char* entryPoint,
                                              const char* shaderModel, ID3DBlob** blobOut)
{
    HRESULT hr = S_OK;

    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    hr = D3DCompileFromFile(filename.c_str(), nullptr, nullptr, entryPoint, shaderModel, shaderFlags, 0, blobOut,
                            &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errorMsg(static_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }

    return S_OK;
}

HRESULT GraphicsEngine::CompileEmbeddedVertexShader(ID3DBlob** blobOut)
{
    // Embedded vertex shader source code
    const char* vertexShaderSource = R"(
        cbuffer PerObjectConstants : register(b0)
        {
            matrix World;
            matrix WorldViewProjection;
            matrix WorldInverseTranspose;
            // MUST mirror Shader.h PerObjectConstants exactly. This field was
            // missing, shifting every following member by 64 bytes — the GPU
            // read ObjectColor from a zeroed matrix region, so every basic-
            // shader draw came out rgba(0,0,0,0): the black-screen bug.
            matrix PreviousWorld;
            float3 ObjectPosition;
            float ObjectScale;
            float4 ObjectColor;
            float4 MaterialProperties;
            float4 UVTiling;
        };

        struct VertexInput
        {
            float3 Position : POSITION;
            float3 Normal   : NORMAL;
            float2 TexCoord : TEXCOORD0;
        };

        struct VertexOutput
        {
            float4 Position     : SV_POSITION;
            float3 WorldPos     : POSITION;
            float3 Normal       : NORMAL;
            float2 TexCoord     : TEXCOORD0;
            float4 Color        : COLOR;
        };

        VertexOutput main(VertexInput input)
        {
            VertexOutput output = (VertexOutput)0;

            output.Position = mul(float4(input.Position, 1.0f), WorldViewProjection);
            output.WorldPos = mul(float4(input.Position, 1.0f), World).xyz;
            output.Normal = normalize(mul(input.Normal, (float3x3)WorldInverseTranspose));
            output.TexCoord = input.TexCoord * UVTiling.xy + UVTiling.zw;
            output.Color = ObjectColor;

            return output;
        }
    )";

    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(vertexShaderSource, strlen(vertexShaderSource), "EmbeddedVertexShader", nullptr, nullptr,
                            "main", "vs_5_0", shaderFlags, 0, blobOut, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errorMsg(static_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Embedded vertex shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Embedded vertex shader compiled successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CompileEmbeddedPixelShader(ID3DBlob** blobOut)
{
    // Embedded pixel shader source code
    const char* pixelShaderSource = R"(
        cbuffer PerFrameConstants : register(b1)
        {
            matrix ViewMatrix;
            matrix ProjectionMatrix;
            matrix ViewProjectionMatrix;
            float3 CameraPosition;
            float Time;
            float3 CameraDirection;
            float DeltaTime;
            float2 ScreenResolution;
            float2 InvScreenResolution;

            float3 DirectionalLightDir;
            float DirectionalLightIntensity;
            float3 DirectionalLightColor;
            float AmbientIntensity;
            float3 AmbientColor;
            float _padding1;
        };

        // Per-object slot b0 (bound to the PS in SetBasicShaders). Mirrors the
        // vertex shader's layout; the PS only needs MaterialProperties
        // (z: emissive, w: alpha), but the whole cbuffer must be declared so the
        // field offsets line up with the constant buffer.
        cbuffer PerObjectConstants : register(b0)
        {
            matrix World;
            matrix WorldViewProjection;
            matrix WorldInverseTranspose;
            matrix PreviousWorld;
            float3 ObjectPosition;
            float  ObjectScale;
            float4 ObjectColor;
            float4 MaterialProperties; // x: metallic, y: roughness, z: emissive, w: alpha
            float4 UVTiling;
        };

        Texture2D MainTexture : register(t0);
        // Normal (t1) + roughness (t2) maps for the tangent-less normal-mapping
        // path. SetBasicShaders() default-binds a flat FLOAT normal (0.5,0.5,1)
        // and a fully-rough (1.0) roughness, which make both new terms exact
        // no-ops — see CotangentFrame() and the specular block below.
        Texture2D NormalTexture : register(t1);
        Texture2D RoughnessTexture : register(t2);
        SamplerState MainSampler : register(s0);

        struct PixelInput
        {
            float4 Position     : SV_POSITION;
            float3 WorldPos     : POSITION;
            float3 Normal       : NORMAL;
            float2 TexCoord     : TEXCOORD0;
            float4 Color        : COLOR;
        };

        // Screen-space cotangent frame (Schuler, "Normal Mapping Without
        // Precomputed Tangents" / Mikkelsen's derivative method). The vertex
        // format has no tangents, so we rebuild the (T, B, N) frame per pixel:
        // ddx/ddy of the world position give two in-triangle edge vectors,
        // ddx/ddy of the UVs give the same edges in texture space, and solving
        // that 2x2 system (via the cross-product co-vectors below) yields the
        // directions in which u and v increase across the surface.
        float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
        {
            float3 dp1 = ddx(p);
            float3 dp2 = ddy(p);
            float2 duv1 = ddx(uv);
            float2 duv2 = ddy(uv);

            // Co-vectors perpendicular to N: projecting the edge vectors
            // through these solves for T (du direction) and B (dv direction)
            // while keeping both in the surface plane.
            float3 dp2perp = cross(dp2, N);
            float3 dp1perp = cross(N, dp1);
            float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
            float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

            // Scale-invariant normalization (max keeps the T:B aspect so
            // mirrored/anisotropic UVs stay correct). Guard degenerate UVs:
            // constant texcoords across the pixel quad make T = B = 0, and
            // rsqrt(0) = INF would turn the frame into NaNs; forcing 0 makes
            // the tangential components drop out so N passes through instead.
            float maxLen2 = max(dot(T, T), dot(B, B));
            float invmax = (maxLen2 > 1e-10f) ? rsqrt(maxLen2) : 0.0f;
            return float3x3(T * invmax, B * invmax, N);
        }

        float4 main(PixelInput input) : SV_TARGET
        {
            float4 texColor = MainTexture.Sample(MainSampler, input.TexCoord);

            // Perturb the geometric normal with the t1 normal map.
            // Identity proof for the default binding: the default texture is
            // FLOAT-format (0.5, 0.5, 1), so nTS = sample*2-1 == (0, 0, 1)
            // EXACTLY (an 8-bit 128/255 texel would leave +0.0039 in x/y).
            // mul(rowVector, matrix) = x*T + y*B + z*N, so (0,0,1) selects the
            // third row: geoN, already unit length -- the lighting below sees
            // the same normalize(input.Normal) the pre-normal-map shader used.
            float3 geoN = normalize(input.Normal);
            float3 nTS = NormalTexture.Sample(MainSampler, input.TexCoord).xyz * 2.0f - 1.0f;
            float3x3 TBN = CotangentFrame(geoN, input.WorldPos, input.TexCoord);
            float3 normal = normalize(mul(nTS, TBN));

            float3 lightDir = normalize(-DirectionalLightDir);
            float NdotL = max(0.0f, dot(normal, lightDir));

            float3 diffuse = DirectionalLightColor * DirectionalLightIntensity * NdotL;
            float3 ambient = AmbientColor * AmbientIntensity;

            // Soft camera-facing fill (headlight): surfaces turned toward the
            // viewer get a little extra warm light so they read with detail
            // instead of crushing to black when they face away from the sun.
            float3 viewDir = normalize(CameraPosition - input.WorldPos);
            float fill = 0.35f * max(0.0f, dot(normal, viewDir));

            float3 lighting = diffuse + ambient + float3(fill, fill * 0.95f, fill * 0.88f);

            // Modest Blinn-Phong specular scaled by (1 - roughness). The
            // default t2 binding is fully rough (1.0), so gloss == 0 and this
            // whole term is EXACTLY zero for draws that never bind a roughness
            // map -- specular is opt-in per material. The saturate(NdotL*4)
            // fade kills the highlight on the unlit side without a hard edge.
            float roughness = saturate(RoughnessTexture.Sample(MainSampler, input.TexCoord).r);
            float gloss = 1.0f - roughness;
            float3 halfVec = normalize(lightDir + viewDir);
            float NdotH = max(0.0f, dot(normal, halfVec));
            float specPower = lerp(8.0f, 96.0f, gloss); // rough -> broad sheen, smooth -> tight highlight
            float3 specular = DirectionalLightColor * DirectionalLightIntensity *
                              pow(NdotH, specPower) * gloss * 0.5f * saturate(NdotL * 4.0f);

            float4 finalColor = texColor * input.Color;
            finalColor.rgb *= lighting;
            finalColor.rgb += specular; // dielectric-style: not tinted by albedo

            // Emissive term (MaterialProperties.z, default 0 == no change): adds
            // the surface color back UNLIT so glow strips / holo panels / muzzle
            // flashes read as light sources even in shadow. Backward-compatible.
            finalColor.rgb += texColor.rgb * input.Color.rgb * MaterialProperties.z;

            // Alpha (MaterialProperties.w, default 1): lets materials fade when a
            // blend state is active; opaque draws keep w==1 so this is a no-op.
            finalColor.a = texColor.a * input.Color.a * MaterialProperties.w;

            return finalColor;
        }
    )";

    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(pixelShaderSource, strlen(pixelShaderSource), "EmbeddedPixelShader", nullptr, nullptr,
                            "main", "ps_5_0", shaderFlags, 0, blobOut, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errorMsg(static_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Embedded pixel shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Embedded pixel shader compiled successfully", L"SUCCESS");
    return S_OK;
}

// ============================================================================
// BASIC INSTANCED DRAW PATH (W12 decor-instancing)
// ============================================================================
// A SECOND permutation of the embedded basic vertex shader that reads the
// world matrix from a per-instance vertex stream (IA slot 1) instead of the
// b0 cbuffer, letting repeated static meshes (region decor) collapse N draw
// calls into one DrawIndexedInstanced. Everything here is additive and lazy:
// the existing VS / input layout / SetBasicShaders() path is untouched, the
// pipeline objects are only created on the first HasInstancedBasicPipeline()
// probe, and a compile failure permanently reports "unavailable" so callers
// fall back to the per-entity path.
//
// SHADER MATH (identical to the non-instanced path by construction):
//  - Every cbuffer matrix is uploaded TRANSPOSED (XMMatrixTranspose) and
//    consumed as mul(rowVector, M) — the effective transform is v * W * V * P
//    in row-vector convention.
//  - The instance stream carries the world matrix UNTRANSPOSED: XMFLOAT4X4
//    row-major memory rows map 1:1 onto the four INSTWORLD float4 attributes,
//    and HLSL float4x4(r0, r1, r2, r3) builds its ROWS from them, so
//    mul(v, instWorld) == v * W — the same product the non-instanced VS gets
//    from its transposed cbuffer World. Clip position then applies the b1
//    ViewProjectionMatrix: (v * W) * (V * P), associativity-identical to the
//    non-instanced v * (W * V * P) WorldViewProjection.
//  - Normals use (float3x3)instWorld + normalize instead of the cbuffer
//    WorldInverseTranspose. For rotation + translation + UNIFORM scale
//    worlds, inverse-transpose(R) == R and the scale factor is normalized
//    away, so the lit result is identical (decor worlds are yaw+translation
//    only). NON-uniform scale would skew normals — callers must not use this
//    path for non-uniformly scaled instances.
//  - WorldPos, TexCoord (UVTiling), Color (ObjectColor) and the shared pixel
//    shader all read the same inputs as the non-instanced path, so lighting
//    is bit-comparable.

namespace
{
    /// Hard cap on instances per DrawIndexedInstanced upload (larger requests
    /// are split into chunks). 4096 * 64 B = 256 KB dynamic buffer worst case.
    constexpr uint32_t kMaxBasicInstances = 4096u;
    /// First allocation; grown geometrically (doubling) up to the cap.
    constexpr uint32_t kInitialBasicInstances = 256u;
} // namespace

HRESULT GraphicsEngine::CompileEmbeddedVertexShaderInstanced(ID3DBlob** blobOut)
{
    // Mirrors CompileEmbeddedVertexShader with ONE change: the world matrix
    // comes from the per-instance INSTWORLD attributes; b0's World/WVP/WIT
    // are ignored (ObjectColor/UVTiling are still read from b0, and the
    // shared pixel shader still reads MaterialProperties from b0).
    const char* vertexShaderSource = R"(
        cbuffer PerObjectConstants : register(b0)
        {
            matrix World;                   // unused by the instanced path
            matrix WorldViewProjection;     // unused by the instanced path
            matrix WorldInverseTranspose;   // unused by the instanced path
            matrix PreviousWorld;
            float3 ObjectPosition;
            float ObjectScale;
            float4 ObjectColor;
            float4 MaterialProperties;
            float4 UVTiling;
        };

        // Per-frame slot b1 — the instanced path takes view*proj from here
        // (the per-object WVP cannot exist when the world matrix varies per
        // instance). Must mirror Shader.h PerFrameConstants exactly.
        cbuffer PerFrameConstants : register(b1)
        {
            matrix ViewMatrix;
            matrix ProjectionMatrix;
            matrix ViewProjectionMatrix;
            float3 CameraPosition;
            float Time;
            float3 CameraDirection;
            float DeltaTime;
            float2 ScreenResolution;
            float2 InvScreenResolution;

            float3 DirectionalLightDir;
            float DirectionalLightIntensity;
            float3 DirectionalLightColor;
            float AmbientIntensity;
            float3 AmbientColor;
            float _padding1;
        };

        struct VertexInput
        {
            float3 Position : POSITION;
            float3 Normal   : NORMAL;
            float2 TexCoord : TEXCOORD0;
            // Per-instance world matrix rows (slot 1, step rate 1), uploaded
            // UNTRANSPOSED so float4x4(rows...) reproduces the XMMATRIX.
            float4 InstW0   : INSTWORLD0;
            float4 InstW1   : INSTWORLD1;
            float4 InstW2   : INSTWORLD2;
            float4 InstW3   : INSTWORLD3;
        };

        struct VertexOutput
        {
            float4 Position     : SV_POSITION;
            float3 WorldPos     : POSITION;
            float3 Normal       : NORMAL;
            float2 TexCoord     : TEXCOORD0;
            float4 Color        : COLOR;
        };

        VertexOutput main(VertexInput input)
        {
            VertexOutput output = (VertexOutput)0;

            float4x4 instWorld = float4x4(input.InstW0, input.InstW1, input.InstW2, input.InstW3);
            float4 worldPos = mul(float4(input.Position, 1.0f), instWorld); // v * W (row-vector)
            output.Position = mul(worldPos, ViewProjectionMatrix);          // (v*W) * (V*P)
            output.WorldPos = worldPos.xyz;
            // Valid for rotation + translation + uniform scale (see the
            // section header): inverse-transpose == the matrix itself for
            // the rotation part, and normalize() removes uniform scale.
            output.Normal = normalize(mul(input.Normal, (float3x3)instWorld));
            output.TexCoord = input.TexCoord * UVTiling.xy + UVTiling.zw;
            output.Color = ObjectColor;

            return output;
        }
    )";

    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(vertexShaderSource, strlen(vertexShaderSource), "EmbeddedVertexShaderInstanced", nullptr,
                            nullptr, "main", "vs_5_0", shaderFlags, 0, blobOut, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errorMsg(static_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Embedded instanced vertex shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Embedded instanced vertex shader compiled successfully", L"SUCCESS");
    return S_OK;
}

bool GraphicsEngine::EnsureBasicInstancedPipeline()
{
    if (m_basicVertexShaderInstanced && m_basicInputLayoutInstanced)
        return true;
    if (m_basicInstancedTried) // one attempt per device: fail = permanently off
        return false;
    m_basicInstancedTried = true;

    // The instanced permutation piggybacks on the basic system (shared PS,
    // cbuffers, sampler, defaults) — require it to be initialized first.
    if (!m_device || !m_basicPixelShader || !m_basicConstantBuffer || !m_basicFrameConstantBuffer)
        return false;

    ComPtr<ID3DBlob> vsBlob;
    if (FAILED(CompileEmbeddedVertexShaderInstanced(&vsBlob)))
        return false;

    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                            &m_basicVertexShaderInstanced)))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create instanced vertex shader", L"ERROR");
        return false;
    }

    // Slot 0: the exact per-vertex layout the basic path uses. Slot 1: the
    // per-instance world matrix as four float4 rows, step rate 1.
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INSTWORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTWORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTWORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTWORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };

    if (FAILED(m_device->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(),
                                           vsBlob->GetBufferSize(), &m_basicInputLayoutInstanced)))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create instanced input layout", L"ERROR");
        m_basicVertexShaderInstanced.Reset();
        return false;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Basic instanced pipeline created", L"SUCCESS");
    return true;
}

bool GraphicsEngine::HasInstancedBasicPipeline()
{
    return EnsureBasicInstancedPipeline();
}

void GraphicsEngine::SetBasicShadersInstanced()
{
    if (!m_context || !EnsureBasicInstancedPipeline())
        return;

    // Instanced VS + the SHARED basic pixel shader (no PS changes).
    m_context->VSSetShader(m_basicVertexShaderInstanced.Get(), nullptr, 0);
    m_context->PSSetShader(m_basicPixelShader.Get(), nullptr, 0);
    m_context->IASetInputLayout(m_basicInputLayoutInstanced.Get());

    // Identical resource bindings to SetBasicShaders(): per-object b0
    // (ObjectColor/UVTiling/MaterialProperties still per-GROUP via
    // UpdateBasicConstants), per-frame b1 (view*proj source), sampler and
    // texture defaults.
    m_context->VSSetConstantBuffers(0, 1, m_basicConstantBuffer.GetAddressOf());
    m_context->VSSetConstantBuffers(1, 1, m_basicFrameConstantBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_basicConstantBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(1, 1, m_basicFrameConstantBuffer.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_basicSamplerState.GetAddressOf());
    if (m_defaultSRV)
    {
        m_context->PSSetShaderResources(0, 1, m_defaultSRV.GetAddressOf());
    }
    SetBasicMaterialTextures(nullptr, nullptr);
}

bool GraphicsEngine::EnsureBasicInstanceCapacity(uint32_t instanceCount)
{
    if (!m_device || instanceCount == 0 || instanceCount > kMaxBasicInstances)
        return false;
    if (m_basicInstanceBuffer && m_basicInstanceCapacity >= instanceCount)
        return true;

    // Geometric growth, capped: 256 -> 512 -> ... -> 4096 matrices.
    uint32_t capacity = (m_basicInstanceCapacity > 0) ? m_basicInstanceCapacity : kInitialBasicInstances;
    while (capacity < instanceCount)
        capacity *= 2u;
    if (capacity > kMaxBasicInstances)
        capacity = kMaxBasicInstances;

    D3D11_BUFFER_DESC desc = {};
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.ByteWidth = capacity * static_cast<UINT>(sizeof(DirectX::XMFLOAT4X4));
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(m_device->CreateBuffer(&desc, nullptr, &buffer)))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to (re)create basic instance buffer", L"ERROR");
        return false;
    }
    m_basicInstanceBuffer = std::move(buffer);
    m_basicInstanceCapacity = capacity;
    return true;
}

bool GraphicsEngine::DrawMeshInstanced(Mesh& mesh, const DirectX::XMFLOAT4X4* instanceWorlds, uint32_t instanceCount,
                                       uint32_t indexStart, uint32_t indexCount)
{
    // Caller contract: SetBasicShadersInstanced() bound the pipeline and
    // UpdateBasicConstants(...) set the per-group b0 constants (the world
    // part of b0 is ignored — instance matrices replace it). Instance worlds
    // must be rotation+translation+uniform-scale (see the section header).
    if (!m_context || !instanceWorlds || instanceCount == 0)
        return false;
    if (!EnsureBasicInstancedPipeline())
        return false;

    ID3D11Buffer* vb = mesh.GetVertexBuffer();
    ID3D11Buffer* ib = mesh.GetIndexBuffer();
    if (!vb || !ib || mesh.GetIndexCount() == 0)
        return false;
    if (indexCount == 0) // 0 = whole mesh (RenderRange semantics otherwise)
    {
        indexStart = 0;
        indexCount = mesh.GetIndexCount();
    }

    // Upload + draw in chunks of kMaxBasicInstances (one chunk in practice —
    // decor groups are tens of instances; the cap bounds the dynamic buffer).
    for (uint32_t base = 0; base < instanceCount; base += kMaxBasicInstances)
    {
        const uint32_t remaining = instanceCount - base;
        const uint32_t n = (remaining < kMaxBasicInstances) ? remaining : kMaxBasicInstances;
        if (!EnsureBasicInstanceCapacity(n))
            return false;

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(m_basicInstanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        memcpy(mapped.pData, instanceWorlds + base, static_cast<size_t>(n) * sizeof(DirectX::XMFLOAT4X4));
        m_context->Unmap(m_basicInstanceBuffer.Get(), 0);

        // Same bindings Mesh::Render/RenderRange use for slot 0, plus the
        // instance stream at slot 1 (the non-instanced input layout never
        // references slot 1, so a stale binding there is inert).
        ID3D11Buffer* vbs[2] = {vb, m_basicInstanceBuffer.Get()};
        const UINT strides[2] = {sizeof(Vertex), sizeof(DirectX::XMFLOAT4X4)};
        const UINT offsets[2] = {0, 0};
        m_context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        m_context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->DrawIndexedInstanced(indexCount, n, indexStart, 0, 0);
    }
    return true;
}


#endif // SPARK_PLATFORM_WINDOWS
