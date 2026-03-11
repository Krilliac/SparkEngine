/**
 * @file RenderDevice.h
 * @brief Thin wrapper around the RHI device and swap chain (R3.1 — Architecture Analysis)
 *
 * Extracted from the monolithic GraphicsEngine to separate device management
 * from rendering logic. RenderDevice owns the GPU device, swap chain, and
 * core render targets. It provides the low-level interface that RenderPipeline
 * and SceneRenderer build upon.
 *
 * @threadsafety Main thread only for device operations.
 */

#pragma once

#include "../Core/Platform.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHITypes.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
using Microsoft::WRL::ComPtr;
#endif

#include <cstdint>
#include <memory>
#include <string>

namespace Spark::Graphics
{

    /**
     * @brief GPU device wrapper providing a unified interface over the RHI.
     *
     * RenderDevice encapsulates device creation, swap chain management,
     * and core render target lifecycle. It serves as the foundation for
     * all GPU operations in the engine.
     *
     * ### DX11 direct access (transitional)
     * During the RHI migration, DX11-specific accessors remain available.
     * New code should use the RHI interface methods instead.
     */
    class RenderDevice
    {
      public:
        RenderDevice() = default;
        ~RenderDevice();

        /**
         * @brief Initialize the device with a native window handle.
         * @param hwnd       Native window handle.
         * @param width      Initial backbuffer width.
         * @param height     Initial backbuffer height.
         * @param fullscreen Whether to initialize in fullscreen mode.
         * @param backend    Preferred graphics backend (Auto = platform default).
         * @return true on success.
         */
        bool Initialize(Spark::NativeWindowHandle hwnd, uint32_t width, uint32_t height, bool fullscreen = false,
                        RHI::GraphicsBackend backend = RHI::GraphicsBackend::Auto);

        /**
         * @brief Release all device resources.
         */
        void Shutdown();

        /**
         * @brief Resize the swap chain and associated render targets.
         * @param width  New width in pixels.
         * @param height New height in pixels.
         * @return true on success.
         */
        bool Resize(uint32_t width, uint32_t height);

        /**
         * @brief Present the current backbuffer to the screen.
         * @param vsync  Whether to synchronize with vertical blank.
         */
        void Present(bool vsync = true);

        // ====================================================================
        // Device queries
        // ====================================================================

        uint32_t GetWidth() const { return m_width; }
        uint32_t GetHeight() const { return m_height; }
        bool IsFullscreen() const { return m_fullscreen; }
        RHI::GraphicsBackend GetBackend() const { return m_backend; }

        /**
         * @brief Get estimated VRAM usage in bytes.
         */
        size_t GetVRAMUsage() const { return m_vramUsage; }

        /**
         * @brief Get device capability information.
         */
        const RHI::RHIDeviceCapabilities& GetCapabilities() const { return m_capabilities; }

        // ====================================================================
        // RHI access (preferred for new code)
        // ====================================================================

        RHI::IRHIDevice* GetRHIDevice() const { return m_rhiDevice.get(); }

        // ====================================================================
        // DX11 direct access (transitional — prefer RHI for new code)
        // ====================================================================

#ifdef SPARK_PLATFORM_WINDOWS
        ID3D11Device1* GetD3DDevice() const { return m_d3dDevice.Get(); }
        ID3D11DeviceContext1* GetD3DContext() const { return m_d3dContext.Get(); }
        IDXGISwapChain1* GetSwapChain() const { return m_swapChain.Get(); }
        ID3D11RenderTargetView* GetBackBufferRTV() const { return m_backBufferRTV.Get(); }
        ID3D11DepthStencilView* GetDepthStencilView() const { return m_depthStencilView.Get(); }
#endif

      private:
        bool CreateDevice(Spark::NativeWindowHandle hwnd, uint32_t width, uint32_t height, bool fullscreen);
        bool CreateBackBufferViews();

        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_fullscreen = false;
        RHI::GraphicsBackend m_backend = RHI::GraphicsBackend::Auto;
        size_t m_vramUsage = 0;
        RHI::RHIDeviceCapabilities m_capabilities{};

        std::unique_ptr<RHI::IRHIDevice> m_rhiDevice;

#ifdef SPARK_PLATFORM_WINDOWS
        ComPtr<ID3D11Device1> m_d3dDevice;
        ComPtr<ID3D11DeviceContext1> m_d3dContext;
        ComPtr<IDXGISwapChain1> m_swapChain;
        ComPtr<ID3D11RenderTargetView> m_backBufferRTV;
        ComPtr<ID3D11DepthStencilView> m_depthStencilView;
        ComPtr<ID3D11Texture2D> m_depthStencilTexture;
#endif
    };

} // namespace Spark::Graphics
