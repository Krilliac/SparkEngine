/**
 * @file RHI210D3D11EngineFixture.h
 * @brief Shared RHI-210 fixture: a WARP GraphicsEngine in a hidden window that
 *        renders a lit, shadow-casting cube and reads back the finished frame.
 *
 * Used by TestRHI210D3D11ValidationReal.cpp, TestRHI210D3D11DeviceLossReal.cpp
 * and the d3d11-warp golden lanes (WarpGraphicsEngine, RenderObjectsFrame).
 * Everything here drives the production GraphicsEngine (windowed Initialize,
 * BeginFrame/RenderScene/EndFrame, the ECS draw list and the AssetPipeline);
 * only the capture reads the swap-chain back buffer directly, before Present.
 */
#pragma once

#ifdef _WIN32

#include "Core/EngineContext.h"
#include "Game/GameObject.h"
#include "Graphics/AssetPipeline.h"
#include "Graphics/GraphicsEngine.h"
#include "TestFramework.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace RHI210
{
    /// Sets a process environment variable (the Win32 block GraphicsEngine reads)
    /// for one scope and restores the previous value, or removes it, afterwards.
    class ScopedEnvironmentVariable
    {
      public:
        ScopedEnvironmentVariable(const wchar_t* name, const wchar_t* value) : m_name(name)
        {
            wchar_t previous[256] = {};
            const DWORD length = GetEnvironmentVariableW(name, previous, 256);
            m_hadValue = length > 0 && length < 256;
            if (m_hadValue)
            {
                m_previous = previous;
            }
            SetEnvironmentVariableW(name, value);
        }
        ~ScopedEnvironmentVariable()
        {
            SetEnvironmentVariableW(m_name.c_str(), m_hadValue ? m_previous.c_str() : nullptr);
        }

        ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
        ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

      private:
        std::wstring m_name;
        std::wstring m_previous;
        bool m_hadValue = false;
    };

    /// A never-shown top-level window for the engine's swap chain.
    class HiddenWindow
    {
      public:
        HiddenWindow()
            : m_hwnd(CreateWindowExW(0, L"STATIC", L"SparkTestsRHI210", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr,
                                     nullptr, GetModuleHandleW(nullptr), nullptr))
        {
        }
        ~HiddenWindow()
        {
            if (m_hwnd)
            {
                DestroyWindow(m_hwnd);
            }
        }

        HiddenWindow(const HiddenWindow&) = delete;
        HiddenWindow& operator=(const HiddenWindow&) = delete;

        HWND Get() const { return m_hwnd; }

      private:
        HWND m_hwnd;
    };

    /// A captured back buffer, tightly packed RGBA8.
    struct Frame
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;

        const uint8_t* At(uint32_t x, uint32_t y) const { return rgba.data() + (size_t(y) * width + x) * 4; }
    };

    /// AssetPipeline resolves a path that names no file to a unit cube, which gives
    /// a real GPU-resident mesh without a fixture file.
    inline constexpr const char* kCubeMesh = "test://rhi210_cube";

    inline bool LoadCube(GraphicsEngine& engine)
    {
        AssetPipeline* assets = engine.GetAssetPipeline();
        return assets && assets->LoadMesh(kCubeMesh) != nullptr;
    }

    /// Copies the swap-chain back buffer through a staging texture. Must run
    /// between RenderScene and EndFrame: the DISCARD swap effect invalidates the
    /// buffer at Present.
    inline Frame ReadBackBuffer(GraphicsEngine& engine)
    {
        Frame frame;
        IDXGISwapChain* swapChain = engine.GetSwapChain();
        ID3D11DeviceContext* context = engine.GetContext();
        if (!swapChain || !context || !engine.GetDevice())
        {
            return frame;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        {
            return frame;
        }
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        if (FAILED(engine.GetDevice()->CreateTexture2D(&desc, nullptr, &staging)))
        {
            return frame;
        }
        context->CopyResource(staging.Get(), backBuffer.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        {
            return frame;
        }
        frame.width = desc.Width;
        frame.height = desc.Height;
        frame.rgba.resize(size_t(desc.Width) * desc.Height * 4);
        for (uint32_t y = 0; y < desc.Height; ++y)
        {
            const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
            std::copy(row, row + size_t(desc.Width) * 4, frame.rgba.begin() + size_t(y) * desc.Width * 4);
        }
        context->Unmap(staging.Get(), 0);
        return frame;
    }

    /// One production frame: a shadow-casting default-material cube through the
    /// ECS draw list, the active render path and post-processing, captured
    /// before Present.
    inline Frame RenderCubeFrame(GraphicsEngine& engine)
    {
        using namespace DirectX;
        const float aspect =
            engine.GetWindowHeight() > 0 ? float(engine.GetWindowWidth()) / engine.GetWindowHeight() : 1.0f;
        const XMMATRIX view =
            XMMatrixLookAtLH(XMVectorSet(1.4f, 1.2f, -2.2f, 1.0f), XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.1f, 100.0f);

        engine.SubmitMeshForRendering(kCubeMesh, "", XMMatrixRotationY(0.6f), /*castShadows*/ true);
        engine.BeginFrame();
        engine.RenderScene(view, proj, {});
        Frame frame = ReadBackBuffer(engine);
        engine.EndFrame();
        return frame;
    }

    /**
     * @brief A WARP GraphicsEngine in a hidden window with a fixed back-buffer size.
     *
     * The engine is installed as the injected EngineContext's graphics, as a game
     * module's frame has it, so GameObject::Render binds its basic material. The
     * back buffer is resized explicitly: the hidden window's client size depends on
     * the host's window metrics and DPI.
     */
    class WarpGraphicsEngine
    {
      public:
        WarpGraphicsEngine(uint32_t width, uint32_t height) : m_width(width), m_height(height)
        {
            if (!m_window.Get() || FAILED(m_engine.Initialize(m_window.Get())))
            {
                return;
            }
            m_engine.OnResize(width, height);
            m_context.SetGraphics(&m_engine);
            EngineContext::SetInjected(&m_context);
            m_injected = true;
        }

        ~WarpGraphicsEngine()
        {
            if (m_injected)
            {
                EngineContext::SetInjected(nullptr);
            }
            m_engine.Shutdown();
        }

        WarpGraphicsEngine(const WarpGraphicsEngine&) = delete;
        WarpGraphicsEngine& operator=(const WarpGraphicsEngine&) = delete;

        bool Ready() const
        {
            return m_injected && m_engine.GetWindowWidth() == m_width && m_engine.GetWindowHeight() == m_height;
        }

        GraphicsEngine& Engine() { return m_engine; }

      private:
        ScopedEnvironmentVariable m_warp{L"SPARK_D3D11_DRIVER", L"warp"};
        ScopedEnvironmentVariable m_debug{L"SPARK_D3D11_DEBUG_LAYER", L"1"};
        HiddenWindow m_window;
        GraphicsEngine m_engine;
        EngineContext m_context;
        uint32_t m_width;
        uint32_t m_height;
        bool m_injected = false;
    };

    /// One production frame of GameObjects through RenderScene (plus whatever the
    /// ECS draw list holds), captured before Present.
    inline Frame RenderObjectsFrame(GraphicsEngine& engine, const DirectX::XMMATRIX& view,
                                    const DirectX::XMMATRIX& projection, const std::vector<GameObject*>& objects)
    {
        engine.BeginFrame();
        engine.RenderScene(view, projection, objects);
        Frame frame = ReadBackBuffer(engine);
        engine.EndFrame();
        const auto validation = engine.GetValidationCounts();
        ASSERT_TRUE(validation.has_value());
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> queue;
        ASSERT_TRUE(SUCCEEDED(engine.GetDevice()->QueryInterface(IID_PPV_ARGS(&queue))));
        EXPECT_EQ(queue->GetNumMessagesDiscardedByMessageCountLimit(), uint64_t(0));
        EXPECT_EQ(validation->corruption, uint64_t(0));
        EXPECT_EQ(validation->errors, uint64_t(0));
        EXPECT_EQ(validation->warnings, uint64_t(0));
        return frame;
    }

    /// Mean absolute per-channel (RGB) difference between two same-sized frames,
    /// or a value above any threshold when the sizes differ.
    inline double MeanAbsoluteDifference(const Frame& a, const Frame& b)
    {
        if (a.width != b.width || a.height != b.height || a.rgba.empty())
        {
            return 1.0e9;
        }
        uint64_t total = 0;
        for (size_t i = 0; i < a.rgba.size(); i += 4)
        {
            for (size_t c = 0; c < 3; ++c)
            {
                total += static_cast<uint64_t>(std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])));
            }
        }
        return double(total) / double((a.rgba.size() / 4) * 3);
    }
} // namespace RHI210

#endif // _WIN32
