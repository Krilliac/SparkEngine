#ifdef _WIN32

#include "WizardGui.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <tchar.h>
#include <windows.h>

#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace SparkInstaller
{
    namespace
    {
        HWND g_hwnd = nullptr;
        WNDCLASSEXW g_wc{};
        ID3D11Device* g_d3dDevice = nullptr;
        ID3D11DeviceContext* g_d3dContext = nullptr;
        IDXGISwapChain* g_swapChain = nullptr;
        ID3D11RenderTargetView* g_mainRTV = nullptr;
        bool g_quit = false;

        void CreateRenderTarget()
        {
            ID3D11Texture2D* backBuffer = nullptr;
            g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
            if (backBuffer)
            {
                g_d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRTV);
                backBuffer->Release();
            }
        }

        void CleanupRenderTarget()
        {
            if (g_mainRTV)
            {
                g_mainRTV->Release();
                g_mainRTV = nullptr;
            }
        }

        LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
                return true;
            switch (msg)
            {
            case WM_SIZE:
                if (g_d3dDevice && wParam != SIZE_MINIMIZED)
                {
                    CleanupRenderTarget();
                    g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                    CreateRenderTarget();
                }
                return 0;
            case WM_SYSCOMMAND:
                if ((wParam & 0xfff0) == SC_KEYMENU)
                    return 0;
                break;
            case WM_DESTROY:
                g_quit = true;
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    } // namespace

    bool GuiBackend_Init(int width, int height, const char* title)
    {
        g_wc = {
            sizeof(g_wc), CS_CLASSDC,           WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr,
            nullptr,      L"SparkInstallerWnd", nullptr};
        RegisterClassExW(&g_wc);
        wchar_t wtitle[256] = {};
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
        g_hwnd = CreateWindowW(g_wc.lpszClassName, wtitle, WS_OVERLAPPEDWINDOW, 100, 100, width, height, nullptr,
                               nullptr, g_wc.hInstance, nullptr);

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = g_hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL outLevel;
        if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                          &sd, &g_swapChain, &g_d3dDevice, &outLevel, &g_d3dContext) != S_OK)
        {
            std::fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed\n");
            return false;
        }

        CreateRenderTarget();
        ShowWindow(g_hwnd, SW_SHOWDEFAULT);
        UpdateWindow(g_hwnd);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        if (!ImGui_ImplWin32_Init(g_hwnd))
            return false;
        if (!ImGui_ImplDX11_Init(g_d3dDevice, g_d3dContext))
            return false;
        return true;
    }

    bool GuiBackend_BeginFrame()
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                g_quit = true;
        }
        if (g_quit)
            return false;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        return true;
    }

    void GuiBackend_EndFrame()
    {
        ImGui::Render();
        const float clear[] = {0.10f, 0.10f, 0.12f, 1.0f};
        g_d3dContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_d3dContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    void GuiBackend_Shutdown()
    {
        if (ImGui::GetCurrentContext())
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        CleanupRenderTarget();
        if (g_swapChain)
        {
            g_swapChain->Release();
            g_swapChain = nullptr;
        }
        if (g_d3dContext)
        {
            g_d3dContext->Release();
            g_d3dContext = nullptr;
        }
        if (g_d3dDevice)
        {
            g_d3dDevice->Release();
            g_d3dDevice = nullptr;
        }
        if (g_hwnd)
        {
            DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
        }
        UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);
    }
} // namespace SparkInstaller

#endif // _WIN32
