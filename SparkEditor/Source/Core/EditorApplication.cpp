/**
 * @file EditorApplication.cpp
 * @brief Implementation of the enhanced editor application class
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorApplication.h"
#include "EditorUI.h"
#include "EditorFonts.h"
#include "Core/FaultIsolation.h"
#include "EditorCrashHandler.h"
#include "EditorPluginManager.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include <memory>
#include <iostream>
#include <stdexcept>

// Dear ImGui includes
#include <imgui.h>

#ifdef _WIN32
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <dxgi.h>
#include <Windows.h>
using Microsoft::WRL::ComPtr;

// External ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#else
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <SDL.h>
#include <SDL_opengl.h>
#endif

#include <chrono>
#include <filesystem>

namespace SparkEditor
{

    // Static instance pointer for message handling
    static EditorApplication* s_instance = nullptr;

    EditorApplication::EditorApplication()
        : m_startTime(std::chrono::high_resolution_clock::now()), m_lastFrameTime(m_startTime)
    {
        s_instance = this;
        std::cout << "Enhanced EditorApplication constructed\n";
    }

    EditorApplication::~EditorApplication()
    {
        std::cout << "EditorApplication destructor called\n";
        if (m_isInitialized)
        {
            Shutdown();
        }
        s_instance = nullptr;
    }

    bool EditorApplication::Initialize(const EditorConfig& config)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Initializing Enhanced Spark Engine Editor...");

        m_config = config;
        m_windowWidth = config.windowWidth;
        m_windowHeight = config.windowHeight;

        // Create main editor window
        console.LogInfo("Creating main editor window...");
        if (!CreateMainWindow(config))
        {
            console.LogError("Failed to create main editor window");
            return false;
        }
        console.LogSuccess("Main window created successfully (" + std::to_string(m_windowWidth) + "x" +
                           std::to_string(m_windowHeight) + ")");

        // Initialize graphics backend
        console.LogInfo("Initializing graphics backend...");
        if (!InitializeGraphics())
        {
            console.LogError("Failed to initialize graphics backend");
            return false;
        }
        console.LogSuccess("Graphics backend initialized successfully");

        // Initialize Dear ImGui
        console.LogInfo("Initializing Dear ImGui...");
        if (!InitializeImGui())
        {
            console.LogError("Failed to initialize Dear ImGui");
            return false;
        }
        console.LogSuccess("Dear ImGui initialized successfully");

        // Initialize EditorUI (this creates all panels including console)
        console.LogInfo("Initializing EditorUI...");
        m_ui = std::make_unique<EditorUI>();
        if (!m_ui->Initialize(config))
        { // Pass config parameter
            console.LogError("Failed to initialize EditorUI");
            return false;
        }
        // Give EditorUI access to the plugin manager for menu bar rendering
        m_ui->SetPluginManager(&m_pluginManager);
        console.LogSuccess("EditorUI initialized successfully");

#ifdef _WIN32
        // Pass the DirectX device to panels that need it (Scene View)
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Editor, m_device.Get(), false);
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Editor, m_context.Get(), false);
        if (m_device && m_context)
        {
            m_ui->SetGraphicsDevice(m_device.Get(), m_context.Get());
        }
#endif

        // Initialize plugin system
        console.LogInfo("Initializing editor plugins...");
        m_pluginManager.InitializeAll(this);
        console.LogSuccess("Editor plugins initialized");

        m_isInitialized = true;
        m_isRunning = true;

        console.LogSuccess("Enhanced Editor initialization complete");
        console.LogInfo("SparkEditor is now ready for use");
        console.LogInfo("All editor operations will be logged to external console");

        return true;
    }

    // =========================================================================
    // Windows platform implementation
    // =========================================================================
#ifdef _WIN32

    bool EditorApplication::CreateMainWindow(const EditorConfig& config)
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        // Register window class
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"SparkEditorWindow";

        if (!RegisterClassExW(&wc))
        {
            console.LogError("Failed to register window class");
            return false;
        }
        console.LogInfo("Window class registered successfully");

        // Create window
        m_hwnd = CreateWindowExW(0,                         // dwExStyle
                                 L"SparkEditorWindow",      // lpClassName
                                 L"Spark Engine Editor",    // lpWindowName
                                 WS_OVERLAPPEDWINDOW,       // dwStyle
                                 CW_USEDEFAULT,             // X
                                 CW_USEDEFAULT,             // Y
                                 config.windowWidth,        // nWidth
                                 config.windowHeight,       // nHeight
                                 nullptr,                   // hWndParent
                                 nullptr,                   // hMenu
                                 GetModuleHandleW(nullptr), // hInstance
                                 nullptr                    // lpParam
        );

        if (!m_hwnd)
        {
            DWORD error = GetLastError();
            console.LogError("Failed to create window (Error: " + std::to_string(error) + ")");
            return false;
        }

        console.LogInfo("Window created successfully");

        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);

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

        // Enable keyboard controls and docking
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        // Docking configuration
        io.ConfigDockingWithShift = false; // Dock without holding Shift
        io.ConfigWindowsResizeFromEdges = true;

        // Load custom fonts before backend initialization
        EditorFonts::LoadFonts(15.0f);

        // Setup Platform/Renderer backends
        if (!ImGui_ImplWin32_Init(m_hwnd))
        {
            std::cerr << "Failed to initialize ImGui Win32 backend\n";
            return false;
        }

        if (!ImGui_ImplDX11_Init(m_device.Get(), m_context.Get()))
        {
            std::cerr << "Failed to initialize ImGui DirectX 11 backend\n";
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
            console.LogCritical("EditorApplication::Run() called but editor not initialized!");
            return -1;
        }

        console.LogInfo("Starting enhanced editor main loop...");

        // Main message loop
        MSG msg = {};
        auto lastTime = std::chrono::high_resolution_clock::now();

        while (m_isRunning && msg.message != WM_QUIT)
        {
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
        // Clear render target
        float clearColor[4] = {0.45f, 0.55f, 0.60f, 1.00f};
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

        // Render ImGui
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present frame
        m_swapChain->Present(1, 0); // VSync enabled
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
            std::wstring wTitle(title.begin(), title.end());
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
        console.LogInfo("Shutting down editor plugins...");
        m_pluginManager.ShutdownAll();
        console.LogSuccess("Editor plugins shutdown complete");

        if (m_ui)
        {
            console.LogInfo("Shutting down EditorUI...");
            m_ui->Shutdown();
            m_ui.reset();
            console.LogSuccess("EditorUI shutdown complete");
        }

        // Cleanup ImGui
        console.LogInfo("Cleaning up Dear ImGui...");
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        console.LogSuccess("Dear ImGui cleanup complete");

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

        m_isInitialized = false;
        console.LogSuccess("Enhanced editor shutdown complete");
    }

    LRESULT CALLBACK EditorApplication::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        // Forward messages to ImGui
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
            return true;

        // Get application instance
        EditorApplication* app = s_instance;
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

    // =========================================================================
    // Linux/SDL2+OpenGL platform implementation
    // =========================================================================
#else

    bool EditorApplication::CreateMainWindow(const EditorConfig& config)
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
        {
            console.LogError("Failed to initialize SDL2: " + std::string(SDL_GetError()));
            return false;
        }
        console.LogInfo("SDL2 initialized successfully");

        // Set OpenGL attributes for OpenGL 3.3 Core Profile
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

        Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (config.startMaximized)
        {
            windowFlags |= SDL_WINDOW_MAXIMIZED;
        }

        m_window = SDL_CreateWindow("Spark Engine Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    config.windowWidth, config.windowHeight, windowFlags);

        if (!m_window)
        {
            console.LogError("Failed to create SDL2 window: " + std::string(SDL_GetError()));
            return false;
        }

        console.LogInfo("SDL2 window created successfully");
        console.LogInfo("Window is now visible and active");
        return true;
    }

    bool EditorApplication::InitializeGraphics()
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        std::cout << "Initializing OpenGL 3.3...\n";

        m_glContext = SDL_GL_CreateContext(m_window);
        if (!m_glContext)
        {
            console.LogError("Failed to create OpenGL context: " + std::string(SDL_GetError()));
            return false;
        }

        SDL_GL_MakeCurrent(m_window, m_glContext);
        SDL_GL_SetSwapInterval(1); // VSync

        std::cout << "OpenGL initialized: " << glGetString(GL_VERSION) << "\n";
        std::cout << "GLSL version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << "\n";
        return true;
    }

    bool EditorApplication::InitializeImGui()
    {
        std::cout << "Initializing Dear ImGui...\n";

        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();

        // Enable keyboard controls and docking
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        // Docking configuration
        io.ConfigDockingWithShift = false;
        io.ConfigWindowsResizeFromEdges = true;

        // Load custom fonts before backend initialization
        EditorFonts::LoadFonts(15.0f);

        // Setup Platform/Renderer backends
        if (!ImGui_ImplSDL2_InitForOpenGL(m_window, m_glContext))
        {
            std::cerr << "Failed to initialize ImGui SDL2 backend\n";
            return false;
        }

        const char* glslVersion = "#version 330 core";
        if (!ImGui_ImplOpenGL3_Init(glslVersion))
        {
            std::cerr << "Failed to initialize ImGui OpenGL3 backend\n";
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
            console.LogCritical("EditorApplication::Run() called but editor not initialized!");
            return -1;
        }

        console.LogInfo("Starting enhanced editor main loop...");

        auto lastTime = std::chrono::high_resolution_clock::now();

        while (m_isRunning)
        {
            // Calculate delta time
            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
            lastTime = currentTime;

            // Update console
            SPARK_GUARDED_UPDATE("EditorConsole", "Editor", { console.Update(); });

            // Process events
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
        return 0;
    }

    bool EditorApplication::ProcessMessages()
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);

            switch (event.type)
            {
            case SDL_QUIT:
                return false;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(m_window))
                {
                    if (OnShutdownRequested())
                    {
                        return false;
                    }
                }
                else if (event.window.event == SDL_WINDOWEVENT_RESIZED)
                {
                    OnWindowResize(event.window.data1, event.window.data2);
                }
                break;
            }
        }
        return true;
    }

    void EditorApplication::Render()
    {
        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Render UI
        if (m_ui)
        {
            m_ui->Render();
        }

        // Render editor plugin GUI
        m_pluginManager.RenderAll();

        // Render ImGui
        ImGui::Render();

        ImGuiIO& io = ImGui::GetIO();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(m_window);
    }

    void EditorApplication::OnWindowResize(int width, int height)
    {
        if (width <= 0 || height <= 0)
            return;

        m_windowWidth = width;
        m_windowHeight = height;
        glViewport(0, 0, width, height);
    }

    void EditorApplication::SetWindowTitle(const std::string& title)
    {
        if (m_window)
        {
            SDL_SetWindowTitle(m_window, title.c_str());
        }
    }

    void EditorApplication::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Shutting down enhanced editor...");

        m_isRunning = false;

        // Shutdown editor plugins before UI teardown
        console.LogInfo("Shutting down editor plugins...");
        m_pluginManager.ShutdownAll();
        console.LogSuccess("Editor plugins shutdown complete");

        if (m_ui)
        {
            console.LogInfo("Shutting down EditorUI...");
            m_ui->Shutdown();
            m_ui.reset();
            console.LogSuccess("EditorUI shutdown complete");
        }

        // Cleanup ImGui
        console.LogInfo("Cleaning up Dear ImGui...");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        console.LogSuccess("Dear ImGui cleanup complete");

        // Cleanup OpenGL and SDL
        console.LogInfo("Cleaning up OpenGL and SDL2...");
        if (m_glContext)
        {
            SDL_GL_DeleteContext(m_glContext);
            m_glContext = nullptr;
        }
        if (m_window)
        {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
        }
        SDL_Quit();
        console.LogSuccess("OpenGL and SDL2 cleanup complete");

        m_isInitialized = false;
        console.LogSuccess("Enhanced editor shutdown complete");
    }

#endif // _WIN32

    // =========================================================================
    // Shared implementation (cross-platform)
    // =========================================================================

    void EditorApplication::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        // Update UI system
        if (m_ui)
        {
            SPARK_GUARDED_UPDATE("EditorUI", "Editor", { m_ui->Update(deltaTime); });

            // Check if user requested exit via File > Exit
            if (m_ui->IsExitRequested())
            {
                RequestExit();
            }
        }

        // Update editor plugins
        m_pluginManager.UpdateAll(deltaTime);
    }

    void EditorApplication::UpdatePerformanceMetrics()
    {
        static float frameTimeAccumulator = 0.0f;
        static int frameCount = 0;
        static auto lastUpdateTime = std::chrono::high_resolution_clock::now();

        auto currentTime = std::chrono::high_resolution_clock::now();
        float frameTime = std::chrono::duration<float>(currentTime - m_lastFrameTime).count();
        m_lastFrameTime = currentTime;

        frameTimeAccumulator += frameTime;
        frameCount++;

        // Update metrics every 0.5 seconds
        if (std::chrono::duration<float>(currentTime - lastUpdateTime).count() >= 0.5f)
        {
            m_performanceMetrics.fps = frameCount / frameTimeAccumulator;
            m_performanceMetrics.frameTime = (frameTimeAccumulator / frameCount) * 1000.0f;

            frameTimeAccumulator = 0.0f;
            frameCount = 0;
            lastUpdateTime = currentTime;
        }
    }

    void EditorApplication::RequestExit()
    {
        std::cout << "Exit requested\n";
        m_isRunning = false;
    }

    EditorApplication::PerformanceMetrics EditorApplication::GetPerformanceMetrics() const
    {
        return m_performanceMetrics;
    }

    bool EditorApplication::OnShutdownRequested()
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        // Check for unsaved scene changes
        if (m_ui && m_ui->IsSceneModified())
        {
            console.LogWarning("Exiting with unsaved scene changes in: " + m_ui->GetCurrentSceneName());
            // Allow exit but log the warning so user sees it in console
        }

        // Auto-save the project on close if one is open
        if (m_ui)
        {
            auto* pm = m_ui->GetProjectManager();
            if (pm && pm->HasOpenProject())
            {
                pm->SaveProject();
                console.LogInfo("Project auto-saved on exit");
            }
        }

        return true;
    }

} // namespace SparkEditor
