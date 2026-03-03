#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
// SparkEngine.cpp – unified entry point with Win32 boilerplate + classic subsystems
// ===================================================================================

#include "framework.h"
#include "SparkEngine.h"
#include "Utils/Assert.h"             // <-- custom assert
#include "Utils/SparkError.h"         // <-- enhanced error handling

#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <memory>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cstdio>                     // for swprintf_s
#include <algorithm>                  // for std::transform
#include <sstream>                    // for stringstream
#include <chrono>                     // for timestamp in debug command
#include <cmath>                      // for isfinite

#include "../Graphics/GraphicsEngine.h"
#include "../Game/Game.h"
#include "../Input/InputManager.h"
#include "../Utils/Timer.h"
#include "../Game/Console.h"
#include "Utils/CrashHandler.h"
#include "Utils/D3DUtils.h"
#include "../Utils/SparkConsole.h"  // **SIMPLE: Use simple console - reliable and works**

// -----------------------------------------------------------------------------
// Globals & constants
// -----------------------------------------------------------------------------
constexpr int MAX_LOADSTRING = 100;

HINSTANCE                          g_hInst;
WCHAR                              g_szTitle[MAX_LOADSTRING];
WCHAR                              g_szClass[MAX_LOADSTRING];

// Classic subsystems
std::unique_ptr<GraphicsEngine>    g_graphics;
std::unique_ptr<Game>              g_game;
std::unique_ptr<InputManager>      g_input;
std::unique_ptr<Timer>             g_timer;
Console                            g_console;

// Win32 forward declarations
ATOM                MyRegisterClass(HINSTANCE);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

// Enhanced console command registration
void RegisterGraphicsConsoleCommands();
void RegisterGameConsoleCommands();

// ===================================================================================
//                                    wWinMain
// ===================================================================================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int nCmdShow)
{
    ASSERT(hInstance != nullptr);

    // 1. Crash handler
    CrashConfig crashCfg{};
    crashCfg.dumpPrefix = L"SparkCrash";
    crashCfg.uploadURL = "";  // Crash upload disabled — set a real endpoint before release
    crashCfg.captureScreenshot = true;
    crashCfg.captureSystemInfo = true;
    crashCfg.captureAllThreads = true;
    crashCfg.zipBeforeUpload = true;
    crashCfg.triggerCrashOnAssert = false;  // NEW: Don't crash on assertions during development
    InstallCrashHandler(crashCfg);

    // 2. Class & window title
    ASSERT(MAX_LOADSTRING <= _countof(g_szClass) && MAX_LOADSTRING <= _countof(g_szTitle));
    wcscpy_s(g_szClass, MAX_LOADSTRING, L"SparkEngineWindowClass");
    wcscpy_s(g_szTitle, MAX_LOADSTRING, L"Spark Engine");

    // 3. Register window class
    ATOM cls = MyRegisterClass(hInstance);
    ASSERT_MSG(cls != 0, "MyRegisterClass failed");
    if (cls == 0)
    {
        MessageBoxW(nullptr, L"RegisterClassExW failed", L"Fatal Error", MB_ICONERROR);
        return -1;
    }

    // 4. Create window & init subsystems
    if (!InitInstance(hInstance, nCmdShow))
        return -1;

    // 5. Message loop + tick (timer is now already initialized in InitInstance)
    HACCEL accel = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_SparkEngine));
    MSG msg = {};
    ASSERT(g_timer);  // Should already be initialized

    // Log that main loop is starting
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Starting main engine loop...");

    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            // Console first
            if (msg.message == WM_CHAR &&
                g_console.HandleChar(static_cast<wchar_t>(msg.wParam)))
                continue;
            if (msg.message == WM_KEYDOWN &&
                g_console.HandleKeyDown(msg.wParam))
                continue;

            if (!TranslateAccelerator(msg.hwnd, accel, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        else
        {
            // **SOLUTION: Fixed main loop - NO double BeginFrame/EndFrame**
            float dt = g_timer ? g_timer->GetDeltaTime() : 0.016f;

            // Input update
            if (g_input) g_input->Update();
            
            // Game update (only if console not visible)
            if (g_game && !g_console.IsVisible()) {
                g_game->Update(dt);
            }

            // **CRITICAL FIX: Single, clean rendering - Game::Render() handles ALL frame management**
            if (g_game) {
                g_game->Render();  // Game::Render() calls BeginFrame/EndFrame internally
            } else if (g_graphics) {
                // Fallback: If no game, just clear the screen
                g_graphics->BeginFrame();
                g_graphics->EndFrame();
            }
            // **NO graphics BeginFrame/EndFrame here - that was causing double rendering!**

            // Console update (non-rendering operations only)
            console.Update();
        }
    }

    // Shutdown console
    console.LogInfo("Shutting down engine main loop...");
    console.Shutdown();

    return static_cast<int>(msg.wParam);
}

