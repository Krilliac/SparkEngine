/**
 * @file BackendWindows.cpp
 * @brief Win32 + Direct3D 11 backend of the installer GUI: a per-monitor-DPI-aware, centred,
 *        fixed-size window, the shell folder picker and title-bar colouring.
 */

#ifdef _WIN32

#include "WizardGui.h"

#include "SparkUI/Dpi.h"
#include "SparkUI/Runtime.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <windows.h>

#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace SparkInstaller
{
    namespace
    {
        // Resource 1 of SparkInstaller.rc is the application icon.
        constexpr WORD kAppIconResourceId = 1;
        constexpr wchar_t kWindowClassName[] = L"SparkInstallerWnd";
        // Fixed-size window: a caption with system menu and minimise, but no sizing frame.
        constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        // DwmSetWindowAttribute attributes, spelled out because older SDKs do not declare them.
        constexpr DWORD kDwmUseImmersiveDarkModeBefore20H1 = 19;
        constexpr DWORD kDwmUseImmersiveDarkMode = 20;
        constexpr DWORD kDwmCaptionColor = 35;
        constexpr DWORD kDwmTextColor = 36;

        HWND g_hwnd = nullptr;
        WNDCLASSEXW g_wc{};
        bool g_classRegistered = false;
        ID3D11Device* g_d3dDevice = nullptr;
        ID3D11DeviceContext* g_d3dContext = nullptr;
        IDXGISwapChain* g_swapChain = nullptr;
        ID3D11RenderTargetView* g_mainRTV = nullptr;
        bool g_win32BackendReady = false;
        bool g_dx11BackendReady = false;
        bool g_quit = false;
        bool g_closeRequested = false;
        float g_dpiScale = 1.0f;

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

        std::wstring Widen(const std::string& utf8)
        {
            if (utf8.empty())
                return {};
            const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
            if (length <= 0)
                return {};
            std::wstring wide(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), length);
            return wide;
        }

        std::string Narrow(const wchar_t* wide)
        {
            if (wide == nullptr || wide[0] == L'\0')
                return {};
            const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (length <= 1)
                return {};
            std::string utf8(static_cast<size_t>(length), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), length, nullptr, nullptr);
            utf8.resize(static_cast<size_t>(length) - 1);
            return utf8;
        }

        // AdjustWindowRectExForDpi needs Windows 10 1607; resolve it at run time and fall back
        // to the system-DPI variant.
        void AdjustRectForDpi(RECT& rect, DWORD style, UINT dpi)
        {
            using AdjustFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
            AdjustFn adjust = nullptr;
            if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
                adjust = reinterpret_cast<AdjustFn>(
                    reinterpret_cast<void*>(GetProcAddress(user32, "AdjustWindowRectExForDpi")));
            if (adjust)
                adjust(&rect, style, FALSE, 0, dpi);
            else
                AdjustWindowRectEx(&rect, style, FALSE, 0);
        }

        // Size the window so its client area is width x height logical pixels at the current DPI
        // and centre it on the work area of the monitor it was created on.
        void SizeAndCenterWindow(int width, int height)
        {
            RECT rect{0, 0, static_cast<LONG>(std::lround(static_cast<float>(width) * g_dpiScale)),
                      static_cast<LONG>(std::lround(static_cast<float>(height) * g_dpiScale))};
            AdjustRectForDpi(rect, kWindowStyle, static_cast<UINT>(std::lround(g_dpiScale * 96.0f)));
            const LONG outerWidth = rect.right - rect.left;
            const LONG outerHeight = rect.bottom - rect.top;

            RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &monitorInfo))
                work = monitorInfo.rcWork;
            const LONG x = work.left + std::max<LONG>(0, (work.right - work.left - outerWidth) / 2);
            const LONG y = work.top + std::max<LONG>(0, (work.bottom - work.top - outerHeight) / 2);
            SetWindowPos(g_hwnd, nullptr, x, y, outerWidth, outerHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        bool CreateDevice(D3D_DRIVER_TYPE driverType, const DXGI_SWAP_CHAIN_DESC& desc)
        {
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
            D3D_FEATURE_LEVEL outLevel;
            return SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, driverType, nullptr, 0, levels, 2,
                                                           D3D11_SDK_VERSION, &desc, &g_swapChain, &g_d3dDevice,
                                                           &outLevel, &g_d3dContext));
        }

        LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
                return true;
            switch (msg)
            {
            case WM_SIZE:
                if (g_d3dDevice && g_swapChain && wParam != SIZE_MINIMIZED)
                {
                    CleanupRenderTarget();
                    g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                    CreateRenderTarget();
                }
                return 0;
            case WM_DPICHANGED:
            {
                // Move to the rectangle Windows suggests for the new monitor and let the Spark UI
                // runtime apply the new scale at the start of the next frame.
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                g_dpiScale = static_cast<float>(LOWORD(wParam)) / 96.0f;
                Spark::Ui::RequestScale(g_dpiScale, 1.0f);
                return 0;
            }
            case WM_CLOSE:
                g_closeRequested = true;
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
        // Per-monitor DPI awareness must be set before the first window exists.
        Spark::Ui::EnableProcessDpiAwareness();

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(kAppIconResourceId));
        if (icon == nullptr)
            icon = LoadIconW(nullptr, IDI_APPLICATION);

        g_wc = {};
        g_wc.cbSize = sizeof(g_wc);
        g_wc.style = CS_CLASSDC;
        g_wc.lpfnWndProc = WndProc;
        g_wc.hInstance = instance;
        g_wc.hIcon = icon;
        g_wc.hIconSm = icon;
        g_wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        g_wc.lpszClassName = kWindowClassName;
        if (!RegisterClassExW(&g_wc))
        {
            std::fprintf(stderr, "RegisterClassExW failed (error %lu)\n", GetLastError());
            return false;
        }
        g_classRegistered = true;

        const std::wstring wideTitle = Widen(title != nullptr ? title : "");
        g_hwnd = CreateWindowExW(0, kWindowClassName, wideTitle.c_str(), kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                                 width, height, nullptr, nullptr, instance, nullptr);
        if (g_hwnd == nullptr)
        {
            std::fprintf(stderr, "CreateWindowExW failed (error %lu)\n", GetLastError());
            GuiBackend_Shutdown();
            return false;
        }
        g_dpiScale = Spark::Ui::WindowDpiScale(g_hwnd);
        SizeAndCenterWindow(width, height);

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = g_hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        // The software rasteriser keeps the wizard usable in virtual machines without a GPU.
        if (!CreateDevice(D3D_DRIVER_TYPE_HARDWARE, sd) && !CreateDevice(D3D_DRIVER_TYPE_WARP, sd))
        {
            std::fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed\n");
            GuiBackend_Shutdown();
            return false;
        }

        // A fixed-size installer must not toggle to exclusive fullscreen on Alt+Enter.
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(g_swapChain->GetParent(IID_PPV_ARGS(&factory))) && factory)
        {
            factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
            factory->Release();
        }
        CreateRenderTarget();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        if (!ImGui_ImplWin32_Init(g_hwnd))
        {
            std::fprintf(stderr, "ImGui_ImplWin32_Init failed\n");
            GuiBackend_Shutdown();
            return false;
        }
        g_win32BackendReady = true;
        if (!ImGui_ImplDX11_Init(g_d3dDevice, g_d3dContext))
        {
            std::fprintf(stderr, "ImGui_ImplDX11_Init failed\n");
            GuiBackend_Shutdown();
            return false;
        }
        g_dx11BackendReady = true;

        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        UpdateWindow(g_hwnd);
        return true;
    }

    float GuiBackend_DpiScale()
    {
        return g_dpiScale;
    }

    GuiSystemPreferences GuiBackend_QuerySystemPreferences()
    {
        GuiSystemPreferences prefs;
        BOOL animations = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0))
            prefs.reduceMotion = animations == FALSE;
        HIGHCONTRASTW contrast{};
        contrast.cbSize = sizeof(contrast);
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0))
            prefs.highContrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        return prefs;
    }

    std::filesystem::path GuiBackend_ExecutableDir()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
                return {};
            if (length < buffer.size())
            {
                buffer.resize(length);
                return std::filesystem::path(buffer).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    bool GuiBackend_BeginFrame()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                g_quit = true;
        }
        if (g_quit)
            return false;

        // A minimised window has nothing to show; do not spin.
        if (IsIconic(g_hwnd))
            Sleep(20);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // Apply a pending DPI change here, between the platform frame and NewFrame, so the scale
        // never changes mid-frame.
        Spark::Ui::BeginFrameScale();
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
        // An occluded window (screen locked, fully covered) presents without vsync pacing.
        if (g_swapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED)
            Sleep(20);
    }

    bool GuiBackend_ConsumeCloseRequest()
    {
        const bool requested = g_closeRequested;
        g_closeRequested = false;
        return requested;
    }

    void GuiBackend_Shutdown()
    {
        if (g_dx11BackendReady)
        {
            ImGui_ImplDX11_Shutdown();
            g_dx11BackendReady = false;
        }
        if (g_win32BackendReady)
        {
            ImGui_ImplWin32_Shutdown();
            g_win32BackendReady = false;
        }
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
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
        if (g_classRegistered)
        {
            UnregisterClassW(kWindowClassName, g_wc.hInstance);
            g_classRegistered = false;
        }
        g_quit = false;
        g_closeRequested = false;
    }

    bool GuiBackend_PickFolder(const std::string& startPath, std::string& chosen)
    {
        // COM may already be initialised on this thread (S_FALSE) or in another apartment model
        // (RPC_E_CHANGED_MODE); only a successful init is balanced.
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool balanceCom = SUCCEEDED(comInit);

        bool picked = false;
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) &&
            dialog)
        {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            dialog->SetTitle(L"Choose the install folder");

            // The destination usually does not exist yet; start in its nearest existing ancestor.
            std::filesystem::path start(Widen(startPath));
            std::error_code ec;
            while (!start.empty() && !std::filesystem::is_directory(start, ec))
            {
                std::filesystem::path parent = start.parent_path();
                if (parent == start)
                    break;
                start = std::move(parent);
            }
            IShellItem* startItem = nullptr;
            if (!start.empty() &&
                SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&startItem))) && startItem)
            {
                dialog->SetFolder(startItem);
                startItem->Release();
            }

            if (SUCCEEDED(dialog->Show(g_hwnd)))
            {
                IShellItem* result = nullptr;
                if (SUCCEEDED(dialog->GetResult(&result)) && result)
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        chosen = Narrow(path);
                        picked = !chosen.empty();
                        CoTaskMemFree(path);
                    }
                    result->Release();
                }
            }
            dialog->Release();
        }

        if (balanceCom)
            CoUninitialize();
        return picked;
    }

    void GuiBackend_SetCaptionColors(std::uint32_t captionRgb, std::uint32_t textRgb)
    {
        if (g_hwnd == nullptr)
            return;
        HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dwm == nullptr)
            return;
        using SetAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        const auto setAttribute =
            reinterpret_cast<SetAttributeFn>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
        if (setAttribute)
        {
            const BOOL dark = TRUE;
            if (FAILED(setAttribute(g_hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark))))
                setAttribute(g_hwnd, kDwmUseImmersiveDarkModeBefore20H1, &dark, sizeof(dark));
            const COLORREF caption = RGB((captionRgb >> 16) & 0xFF, (captionRgb >> 8) & 0xFF, captionRgb & 0xFF);
            const COLORREF text = RGB((textRgb >> 16) & 0xFF, (textRgb >> 8) & 0xFF, textRgb & 0xFF);
            setAttribute(g_hwnd, kDwmCaptionColor, &caption, sizeof(caption));
            setAttribute(g_hwnd, kDwmTextColor, &text, sizeof(text));
        }
        FreeLibrary(dwm);
    }
} // namespace SparkInstaller

#endif // _WIN32
