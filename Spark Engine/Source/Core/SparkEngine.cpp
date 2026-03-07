/**
 * @file SparkEngine.cpp
 * @brief SparkEngine executable entry point - loads game modules dynamically
 *
 * SparkEngine is the runtime host. It creates the window, initializes engine
 * systems (graphics, input, timer), then loads a game DLL via GameModuleLoader.
 * The game DLL implements IGameModule and provides all game-specific logic.
 *
 * Architecture: Engine (exe) -> loads -> Game (DLL)
 * Similar to Unreal Engine's module loading or Unity's player runtime.
 */

#include "Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "framework.h"
#include "SparkEngine.h"
#include "GameModuleLoader.h"
#include "IGameModule.h"
#include "Utils/Assert.h"
#include "Utils/SparkError.h"

#include <Windows.h>
#include <memory>
#include <DirectXMath.h>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <cmath>
#include <filesystem>

#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "Utils/Timer.h"
#include "Utils/CrashHandler.h"
#include "Utils/D3DUtils.h"
#include "Utils/SparkConsole.h"

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------
constexpr int MAX_LOADSTRING = 100;

HINSTANCE                          g_hInst;
WCHAR                              g_szTitle[MAX_LOADSTRING];
WCHAR                              g_szClass[MAX_LOADSTRING];

// Engine subsystems
std::unique_ptr<GraphicsEngine>    g_graphics;
std::unique_ptr<InputManager>      g_input;
std::unique_ptr<Timer>             g_timer;
std::unique_ptr<GameModuleLoader>  g_moduleLoader;

// Win32 forward declarations
ATOM                MyRegisterClass(HINSTANCE);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

// Console command registration (engine-side only)
void RegisterEngineConsoleCommands();

/**
 * @brief Find the game module DLL to load
 *
 * Searches for game DLLs in the following order:
 * 1. Command line argument: -game <path>
 * 2. SparkGame.dll in the executable directory
 * 3. Any .dll matching *Game*.dll in the executable directory
 */
static std::string FindGameModule(LPWSTR cmdLine)
{
    // Check command line for -game argument
    std::wstring cmd(cmdLine);
    size_t pos = cmd.find(L"-game ");
    if (pos != std::wstring::npos)
    {
        size_t start = pos + 6;
        size_t end = cmd.find(L' ', start);
        std::wstring wpath = cmd.substr(start, end - start);
        std::string path(wpath.begin(), wpath.end());
        if (std::filesystem::exists(path))
            return path;
    }

    // Look for SparkGame.dll next to the engine exe
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

    auto defaultPath = exeDir / "SparkGame.dll";
    if (std::filesystem::exists(defaultPath))
        return defaultPath.string();

    // Search for any *Game*.dll
    for (auto& entry : std::filesystem::directory_iterator(exeDir))
    {
        if (entry.is_regular_file())
        {
            auto fn = entry.path().filename().string();
            if (fn.find("Game") != std::string::npos &&
                entry.path().extension() == ".dll")
            {
                return entry.path().string();
            }
        }
    }

    return "";
}