// ===================================================================================
//                       Win32 boilerplate
// ===================================================================================
ATOM MyRegisterClass(HINSTANCE hInst)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SparkEngine));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDC_SparkEngine);
    wc.lpszClassName = g_szClass;
    wc.hIconSm = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SMALL));

    ATOM result = RegisterClassExW(&wc);
    ASSERT_MSG(result != 0, "RegisterClassExW returned zero");
    return result;
}

BOOL InitInstance(HINSTANCE hInst, int nCmdShow)
{
    ASSERT(hInst != nullptr);
    g_hInst = hInst;

    // Create window
    HWND hWnd = CreateWindowW(
        g_szClass, g_szTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 1280, 720,
        nullptr, nullptr, hInst, nullptr);

    if (!hWnd)
    {
        DWORD err = GetLastError();
        wchar_t buf[256];
        swprintf_s(buf, L"CreateWindowW failed (0x%08X)", static_cast<unsigned>(err));
        MessageBoxW(nullptr, buf, L"Fatal Error", MB_ICONERROR);
        return FALSE;
    }

    // 1. Initialize Console FIRST (before external console)
    g_console.Initialize(1280, 720);

    // 2. Initialize Timer EARLY (needed by console commands)
    g_timer = std::make_unique<Timer>();
    ASSERT(g_timer);

    // 3. Initialize Graphics Engine
    g_graphics = std::make_unique<GraphicsEngine>();
    ASSERT(g_graphics);
    HRESULT hr = g_graphics->Initialize(hWnd);
    if (FAILED(hr))
    {
        wchar_t buf[256];
        swprintf_s(buf, L"Graphics initialization failed (HR=0x%08X)", static_cast<unsigned>(hr));
        MessageBoxW(hWnd, buf, L"Fatal Error", MB_ICONERROR);
        return FALSE;
    }

    // 4. Initialize Input Manager
    g_input = std::make_unique<InputManager>();
    ASSERT(g_input);
    g_input->Initialize(hWnd);

    // 5. Initialize Game System
    g_game = std::make_unique<Game>();
    ASSERT(g_game);
    hr = g_game->Initialize(g_graphics.get(), g_input.get());
    if (FAILED(hr))
    {
        wchar_t buf[256];
        swprintf_s(buf, L"Game initialization failed (HR=0x%08X)", static_cast<unsigned>(hr));
        MessageBoxW(nullptr, buf, L"Fatal Error", MB_ICONERROR);
        return FALSE;
    }

    // **PROFESSIONAL: Initialize SparkConsole and register comprehensive commands**
    auto& console = Spark::SimpleConsole::GetInstance();
    if (console.Initialize()) {
        console.LogSuccess("Enhanced Spark Engine initialized with AAA features and console integration");
        
        // Register enhanced graphics console commands
        RegisterGraphicsConsoleCommands();
        
        // Register enhanced game console commands  
        RegisterGameConsoleCommands();
        
        // The console now has comprehensive built-in commands
        console.LogInfo("Advanced debugging system active with enhanced features");
        console.LogInfo("Type 'help' for complete command reference");
        console.LogInfo("Enhanced features include shader hot-reload, advanced graphics settings, and real-time debugging");
    } else {
        // Fallback to OutputDebugString if console fails
        OutputDebugStringW(L"Failed to initialize enhanced development console\n");
    }

    // 7. Show window and activate
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

    // Log successful initialization
    console.LogSuccess("Enhanced SparkEngine initialization completed successfully with AAA features");
    console.LogInfo("Main window is now visible and ready for interaction");
    console.LogInfo("Press ` (tilde) key to toggle engine console, or use the separate console window");
    console.LogInfo("Enhanced Features: Advanced Graphics Pipeline, Shader Hot-Reload, Real-time Performance Monitoring");

    return TRUE;
}

