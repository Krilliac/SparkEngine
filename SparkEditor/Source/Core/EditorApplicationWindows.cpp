/**
 * @file EditorApplicationWindows.cpp
 * @brief Windows/D3D11 editor application — window creation, graphics init, run loop, shutdown
 *
 * Contains Win32 WNDCLASS, D3D11 device/swap chain, ImGui Win32 backend,
 * and the Windows message loop. Linux counterpart lives in EditorApplicationLinux.cpp.
 * Shared code (Initialize, Update, RenderFrame) stays in EditorApplication.cpp.
 */
#include "EditorApplication.h"

#ifdef _WIN32

#include "EditorUI.h"
#include "EditorFonts.h"
#include "EditorTheme.h"
#include "ProjectManager.h"
#include "EditorWindowManager.h"
#include "EditorCrashHandler.h"
#include "Core/FaultIsolation.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include "Utils/LogMacros.h"
#include "SparkUI/Dpi.h"
#include "SparkUI/Runtime.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <dwmapi.h>
using Microsoft::WRL::ComPtr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace SparkEditor
{
    namespace
    {
        /// Resource id of the application icon (SparkEditor/resources/SparkEditor.rc, IDI_SPARK_APP).
        constexpr WORD kAppIconResourceId = 1;
        /// Share of the monitor work area the restored (non-maximized) window may cover.
        constexpr float kMaxWorkAreaShare = 0.85f;
        /// DWMWA_USE_IMMERSIVE_DARK_MODE on Windows 10 20H1+ / 11, and its pre-20H1 value (1809+).
        constexpr DWORD kDwmUseImmersiveDarkMode = 20;
        constexpr DWORD kDwmUseImmersiveDarkModeLegacy = 19;
        /// DWMWA_CAPTION_COLOR and DWMWA_TEXT_COLOR (Windows 11 only).
        constexpr DWORD kDwmCaptionColor = 35;
        constexpr DWORD kDwmTextColor = 36;
        constexpr float kChannelMax = 255.0f;
        /// Windows logical DPI at 100 % scale.
        constexpr float kBaselineDpi = 96.0f;

        BYTE ToChannel(float value)
        {
            const float clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
            return static_cast<BYTE>(std::lround(clamped * kChannelMax));
        }

        COLORREF ToColorRef(const ThemeColor& colour)
        {
            return RGB(ToChannel(colour.r), ToChannel(colour.g), ToChannel(colour.b));
        }

        /// Match the native caption to the theme: dark mode on Windows 10 1809+ and 11, caption and
        /// caption-text colours on Windows 11. Attributes an OS does not know fail and are ignored.
        void ApplyNativeCaption(HWND hwnd, const EditorTheme::NativeCaptionStyle& style)
        {
            const BOOL darkMode = style.darkMode ? TRUE : FALSE;
            if (FAILED(DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &darkMode, sizeof(darkMode))))
            {
                DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkModeLegacy, &darkMode, sizeof(darkMode));
            }
            const COLORREF caption = ToColorRef(style.caption);
            const COLORREF text = ToColorRef(style.text);
            DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &caption, sizeof(caption));
            DwmSetWindowAttribute(hwnd, kDwmTextColor, &text, sizeof(text));
        }
    } // namespace

    bool EditorApplication::CreateMainWindow(const EditorConfig& config)
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        // Per-monitor DPI awareness must be set before the first window exists.
        Spark::Ui::EnableProcessDpiAwareness();

        // Register window class. No background brush: the swap chain clears every frame, so the
        // OS never flashes a white client area before the first present.
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        wc.hIcon = static_cast<HICON>(
            LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
        wc.hIconSm =
            static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"SparkEditorWindow";

        if (!RegisterClassExW(&wc))
        {
            console.LogError("Failed to register window class");
            return false;
        }
        m_windowClassRegistered = true;
        console.LogInfo("Window class registered successfully");

        // Create window
        m_hwnd = CreateWindowExW(0,                      // dwExStyle
                                 L"SparkEditorWindow",   // lpClassName
                                 L"Spark Engine Editor", // lpWindowName
                                 WS_OVERLAPPEDWINDOW,    // dwStyle
                                 CW_USEDEFAULT,          // X
                                 CW_USEDEFAULT,          // Y
                                 config.windowWidth,     // nWidth
                                 config.windowHeight,    // nHeight
                                 nullptr,                // hWndParent
                                 nullptr,                // hMenu
                                 instance,               // hInstance
                                 this                    // lpParam
        );

        if (!m_hwnd)
        {
            DWORD error = GetLastError();
            console.LogError("Failed to create window (Error: " + std::to_string(error) + ")");
            return false;
        }

        console.LogInfo("Window created successfully");

        // Size the restored window in logical pixels for its monitor, capped to a share of the
        // work area, and centre it there. Maximizing keeps this as the restore rectangle.
        m_dpiScale = Spark::Ui::WindowDpiScale(m_hwnd);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &monitorInfo) != FALSE)
        {
            const RECT& work = monitorInfo.rcWork;
            const int workWidth = work.right - work.left;
            const int workHeight = work.bottom - work.top;
            const int width =
                std::min(static_cast<int>(std::lround(static_cast<float>(config.windowWidth) * m_dpiScale)),
                         static_cast<int>(static_cast<float>(workWidth) * kMaxWorkAreaShare));
            const int height =
                std::min(static_cast<int>(std::lround(static_cast<float>(config.windowHeight) * m_dpiScale)),
                         static_cast<int>(static_cast<float>(workHeight) * kMaxWorkAreaShare));
            SetWindowPos(m_hwnd, nullptr, work.left + (workWidth - width) / 2, work.top + (workHeight - height) / 2,
                         width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        ShowWindow(m_hwnd, config.startMaximized ? SW_SHOWMAXIMIZED : SW_SHOW);
        UpdateWindow(m_hwnd);

        // The swap chain is created at the real client size, not the requested one.
        RECT client{};
        if (GetClientRect(m_hwnd, &client) != FALSE && client.right > client.left && client.bottom > client.top)
        {
            m_windowWidth = client.right - client.left;
            m_windowHeight = client.bottom - client.top;
        }

        console.LogInfo("Window is now visible and active");
        return true;
    }

    bool EditorApplication::InitializeGraphics()
    {
        std::cout << "Initializing DirectX 11...\n";

        // Create device and swap chain
        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        swapChainDesc.BufferCount = 2;
        swapChainDesc.BufferDesc.Width = m_windowWidth;
        swapChainDesc.BufferDesc.Height = m_windowHeight;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.OutputWindow = m_hwnd;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL featureLevel;
        UINT createDeviceFlags = 0;
#ifdef _DEBUG
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,                  // Adapter
                                                   D3D_DRIVER_TYPE_HARDWARE, // Driver type
                                                   nullptr,                  // Software
                                                   createDeviceFlags,        // Flags
                                                   nullptr,                  // Feature levels
                                                   0,                        // Feature levels count
                                                   D3D11_SDK_VERSION,        // SDK version
                                                   &swapChainDesc,           // Swap chain desc
                                                   &m_swapChain,             // Swap chain
                                                   &m_device,                // Device
                                                   &featureLevel,            // Feature level
                                                   &m_context                // Context
        );

        if (FAILED(hr))
        {
            std::cerr << "Failed to create DirectX device and swap chain\n";
            return false;
        }

        // Create render target view
        ComPtr<ID3D11Texture2D> backBuffer;
        hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr))
        {
            std::cerr << "Failed to get back buffer\n";
            return false;
        }

        hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
        if (FAILED(hr))
        {
            std::cerr << "Failed to create render target view\n";
            return false;
        }

        std::cout << "DirectX 11 initialized successfully\n";
        return true;
    }

    bool EditorApplication::InitializeImGui()
    {
        std::cout << "Initializing Dear ImGui...\n";

        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();

        // Keep per-user editor state out of the repository/current working
        // directory. ImGui retains this pointer until context shutdown.
        const std::filesystem::path editorDataDirectory = ProjectManager::GetEditorDataDirectory();
        std::error_code directoryError;
        std::filesystem::create_directories(editorDataDirectory, directoryError);
        m_imguiIniPath = (editorDataDirectory / "imgui.ini").string();
        io.IniFilename = m_imguiIniPath.c_str();

        // Enable keyboard controls and docking
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        // Docking configuration
        io.ConfigDockingWithShift = false; // Dock without holding Shift
        io.ConfigWindowsResizeFromEdges = true;

        // Load custom fonts before backend initialization, then install the Spark UI runtime at
        // the window's DPI and the saved user scale (0 = Auto: DPI only). The startup theme
        // replaces these Ember tokens once EditorUI applies it.
        EditorFonts::LoadFonts(Spark::Ui::Typography{}.body);
        Spark::Ui::Install(ImGui::GetCurrentContext(),
                           Spark::Ui::GenerateTokens(Spark::Ui::BuiltinSeed(Spark::Ui::PaletteId::Ember),
                                                     Spark::Ui::Appearance::Dark, m_preferences.density),
                           m_dpiScale, m_preferences.uiScale > 0.0f ? m_preferences.uiScale : 1.0f,
                           m_preferences.reduceMotion);
        io.ConfigNavCursorVisibleAlways = m_preferences.alwaysShowFocus;

        // Setup Platform/Renderer backends
        if (!ImGui_ImplWin32_Init(m_hwnd))
        {
            std::cerr << "Failed to initialize ImGui Win32 backend\n";
            ImGui::DestroyContext();
            return false;
        }

        if (!ImGui_ImplDX11_Init(m_device.Get(), m_context.Get()))
        {
            std::cerr << "Failed to initialize ImGui DirectX 11 backend\n";
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return false;
        }

        std::cout << "Dear ImGui initialized successfully\n";
        return true;
    }

    int EditorApplication::Run()
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        if (!m_isInitialized)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Run() called but editor not initialized");
            console.LogCritical("EditorApplication::Run() called but editor not initialized!");
            return -1;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Starting editor main loop (Win32)");
        console.LogInfo("Starting enhanced editor main loop...");

        // Main message loop
        MSG msg = {};
        auto lastTime = std::chrono::high_resolution_clock::now();
        int frameCount = 0;

        while (m_isRunning && msg.message != WM_QUIT)
        {
            // Keep automation runs deterministic and bounded, matching the
            // SDL2 implementation. A limit of zero means run indefinitely.
            if (m_config.testFrameLimit > 0 && frameCount >= m_config.testFrameLimit)
            {
                std::cout << "[TEST] Frame limit reached (" << m_config.testFrameLimit << " frames). Exiting.\n"
                          << std::flush;
                m_isRunning = false;
                break;
            }
            ++frameCount;

            // Calculate delta time
            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
            lastTime = currentTime;

            // Update console (important for external console communication)
            SPARK_GUARDED_UPDATE("EditorConsole", "Editor", { console.Update(); });

            // Process messages
            if (!ProcessMessages())
            {
                m_isRunning = false;
                break;
            }

            if (!m_isRunning)
                break;

            // Update editor
            Update(deltaTime);

            // Render frame
            Render();

            // Update performance metrics
            UpdatePerformanceMetrics();
        }

        console.LogInfo("Enhanced editor main loop ended");
        return static_cast<int>(msg.wParam);
    }

    bool EditorApplication::ProcessMessages()
    {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return true;
    }

    void EditorApplication::Render()
    {
        // A DPI or user-scale change queued since the last frame is applied before NewFrame.
        EditorTheme::ApplyPendingScale();
        if (EditorTheme::GetRevision() != m_appliedCaptionRevision)
        {
            m_appliedCaptionRevision = EditorTheme::GetRevision();
            ApplyNativeCaption(m_hwnd, EditorTheme::NativeCaptionStyleFor(EditorTheme::GetCurrentThemeData()));
        }

        // Clear to the theme's sunken surface so resize gaps match the dock background.
        const Spark::Ui::Rgba clear = Spark::Ui::Get(Spark::Ui::ActiveTokens(), Spark::Ui::Role::SurfaceSunken);
        const float clearColor[4] = {clear.r, clear.g, clear.b, 1.0f};
        m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
        m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);

        // Start ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Render UI
        if (m_ui)
        {
            m_ui->Render();
        }

        // Render editor plugin GUI
        m_pluginManager.RenderAll();

        // Panels (e.g. Scene View) can rebind the immediate context's render
        // target while building their ImGui content. Restore the backbuffer so
        // ImGui always renders into the window rather than a stale/unbound target.
        m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);

        // Render ImGui
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present frame
        const HRESULT presentResult = m_swapChain->Present(1, 0); // VSync enabled
        if (presentResult == S_OK)
        {
            ++m_renderedFrameCount;
        }
        else
        {
            ++m_presentFailureCount;
        }
    }

    void EditorApplication::OnWindowResize(int width, int height)
    {
        if (width <= 0 || height <= 0)
            return;

        m_windowWidth = width;
        m_windowHeight = height;

        if (m_swapChain && m_device && m_context)
        {
            m_rtv.Reset();

            HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            if (SUCCEEDED(hr))
            {
                ComPtr<ID3D11Texture2D> backBuffer;
                hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
                if (SUCCEEDED(hr))
                {
                    m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
                }
            }
        }
    }

    void EditorApplication::SetWindowTitle(const std::string& title)
    {
        if (m_hwnd)
        {
            // Titles carry UTF-8 project and scene names; widen them properly.
            const int length =
                MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), nullptr, 0);
            std::wstring wTitle(static_cast<std::size_t>(std::max(length, 0)), L'\0');
            if (length > 0)
            {
                MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), wTitle.data(), length);
            }
            SetWindowTextW(m_hwnd, wTitle.c_str());
        }
    }

    void EditorApplication::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Shutting down enhanced editor...");

        m_isRunning = false;

        // Shutdown editor plugins before UI teardown
        if (m_pluginLifecycleStarted)
        {
            console.LogInfo("Shutting down editor plugins...");
            m_pluginManager.ShutdownAll();
            m_pluginLifecycleStarted = false;
            console.LogSuccess("Editor plugins shutdown complete");
        }

        if (m_ui)
        {
            console.LogInfo("Shutting down EditorUI...");
            m_ui->Shutdown();
            m_ui.reset();
            console.LogSuccess("EditorUI shutdown complete");
        }

        // Window manager shutdown after EditorUI so any auto-save picks
        // up the final panel state.
        if (m_windowManagerInitialized)
        {
            console.LogInfo("Shutting down window manager...");
            // Persist to disk before Shutdown() tears the manager down; the
            // in-memory "__autosave__" entry alone never survives the process.
            if (!EditorWindowManager::GetInstance().SaveCurrentLayoutToFile(WindowLayoutFilePath()))
                console.LogWarning("Window layout could not be written to disk");
            EditorWindowManager::GetInstance().Shutdown();
            m_windowManagerInitialized = false;
            console.LogSuccess("Window manager shutdown complete");
        }

        // Cleanup ImGui
        if (m_imguiInitialized)
        {
            console.LogInfo("Cleaning up Dear ImGui...");
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            m_imguiInitialized = false;
            console.LogSuccess("Dear ImGui cleanup complete");
        }

        // Cleanup DirectX
        console.LogInfo("Cleaning up DirectX 11...");
        m_rtv.Reset();
        m_context.Reset();
        m_device.Reset();
        m_swapChain.Reset();
        console.LogSuccess("DirectX 11 cleanup complete");

        // Cleanup window
        if (m_hwnd)
        {
            console.LogInfo("Destroying main window...");
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
            console.LogSuccess("Main window destroyed");
        }
        if (m_windowClassRegistered)
        {
            UnregisterClassW(L"SparkEditorWindow", GetModuleHandleW(nullptr));
            m_windowClassRegistered = false;
        }

        // Last, so a fault during teardown is still captured: this also restores
        // the previous unhandled-exception filter installed at startup.
        EditorCrashHandler::GetInstance().Shutdown();

        m_isInitialized = false;
        m_initializationStarted = false;
        console.LogSuccess("Enhanced editor shutdown complete");
    }

    LRESULT CALLBACK EditorApplication::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        // Forward messages to ImGui
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            return true;

        // Get application instance
        EditorApplication* app = EditorApplication::s_instance;
        if (!app && msg == WM_CREATE)
        {
            CREATESTRUCTW* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<EditorApplication*>(createStruct->lpCreateParams);
        }

        switch (msg)
        {
        case WM_SIZE:
            if (app && wParam != SIZE_MINIMIZED)
            {
                app->OnWindowResize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;

        case WM_DPICHANGED:
        {
            // Adopt the rectangle Windows suggests for the new monitor, then rescale the UI.
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (app)
            {
                app->OnDpiChanged(static_cast<float>(HIWORD(wParam)) / kBaselineDpi);
            }
            return 0;
        }

        case WM_CLOSE:
            if (app && app->OnShutdownRequested())
            {
                app->RequestExit();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

} // namespace SparkEditor

// =========================================================================
// Linux/SDL2+OpenGL platform implementation
// =========================================================================

#endif // _WIN32