// ===================================================================================
//                                    wWinMain
// ===================================================================================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    ASSERT(hInstance != nullptr);

    // 1. Crash handler
    CrashConfig crashCfg{};
    crashCfg.dumpPrefix = L"SparkCrash";
    crashCfg.uploadURL = "";
    crashCfg.captureScreenshot = true;
    crashCfg.captureSystemInfo = true;
    crashCfg.captureAllThreads = true;
    crashCfg.zipBeforeUpload = true;
    crashCfg.triggerCrashOnAssert = false;
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

    // 4. Create window & init engine subsystems
    if (!InitInstance(hInstance, nCmdShow))
        return -1;

    // 5. Load game module DLL
    g_moduleLoader = std::make_unique<GameModuleLoader>();
    std::string gameDllPath = FindGameModule(lpCmdLine);

    auto& console = Spark::SimpleConsole::GetInstance();

    IGameModule* gameModule = nullptr;
    if (!gameDllPath.empty())
    {
        if (g_moduleLoader->Load(gameDllPath))
        {
            gameModule = g_moduleLoader->GetModule();
            if (gameModule && !gameModule->Initialize(g_graphics.get(), g_input.get()))
            {
                console.LogError("Game module Initialize() failed");
                gameModule = nullptr;
            }
        }
    }
    else
    {
        console.LogWarning("No game module found. Running engine-only mode.");
        console.LogInfo("Place a game DLL (e.g. SparkGame.dll) next to the engine executable,");
        console.LogInfo("or use -game <path> on the command line.");
    }

    if (gameModule)
    {
        // Update window title with game name
        std::wstring title = L"Spark Engine - ";
        std::string gameName = gameModule->GetGameName();
        title.append(gameName.begin(), gameName.end());
        HWND hWnd = FindWindowW(g_szClass, g_szTitle);
        if (hWnd) SetWindowTextW(hWnd, title.c_str());
    }

    // 6. Register engine console commands
    RegisterEngineConsoleCommands();

    // 7. Message loop + tick
    HACCEL accel = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_SparkEngine));
    MSG msg = {};
    ASSERT(g_timer);

    console.LogInfo("Starting main engine loop...");

    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (!TranslateAccelerator(msg.hwnd, accel, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        else
        {
            float dt = g_timer ? g_timer->GetDeltaTime() : 0.016f;

            if (g_input) g_input->Update();

            gameModule = g_moduleLoader ? g_moduleLoader->GetModule() : nullptr;

            if (gameModule)
            {
                if (!gameModule->IsPaused())
                    gameModule->Update(dt);
                gameModule->Render();
            }
            else if (g_graphics)
            {
                // Engine-only mode: just clear and present
                g_graphics->BeginFrame();
                g_graphics->EndFrame();
            }

            console.Update();
        }
    }

    // 8. Shutdown
    console.LogInfo("Shutting down...");

    if (g_moduleLoader)
    {
        auto* mod = g_moduleLoader->GetModule();
        if (mod) mod->Shutdown();
        g_moduleLoader->Unload();
        g_moduleLoader.reset();
    }

    g_input.reset();
    g_graphics.reset();
    g_timer.reset();

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

    g_timer = std::make_unique<Timer>();
    ASSERT(g_timer);

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

    g_input = std::make_unique<InputManager>();
    ASSERT(g_input);
    g_input->Initialize(hWnd);

    auto& console = Spark::SimpleConsole::GetInstance();
    if (console.Initialize()) {
        console.LogSuccess("Spark Engine runtime initialized");
        console.LogInfo("Type 'help' for complete command reference");
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

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
    case WM_KEYUP:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        if (g_input) g_input->HandleMessage(msg, wParam, lParam);
        break;

    case WM_SIZE:
        if (g_graphics)
            g_graphics->OnResize(LOWORD(lParam), HIWORD(lParam));
        if (g_moduleLoader && g_moduleLoader->GetModule())
            g_moduleLoader->GetModule()->OnResize(LOWORD(lParam), HIWORD(lParam));
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
//                    ENGINE CONSOLE COMMANDS
// ===================================================================================
void RegisterEngineConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand("gfx_vsync", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: gfx_vsync <on|off>";
        if (!g_graphics) return "Graphics engine not available";
        bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
        g_graphics->Console_SetVSync(enable);
        return enable ? "VSync enabled" : "VSync disabled";
    }, "Enable/disable VSync");

    console.RegisterCommand("gfx_wireframe", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: gfx_wireframe <on|off>";
        if (!g_graphics) return "Graphics engine not available";
        bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
        g_graphics->Console_SetWireframeMode(enable);
        return enable ? "Wireframe mode enabled" : "Wireframe mode disabled";
    }, "Enable/disable wireframe rendering");

    console.RegisterCommand("gfx_metrics", [](const std::vector<std::string>&) -> std::string {
        if (!g_graphics) return "Graphics engine not available";
        try {
            auto metrics = g_graphics->Console_GetMetrics();
            std::stringstream ss;
            ss << "=== Graphics Metrics ===\n";
            ss << "FPS: " << metrics.fps << "\n";
            ss << "Frame Time: " << metrics.frameTime << "ms\n";
            ss << "Draw Calls: " << metrics.drawCalls << "\n";
            ss << "Triangles: " << metrics.triangles << "\n";
            return ss.str();
        } catch (...) {
            return "Metrics not available";
        }
    }, "Display graphics performance metrics");

    console.RegisterCommand("gfx_screenshot", [](const std::vector<std::string>& args) -> std::string {
        if (!g_graphics) return "Graphics engine not available";
        std::string filename = args.empty() ? "" : args[0];
        bool success = g_graphics->Console_TakeScreenshot(filename);
        return success ? "Screenshot saved" : "Failed to save screenshot";
    }, "Take a screenshot");

    console.RegisterCommand("module_info", [](const std::vector<std::string>&) -> std::string {
        if (!g_moduleLoader || !g_moduleLoader->IsLoaded())
            return "No game module loaded";
        auto* mod = g_moduleLoader->GetModule();
        std::stringstream ss;
        ss << "Game Module: " << mod->GetGameName() << "\n";
        ss << "Version: " << mod->GetGameVersion() << "\n";
        ss << "DLL Path: " << g_moduleLoader->GetModulePath() << "\n";
        return ss.str();
    }, "Show loaded game module info");

    console.RegisterCommand("module_reload", [](const std::vector<std::string>&) -> std::string {
        if (!g_moduleLoader || !g_moduleLoader->IsLoaded())
            return "No game module loaded";
        if (g_moduleLoader->Reload())
        {
            auto* mod = g_moduleLoader->GetModule();
            if (mod && mod->Initialize(g_graphics.get(), g_input.get()))
                return "Game module reloaded and initialized";
            return "Game module reloaded but Initialize() failed";
        }
        return "Failed to reload game module";
    }, "Hot-reload the game module DLL");
}

#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Non-Windows: Minimal main entry point
// ============================================================================
#ifndef SPARK_PLATFORM_WINDOWS
#include "SparkEngine.h"
#include "GameModuleLoader.h"
#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "Utils/Timer.h"
#include <iostream>

std::unique_ptr<GraphicsEngine>    g_graphics;
std::unique_ptr<InputManager>      g_input;
std::unique_ptr<Timer>             g_timer;
std::unique_ptr<GameModuleLoader>  g_moduleLoader;

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    std::cout << "Spark Engine (Linux build)" << std::endl;
    std::cout << "This is a cross-platform build target." << std::endl;
    std::cout << "Full runtime features require the Windows DirectX 11 runtime." << std::endl;
    return 0;
}
#endif // !SPARK_PLATFORM_WINDOWS
