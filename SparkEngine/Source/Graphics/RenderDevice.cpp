#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "RenderDevice.h"
#include "RHI/RHIFactory.h"
#include <d3d11.h>

namespace Spark::Graphics
{

    RenderDevice::~RenderDevice()
    {
        Shutdown();
    }

    bool RenderDevice::Initialize(Spark::NativeWindowHandle hwnd, uint32_t width, uint32_t height, bool fullscreen,
                                  RHI::GraphicsBackend backend)
    {
        m_width = width;
        m_height = height;
        m_fullscreen = fullscreen;
        m_backend = backend;

        // Select backend (Auto defaults to D3D11 on Windows)
        if (m_backend == RHI::GraphicsBackend::Auto)
        {
            m_backend = RHI::GraphicsBackend::D3D11;
        }

        // Create the D3D11 device and swap chain
        if (!CreateDevice(hwnd, width, height, fullscreen))
        {
            return false;
        }

        // Create back buffer render target view
        if (!CreateBackBufferViews())
        {
            return false;
        }

        // Initialize capabilities
        m_capabilities.maxTextureSize = 16384;
        m_capabilities.maxRenderTargets = 8;
        m_capabilities.computeShaderSupport = true;
        m_capabilities.geometryShaderSupport = true;
        m_capabilities.tessellationSupport = true;

        return true;
    }

    void RenderDevice::Shutdown()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        m_backBufferRTV.Reset();
        m_depthStencilView.Reset();
        m_depthStencilTexture.Reset();
        m_swapChain.Reset();
        m_d3dContext.Reset();
        m_d3dDevice.Reset();
#endif
        m_rhiDevice.reset();
    }

    bool RenderDevice::Resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return false;

        m_width = width;
        m_height = height;

#ifdef SPARK_PLATFORM_WINDOWS
        // Release old views
        m_backBufferRTV.Reset();
        m_depthStencilView.Reset();
        m_depthStencilTexture.Reset();

        if (m_d3dContext)
        {
            m_d3dContext->OMSetRenderTargets(0, nullptr, nullptr);
        }

        // Resize swap chain
        if (m_swapChain)
        {
            HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr))
                return false;
        }

        // Recreate views
        if (!CreateBackBufferViews())
            return false;
#endif

        return true;
    }

    void RenderDevice::Present(bool vsync)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (m_swapChain)
        {
            UINT syncInterval = vsync ? 1 : 0;
            m_swapChain->Present(syncInterval, 0);
        }
#endif
    }

    bool RenderDevice::CreateDevice(Spark::NativeWindowHandle hwnd, uint32_t width, uint32_t height, bool fullscreen)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = width;
        swapChainDesc.Height = height;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = 2;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        UINT createDeviceFlags = 0;
#ifdef _DEBUG
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL featureLevel;
        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

        ComPtr<ID3D11Device> baseDevice;
        ComPtr<ID3D11DeviceContext> baseContext;

        HRESULT hr =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevels,
                              _countof(featureLevels), D3D11_SDK_VERSION, &baseDevice, &featureLevel, &baseContext);
        if (FAILED(hr))
            return false;

        hr = baseDevice.As(&m_d3dDevice);
        if (FAILED(hr))
            return false;

        hr = baseContext.As(&m_d3dContext);
        if (FAILED(hr))
            return false;

        // Create swap chain
        ComPtr<IDXGIDevice2> dxgiDevice;
        hr = m_d3dDevice.As(&dxgiDevice);
        if (FAILED(hr))
            return false;

        ComPtr<IDXGIAdapter> dxgiAdapter;
        hr = dxgiDevice->GetAdapter(&dxgiAdapter);
        if (FAILED(hr))
            return false;

        ComPtr<IDXGIFactory2> dxgiFactory;
        hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
        if (FAILED(hr))
            return false;

        hr = dxgiFactory->CreateSwapChainForHwnd(m_d3dDevice.Get(), static_cast<HWND>(hwnd), &swapChainDesc, nullptr,
                                                 nullptr, &m_swapChain);
        if (FAILED(hr))
            return false;

        // Query VRAM
        DXGI_ADAPTER_DESC adapterDesc;
        dxgiAdapter->GetDesc(&adapterDesc);
        m_vramUsage = static_cast<size_t>(adapterDesc.DedicatedVideoMemory);
#endif

        return true;
    }

    bool RenderDevice::CreateBackBufferViews()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        // Get back buffer and create RTV
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr))
            return false;

        hr = m_d3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_backBufferRTV);
        if (FAILED(hr))
            return false;

        // Create depth stencil
        D3D11_TEXTURE2D_DESC depthDesc = {};
        depthDesc.Width = m_width;
        depthDesc.Height = m_height;
        depthDesc.MipLevels = 1;
        depthDesc.ArraySize = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        hr = m_d3dDevice->CreateTexture2D(&depthDesc, nullptr, &m_depthStencilTexture);
        if (FAILED(hr))
            return false;

        hr = m_d3dDevice->CreateDepthStencilView(m_depthStencilTexture.Get(), nullptr, &m_depthStencilView);
        if (FAILED(hr))
            return false;

        // Set viewport
        D3D11_VIEWPORT viewport = {};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<FLOAT>(m_width);
        viewport.Height = static_cast<FLOAT>(m_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_d3dContext->RSSetViewports(1, &viewport);

        // Bind render targets
        ID3D11RenderTargetView* rtvs[] = {m_backBufferRTV.Get()};
        m_d3dContext->OMSetRenderTargets(1, rtvs, m_depthStencilView.Get());
#endif

        return true;
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