// ===================================================================================
//                          Window procedure
// ===================================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wParam == VK_OEM_3)
        {
            g_console.Toggle();
            return 0;
        }
        if (g_input) g_input->HandleMessage(msg, wParam, lParam);
        break;

    case WM_KEYUP:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        if (g_input) g_input->HandleMessage(msg, wParam, lParam);
        break;

    case WM_SIZE:
        if (g_graphics)
            g_graphics->OnResize(LOWORD(lParam), HIWORD(lParam));
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

INT_PTR CALLBACK About(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
{
    if (msg == WM_COMMAND &&
        (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
    {
        EndDialog(hDlg, LOWORD(wParam));
        return TRUE;
    }
    return FALSE;
}

// ===================================================================================
//                    ENHANCED CONSOLE COMMAND REGISTRATION
// ===================================================================================

/**
 * @brief Register enhanced graphics console commands
 */
void RegisterGraphicsConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    
    // Graphics pipeline control
    console.RegisterCommand("gfx_pipeline", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: gfx_pipeline <forward|deferred>";
        if (!g_graphics) return "Graphics engine not available";
        
        try {
            if (args[0] == "forward") {
                g_graphics->Console_SetRenderingPipeline(RenderingPipeline::Forward);
                return "Switched to Forward rendering pipeline";
            } else if (args[0] == "deferred") {
                g_graphics->Console_SetRenderingPipeline(RenderingPipeline::Deferred);
                return "Switched to Deferred rendering pipeline";
            }
            return "Invalid pipeline. Use 'forward' or 'deferred'";
        } catch (...) {
            return "Advanced pipeline control not available in this build";
        }
    }, "Set graphics rendering pipeline");

    // HDR control
    console.RegisterCommand("gfx_hdr", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: gfx_hdr <on|off>";
        if (!g_graphics) return "Graphics engine not available";
        
        try {
            bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
            g_graphics->Console_SetHDR(enable);
            return enable ? "HDR rendering enabled" : "HDR rendering disabled";
        } catch (...) {
            return "HDR control not available in this build";
        }
    }, "Enable/disable HDR rendering");

    // VSync control
    console.RegisterCommand("gfx_vsync", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: gfx_vsync <on|off>";
        if (!g_graphics) return "Graphics engine not available";
        
        bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
        g_graphics->Console_SetVSync(enable);
        return enable ? "VSync enabled" : "VSync disabled";
    }, "Enable/disable VSync");

    // Wireframe mode
    console.RegisterCommand("gfx_wireframe", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: gfx_wireframe <on|off>";
        if (!g_graphics) return "Graphics engine not available";
        
        bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
        g_graphics->Console_SetWireframeMode(enable);
        return enable ? "Wireframe mode enabled" : "Wireframe mode disabled";
    }, "Enable/disable wireframe rendering");

    // Graphics metrics
    console.RegisterCommand("gfx_metrics", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) return "Graphics engine not available";
        
        // Try to get advanced metrics, fallback to basic metrics if not available
        try {
            auto metrics = g_graphics->Console_GetMetrics();
            std::stringstream ss;
            ss << "=== Enhanced Graphics Metrics ===\n";
            ss << "Frame Time: " << metrics.frameTime << "ms\n";
            ss << "Render Time: " << metrics.renderTime << "ms\n";
            ss << "Present Time: " << metrics.presentTime << "ms\n";
            ss << "FPS: " << metrics.fps << "\n";
            ss << "Draw Calls: " << metrics.drawCalls << "\n";
            ss << "Triangles: " << metrics.triangles << "\n";
            ss << "Vertices: " << metrics.vertices << "\n";
            ss << "GPU Usage: " << metrics.gpuUsage << "%\n";
            ss << "GPU Memory: " << (metrics.totalGPUMemory / 1024 / 1024) << " MB\n";
            return ss.str();
        } catch (...) {
            // Fallback to basic metrics
            auto metrics = g_graphics->Console_GetMetrics();
            std::stringstream ss;
            ss << "=== Basic Graphics Metrics ===\n";
            ss << "Frame Time: " << metrics.frameTime << "ms\n";
            ss << "Render Time: " << metrics.renderTime << "ms\n";
            ss << "Present Time: " << metrics.presentTime << "ms\n";
            ss << "Draw Calls: " << metrics.drawCalls << "\n";
            ss << "Triangles: " << metrics.triangles << "\n";
            ss << "Vertices: " << metrics.vertices << "\n";
            ss << "VRAM Usage: " << (metrics.bufferMemory / 1024 / 1024) << " MB\n";
            ss << "VSync: " << (metrics.vsyncEnabled ? "On" : "Off") << "\n";
            ss << "Wireframe: " << (metrics.wireframeMode ? "On" : "Off") << "\n";
            return ss.str();
        }
    }, "Display detailed graphics performance metrics");

    // Screenshot
    console.RegisterCommand("gfx_screenshot", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) return "Graphics engine not available";
        
        std::string filename = args.empty() ? "" : args[0];
        bool success = g_graphics->Console_TakeScreenshot(filename);
        return success ? "Screenshot saved successfully" : "Failed to save screenshot";
    }, "Take a screenshot");
}

