/**
 * @file GraphicsDeviceResources.cpp
 * @brief Device creation, render targets, render states, and shader system for GraphicsEngine
 *
 * Contains D3D11 device/swap chain creation, render target and depth stencil creation,
 * render state objects, pipeline setup (deferred/forward+), and the basic shader system
 * (compilation, constant buffers, embedded shaders). Split from GraphicsEngine.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "Shader.h"
#include "LightingSystem.h"
#include "MaterialSystem.h"
#include "../Utils/LogMacros.h"

#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <d3dcompiler.h>

#include <string>
#include <cstring>

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
    D3D11_TEXTURE2D_DESC depthStencilDesc = {};
    depthStencilDesc.Width = m_windowWidth;
    depthStencilDesc.Height = m_windowHeight;
    depthStencilDesc.MipLevels = 1;
    depthStencilDesc.ArraySize = 1;
    depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthStencilDesc.SampleDesc.Count = 1;
    depthStencilDesc.SampleDesc.Quality = 0;
    depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
    depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depthStencilDesc.CPUAccessFlags = 0;
    depthStencilDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> depthStencilTexture;
    HRESULT hr = m_device->CreateTexture2D(&depthStencilDesc, nullptr, &depthStencilTexture);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create depth stencil texture", L"ERROR");
        return hr;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = {};
    depthStencilViewDesc.Format = depthStencilDesc.Format;
    depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    depthStencilViewDesc.Texture2D.MipSlice = 0;

    hr = m_device->CreateDepthStencilView(depthStencilTexture.Get(), &depthStencilViewDesc, &m_depthStencilView);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create depth stencil view", L"ERROR");
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

    // Set up basic directional lighting
    frameConstants.DirectionalLightDir = XMFLOAT3(0.3f, -0.7f, 0.6f); // Pointing down and slightly forward
    frameConstants.DirectionalLightIntensity = 1.0f;
    frameConstants.DirectionalLightColor = XMFLOAT3(1.0f, 1.0f, 0.9f); // Slightly warm white
    frameConstants.AmbientIntensity = 0.3f;
    frameConstants.AmbientColor = XMFLOAT3(0.2f, 0.3f, 0.4f); // Cool ambient

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_context->Map(m_basicFrameConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr))
    {
        memcpy(mappedResource.pData, &frameConstants, sizeof(PerFrameConstants));
        m_context->Unmap(m_basicFrameConstantBuffer.Get(), 0);
    }
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
            std::string errorMsg = (char*)errorBlob->GetBufferPointer();
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
            std::string errorMsg = (char*)errorBlob->GetBufferPointer();
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

        Texture2D MainTexture : register(t0);
        SamplerState MainSampler : register(s0);

        struct PixelInput
        {
            float4 Position     : SV_POSITION;
            float3 WorldPos     : POSITION;
            float3 Normal       : NORMAL;
            float2 TexCoord     : TEXCOORD0;
            float4 Color        : COLOR;
        };

        float4 main(PixelInput input) : SV_TARGET
        {
            float4 texColor = MainTexture.Sample(MainSampler, input.TexCoord);

            float3 normal = normalize(input.Normal);
            float3 lightDir = normalize(-DirectionalLightDir);
            float NdotL = max(0.0f, dot(normal, lightDir));

            float3 diffuse = DirectionalLightColor * DirectionalLightIntensity * NdotL;
            float3 ambient = AmbientColor * AmbientIntensity;
            float3 lighting = diffuse + ambient;

            float4 finalColor = texColor * input.Color;
            finalColor.rgb *= lighting;
            finalColor.a = texColor.a * input.Color.a;

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
            std::string errorMsg = (char*)errorBlob->GetBufferPointer();
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Embedded pixel shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Embedded pixel shader compiled successfully", L"SUCCESS");
    return S_OK;
}

#else // !SPARK_PLATFORM_WINDOWS

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
    Spark::RHI::IRHITexture* hdrTarget =
        rhi.bridge.CreateRenderTarget(rhi.width, rhi.height, Spark::RHI::PixelFormat::R16G16B16A16_FLOAT);
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
        Spark::RHI::IRHITexture* gBufferRT = rhi.bridge.CreateRenderTarget(rhi.width, rhi.height, format);
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
    rhi.bridge.CreateDepthBuffer(rhi.width, rhi.height);
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
    Spark::RHI::IRHIBuffer* cb = rhi.bridge.CreateConstantBuffer(CB_SIZE);
    if (!cb)
        return E_FAIL;

    constexpr uint64_t FRAME_CB_SIZE = 256;
    Spark::RHI::IRHIBuffer* frameCB = rhi.bridge.CreateConstantBuffer(FRAME_CB_SIZE);
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
    Spark::RHI::IRHITexture* defaultTex = rhi.bridge.CreateTexture2D(
        1, 1, Spark::RHI::PixelFormat::R8G8B8A8_UNORM, Spark::RHI::RHITextureUsage::ShaderResource, &whitePixel);

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
        device->CreateShader(desc);
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
        device->CreateShader(desc);
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

#endif // SPARK_PLATFORM_WINDOWS
