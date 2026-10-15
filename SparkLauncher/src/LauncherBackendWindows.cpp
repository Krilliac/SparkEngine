/**
 * @file LauncherBackendWindows.cpp
 * @brief Win32 + D3D11 backend for SparkLauncher (Windows): per-monitor DPI aware, centred,
 *        minimum-size window with the Spark icon and a caption matching the launcher theme.
 * @author Spark Engine Team
 * @date 2025
 */

#ifdef _WIN32

#include "LauncherBackend.h"
#include "LauncherViews.h"

#include "SparkUI/Dpi.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Tokens.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <windows.h>

#include <d3d11.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace SparkLauncher
{
    namespace
    {
        /// IDI_SPARK_APP in resources/SparkLauncher.rc (the Spark logomark icon).
        constexpr WORD kAppIconResourceId = 1;
        constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
        constexpr float kBaseDpi = 96.0f;

        HWND g_hwnd = nullptr;
        WNDCLASSEXW g_wc{};
        bool g_classRegistered = false;
        bool g_win32BackendReady = false;
        bool g_dx11BackendReady = false;
        int g_minClientWidth = 0;  ///< Logical minimum client size, scaled by the window's DPI.
        int g_minClientHeight = 0; ///< Logical minimum client size, scaled by the window's DPI.
        ID3D11Device* g_d3dDevice = nullptr;
        ID3D11DeviceContext* g_d3dContext = nullptr;
        IDXGISwapChain* g_swapChain = nullptr;
        ID3D11RenderTargetView* g_mainRTV = nullptr;
        bool g_quit = false;
        bool g_systemAppearanceChanged = false; ///< Set by WndProc, applied by Backend_BeginFrame.

        LONG ScaleToDevice(int logical, float scale)
        {
            return static_cast<LONG>(std::lround(static_cast<float>(logical) * scale));
        }

        std::wstring Utf8ToWide(const char* text)
        {
            if (text == nullptr || text[0] == '\0')
            {
                return {};
            }
            const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
            if (length <= 0)
            {
                return {};
            }
            std::wstring wide(static_cast<std::size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
            wide.resize(static_cast<std::size_t>(length - 1));
            return wide;
        }

        /// Appearance that matches Windows: High Contrast when it is on, else the app light/dark mode.
        Spark::Ui::Appearance SystemAppearance()
        {
            HIGHCONTRASTW contrast{};
            contrast.cbSize = sizeof(contrast);
            if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0)
            {
                return Spark::Ui::Appearance::HighContrast;
            }
            DWORD appsUseLightTheme = 0;
            DWORD size = sizeof(appsUseLightTheme);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                             L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &appsUseLightTheme,
                             &size) == ERROR_SUCCESS &&
                appsUseLightTheme != 0)
            {
                return Spark::Ui::Appearance::Light;
            }
            return Spark::Ui::Appearance::Dark;
        }

        /// Dark caption on Windows 10 1809+ and 11, and a SurfaceSunken caption colour on Windows 11.
        /// Systems that do not know an attribute reject it; that is ignored.
        void ApplyCaptionTheme(HWND hwnd, Spark::Ui::Appearance appearance)
        {
            constexpr DWORD kUseImmersiveDarkMode = 20;       // DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1+)
            constexpr DWORD kUseImmersiveDarkModeLegacy = 19; // the same attribute on Windows 10 1809-1909
            constexpr DWORD kCaptionColor = 35;               // DWMWA_CAPTION_COLOR (Windows 11)
            const BOOL dark = appearance == Spark::Ui::Appearance::Light ? FALSE : TRUE;
            if (FAILED(DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark))))
            {
                DwmSetWindowAttribute(hwnd, kUseImmersiveDarkModeLegacy, &dark, sizeof(dark));
            }
            const Spark::Ui::Rgba sunken = Spark::Ui::Get(Spark::Ui::ActiveTokens(), Spark::Ui::Role::SurfaceSunken);
            const auto channel = [](float value)
            { return static_cast<BYTE>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
            const COLORREF caption = RGB(channel(sunken.r), channel(sunken.g), channel(sunken.b));
            DwmSetWindowAttribute(hwnd, kCaptionColor, &caption, sizeof(caption));
        }

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

        bool CreateDeviceAndSwapChain()
        {
            DXGI_SWAP_CHAIN_DESC sd{};
            sd.BufferCount = 2;
            sd.BufferDesc.Width = 0;
            sd.BufferDesc.Height = 0;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferDesc.RefreshRate.Numerator = 60;
            sd.BufferDesc.RefreshRate.Denominator = 1;
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = g_hwnd;
            sd.SampleDesc.Count = 1;
            sd.Windowed = TRUE;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            UINT createFlags = 0;
            D3D_FEATURE_LEVEL featureLevel;
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
            if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags, levels, 2,
                                              D3D11_SDK_VERSION, &sd, &g_swapChain, &g_d3dDevice, &featureLevel,
                                              &g_d3dContext) != S_OK)
            {
                std::fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed\n");
                return false;
            }
            CreateRenderTarget();
            return true;
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
            case WM_GETMINMAXINFO:
            {
                // The minimum is a client size in logical pixels, so it grows with the monitor's DPI.
                const float scale = Spark::Ui::WindowDpiScale(hwnd);
                RECT frame{0, 0, ScaleToDevice(g_minClientWidth, scale), ScaleToDevice(g_minClientHeight, scale)};
                AdjustWindowRectEx(&frame, kWindowStyle, FALSE, 0);
                auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
                limits->ptMinTrackSize.x = frame.right - frame.left;
                limits->ptMinTrackSize.y = frame.bottom - frame.top;
                return 0;
            }
            case WM_DPICHANGED:
            {
                // Windows suggests a rectangle that keeps the window's logical size on the new monitor;
                // the UI scale follows at the start of the next frame (Spark::Ui::BeginFrameScale).
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                if (ImGui::GetCurrentContext())
                {
                    Spark::Ui::RequestScale(static_cast<float>(HIWORD(wParam)) / kBaseDpi, Spark::Ui::UserScale());
                }
                return 0;
            }
            case WM_SETTINGCHANGE:
            {
                // Apps light/dark ("ImmersiveColorSet") and High Contrast arrive here. Only flag the change:
                // the message can be dispatched inside a frame, so Backend_BeginFrame re-themes before NewFrame.
                const auto* area = reinterpret_cast<const wchar_t*>(lParam);
                if (wParam == SPI_SETHIGHCONTRAST ||
                    (area != nullptr && std::wstring_view(area) == L"ImmersiveColorSet"))
                {
                    g_systemAppearanceChanged = true;
                }
                break;
            }
            case WM_THEMECHANGED:
                g_systemAppearanceChanged = true;
                break;
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

        bool RegisterWindowClass(HINSTANCE instance)
        {
            g_wc = {};
            g_wc.cbSize = sizeof(g_wc);
            g_wc.style = CS_CLASSDC;
            g_wc.lpfnWndProc = WndProc;
            g_wc.hInstance = instance;
            g_wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            g_wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON, 0, 0,
                                                       LR_DEFAULTSIZE | LR_SHARED));
            g_wc.hIconSm =
                static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
            g_wc.lpszClassName = L"SparkLauncherWnd";
            if (!RegisterClassExW(&g_wc))
            {
                std::fprintf(stderr, "RegisterClassExW failed (error %lu)\n", GetLastError());
                return false;
            }
            g_classRegistered = true;
            return true;
        }

        /// Create the window centred on the primary monitor's work area at spec size x that monitor's DPI.
        bool CreateCentredWindow(const BackendWindowSpec& spec, HINSTANCE instance)
        {
            const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            if (!GetMonitorInfoW(monitor, &monitorInfo))
            {
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &monitorInfo.rcWork, 0);
            }
            const RECT& work = monitorInfo.rcWork;
            const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
            RECT frame{0, 0, ScaleToDevice(spec.width, scale), ScaleToDevice(spec.height, scale)};
            AdjustWindowRectEx(&frame, kWindowStyle, FALSE, 0);
            const LONG workWidth = work.right - work.left;
            const LONG workHeight = work.bottom - work.top;
            const LONG width = (std::min)(frame.right - frame.left, workWidth);
            const LONG height = (std::min)(frame.bottom - frame.top, workHeight);
            const LONG x = work.left + (workWidth - width) / 2;
            const LONG y = work.top + (workHeight - height) / 2;

            const std::wstring title = Utf8ToWide(spec.title);
            g_hwnd = CreateWindowExW(0, g_wc.lpszClassName, title.c_str(), kWindowStyle, x, y, width, height, nullptr,
                                     nullptr, instance, nullptr);
            if (!g_hwnd)
            {
                std::fprintf(stderr, "CreateWindowExW failed (error %lu)\n", GetLastError());
                return false;
            }
            return true;
        }
    } // namespace

    bool Backend_Init(const BackendWindowSpec& spec)
    {
        // Before any window class exists, so every window of the process is per-monitor aware.
        Spark::Ui::EnableProcessDpiAwareness();
        g_minClientWidth = spec.minWidth;
        g_minClientHeight = spec.minHeight;

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!RegisterWindowClass(instance) || !CreateCentredWindow(spec, instance) || !CreateDeviceAndSwapChain())
        {
            Backend_Shutdown();
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        const Spark::Ui::Appearance appearance = SystemAppearance();
        InstallLauncherStyle(Spark::Ui::WindowDpiScale(g_hwnd), appearance);

        g_win32BackendReady = ImGui_ImplWin32_Init(g_hwnd);
        g_dx11BackendReady = g_win32BackendReady && ImGui_ImplDX11_Init(g_d3dDevice, g_d3dContext);
        if (!g_dx11BackendReady)
        {
            std::fprintf(stderr, "ImGui platform/renderer backend initialization failed\n");
            Backend_Shutdown();
            return false;
        }

        ApplyCaptionTheme(g_hwnd, appearance);
        ShowWindow(g_hwnd, SW_SHOWDEFAULT);
        UpdateWindow(g_hwnd);
        return true;
    }

    bool Backend_BeginFrame()
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

        // A light/dark or High Contrast switch made while the launcher runs is followed between frames,
        // where SetTokens rebuilds the style at once, so the caption reads the new SurfaceSunken.
        if (g_systemAppearanceChanged)
        {
            g_systemAppearanceChanged = false;
            const Spark::Ui::Appearance appearance = SystemAppearance();
            SetLauncherAppearance(appearance);
            ApplyCaptionTheme(g_hwnd, appearance);
        }

        // A DPI change received above is applied here, between frames.
        Spark::Ui::BeginFrameScale();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        return true;
    }

    void Backend_EndFrame()
    {
        ImGui::Render();
        const Spark::Ui::Rgba sunken = Spark::Ui::Get(Spark::Ui::ActiveTokens(), Spark::Ui::Role::SurfaceSunken);
        const float clear[] = {sunken.r, sunken.g, sunken.b, 1.0f};
        g_d3dContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_d3dContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    void Backend_Shutdown()
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
        {
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
        if (g_classRegistered)
        {
            UnregisterClassW(g_wc.lpszClassName, g_wc.hInstance);
            g_classRegistered = false;
        }
    }

    void Backend_ShowFatalError(const char* title, const char* message)
    {
        const std::wstring wideTitle = Utf8ToWide(title);
        const std::wstring wideMessage = Utf8ToWide(message);
        MessageBoxW(nullptr, wideMessage.c_str(), wideTitle.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
} // namespace SparkLauncher

#endif // _WIN32