/**
 * @brief Register enhanced game console commands
 */
void RegisterGameConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    
    // Time scale control
    console.RegisterCommand("game_timescale", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: game_timescale <scale>";
        if (!g_game) return "Game not available";

        try {
            float scale = std::stof(args[0]);
            g_game->SetTimeScale(scale);
            return "Time scale set to " + std::to_string(scale);
        } catch (const std::exception& e) {
            return std::string("Error: invalid number '") + args[0] + "' -- " + e.what();
        }
    }, "Set game time scale");

    // Player teleport
    console.RegisterCommand("player_tp", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 3) return "Usage: player_tp <x> <y> <z>";
        if (!g_game) return "Game not available";

        try {
            float x = std::stof(args[0]);
            float y = std::stof(args[1]);
            float z = std::stof(args[2]);
            g_game->TeleportPlayer(x, y, z);
            return "Player teleported to (" + args[0] + ", " + args[1] + ", " + args[2] + ")";
        } catch (const std::exception& e) {
            return std::string("Error: invalid coordinate -- ") + e.what();
        }
    }, "Teleport player to coordinates");

    // Spawn object
    console.RegisterCommand("spawn", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 4) return "Usage: spawn <type> <x> <y> <z>";
        if (!g_game) return "Game not available";

        try {
            std::string type = args[0];
            float x = std::stof(args[1]);
            float y = std::stof(args[2]);
            float z = std::stof(args[3]);
            bool success = g_game->SpawnObject(type, x, y, z);
            return success ? "Object spawned successfully" : "Failed to spawn object of type '" + type + "'";
        } catch (const std::exception& e) {
            return std::string("Error: invalid spawn arguments -- ") + e.what();
        }
    }, "Spawn an object at coordinates");

    // God mode
    console.RegisterCommand("god", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game) return "Game not available";
        
        bool enable = args.empty() ? true : (args[0] == "on" || args[0] == "true" || args[0] == "1");
        g_game->ApplyDebugSettings(enable, false, false);
        return enable ? "God mode enabled" : "God mode disabled";
    }, "Toggle god mode");

    // Noclip
    console.RegisterCommand("noclip", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game) return "Game not available";
        
        bool enable = args.empty() ? true : (args[0] == "on" || args[0] == "true" || args[0] == "1");
        g_game->ApplyDebugSettings(false, enable, false);
        return enable ? "Noclip enabled" : "Noclip disabled";
    }, "Toggle noclip mode");

    // Performance stats
    console.RegisterCommand("game_stats", [](const std::vector<std::string>& args) -> std::string {
        if (!g_game) return "Game not available";
        
        int drawCalls, triangles, activeObjects;
        g_game->GetPerformanceStats(drawCalls, triangles, activeObjects);
        
        std::stringstream ss;
        ss << "=== Game Performance Stats ===\n";
        ss << "Draw Calls: " << drawCalls << "\n";
        ss << "Triangles: " << triangles << "\n";
        ss << "Active Objects: " << activeObjects << "\n";
        ss << "Time Scale: " << g_game->GetTimeScale() << "\n";
        ss << "Paused: " << (g_game->IsPaused() ? "Yes" : "No") << "\n";
        return ss.str();
    }, "Display game performance statistics");
}
#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Non-Windows: Minimal main entry point for build verification
// ============================================================================
#ifndef SPARK_PLATFORM_WINDOWS
#include <iostream>

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    std::cout << "SparkEngine (Linux build)" << std::endl;
    std::cout << "This is a cross-platform build target." << std::endl;
    std::cout << "Full engine features require the Windows DirectX 11 runtime." << std::endl;
    return 0;
}
#endif // !SPARK_PLATFORM_WINDOWS
