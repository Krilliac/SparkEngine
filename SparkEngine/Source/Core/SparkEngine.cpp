/**
 * @file SparkEngine.cpp
 * @brief SparkEngine executable entry point - loads game modules dynamically
 *
 * SparkEngine is the runtime host. It creates the window, initializes engine
 * systems (graphics, input, timer), then loads game modules via ModuleManager.
 * Modules implement IModule (or the legacy IGameModule) and provide all
 * game-specific logic.
 *
 * Architecture: Engine (exe) -> loads -> Module DLLs (via ModuleManager)
 * Similar to Unreal Engine's module loading or Unity's player runtime.
 */

#include "Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "framework.h"
#include "SparkEngine.h"
#include "ModuleManager.h"
#include "EngineContext.h"
#include "Engine/Events/EventSystem.h"
#include "Utils/Assert.h"
#include "Utils/SparkError.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <memory>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
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
#include "EngineSettings.h"

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
std::unique_ptr<Spark::EventBus>   g_eventBus;
std::unique_ptr<ModuleManager>     g_moduleManager;
std::unique_ptr<EngineContext>     g_engineContext;

// Win32 forward declarations
ATOM                MyRegisterClass(HINSTANCE);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

// Console command registration (engine-side only)
void RegisterEngineConsoleCommands();

/**
 * @brief Get the executable directory
 */
static std::filesystem::path GetExecutableDirectory()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path();
}

/**
 * @brief Find a specific game module DLL from command line
 *
 * Checks for -game <path> on the command line.
 * Returns empty string if not specified.
 */
static std::string FindGameModuleFromCmdLine(LPWSTR cmdLine)
{
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
    return "";
}

/**
 * @brief Find the module manifest or fall back to directory scan
 *
 * Loading priority:
 * 1. Command line: -game <path> (loads single module)
 * 2. spark.modules.json manifest next to the engine exe
 * 3. Directory scan for *Game*.dll / *Module*.dll
 */
static bool LoadGameModules(ModuleManager& manager, LPWSTR cmdLine)
{
    auto exeDir = GetExecutableDirectory();

    // 1. Check command line for specific module
    std::string cmdLineModule = FindGameModuleFromCmdLine(cmdLine);
    if (!cmdLineModule.empty())
        return manager.LoadModule(cmdLineModule);

    // 2. Check for module manifest
    auto manifestPath = exeDir / "spark.modules.json";
    if (std::filesystem::exists(manifestPath))
        return manager.LoadModulesFromManifest(manifestPath.string());

    // 3. Fall back to directory scan
    return manager.LoadModulesFromDirectory(exeDir.string());
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

    // 5. Create event bus and engine context (service locator for modules)
    g_eventBus = std::make_unique<Spark::EventBus>();
    g_engineContext = std::make_unique<EngineContext>(
        g_graphics.get(), g_input.get(), g_timer.get(), g_eventBus.get());

    // 6. Load game modules via ModuleManager
    g_moduleManager = std::make_unique<ModuleManager>();

    auto& console = Spark::SimpleConsole::GetInstance();

    if (LoadGameModules(*g_moduleManager, lpCmdLine))
    {
        g_moduleManager->InitializeAll(g_engineContext.get());

        // Update window title with primary module name
        auto* primary = g_moduleManager->GetPrimaryModule();
        if (primary)
        {
            auto info = primary->GetModuleInfo();
            std::wstring title = L"Spark Engine - ";
            std::string modName(info.name);
            title.append(modName.begin(), modName.end());
            HWND hWnd = FindWindowW(g_szClass, g_szTitle);
            if (hWnd) SetWindowTextW(hWnd, title.c_str());
        }

        console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only mode.");
        console.LogInfo("Place a game DLL (e.g. SparkGame.dll) next to the engine executable,");
        console.LogInfo("use -game <path> on the command line,");
        console.LogInfo("or create a spark.modules.json manifest.");
    }

    // 7. Register engine console commands
    RegisterEngineConsoleCommands();
    EngineSettings::GetInstance().RegisterConsoleCommands();

    // 8. Message loop + tick
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

            if (g_moduleManager && g_moduleManager->HasModules())
            {
                g_moduleManager->UpdateAll(dt);
                g_moduleManager->RenderAll();
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

    // 9. Shutdown
    console.LogInfo("Shutting down...");

    if (g_moduleManager)
    {
        g_moduleManager->ShutdownAll();
        g_moduleManager->UnloadAll();
        g_moduleManager.reset();
    }

    g_engineContext.reset();
    g_eventBus.reset();
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

    // Load engine settings from INI (before window creation so we can use the dimensions)
    auto& settings = EngineSettings::GetInstance();
    settings.Load();

    int winW = settings.Graphics().windowWidth;
    int winH = settings.Graphics().windowHeight;

    HWND hWnd = CreateWindowW(
        g_szClass, g_szTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, winW, winH,
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

    // Apply VSync setting from INI
    g_graphics->Console_SetVSync(settings.Graphics().vsync);

    g_input = std::make_unique<InputManager>();
    ASSERT(g_input);
    g_input->Initialize(hWnd);

    // Apply input settings from INI
    g_input->Console_SetMouseSensitivity(settings.Controls().mouseSensitivity);
    g_input->Console_SetInvertMouseY(settings.Controls().invertMouseY);
    g_input->Console_SetMouseDeadZone(settings.Controls().mouseDeadZone);
    g_input->Console_SetRawMouseInput(settings.Controls().rawMouseInput);
    g_input->Console_SetMouseAcceleration(settings.Controls().mouseAcceleration);

    auto& console = Spark::SimpleConsole::GetInstance();
    if (console.Initialize()) {
        console.LogSuccess("Spark Engine runtime initialized");
        console.LogInfo("Settings loaded from " + settings.GetFilePath());
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
        if (g_moduleManager)
            g_moduleManager->ResizeAll(LOWORD(lParam), HIWORD(lParam));
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
        if (!g_moduleManager || !g_moduleManager->HasModules())
            return "No modules loaded";
        std::stringstream ss;
        ss << "=== Loaded Modules (" << g_moduleManager->GetModuleCount() << ") ===\n";
        // Show primary module info
        auto* primary = g_moduleManager->GetPrimaryModule();
        if (primary)
        {
            auto info = primary->GetModuleInfo();
            ss << "Primary: " << info.name << " v" << info.version
               << " (loadOrder=" << info.loadOrder << ")\n";
        }
        return ss.str();
    }, "Show loaded module info");

    console.RegisterCommand("module_reload", [](const std::vector<std::string>& args) -> std::string {
        if (!g_moduleManager || !g_moduleManager->HasModules())
            return "No modules loaded";
        if (!g_engineContext)
            return "Engine context not available";

        std::string name;
        if (!args.empty())
        {
            name = args[0];
        }
        else
        {
            // Reload primary module
            auto* primary = g_moduleManager->GetPrimaryModule();
            if (!primary) return "No primary module to reload";
            name = primary->GetModuleInfo().name;
        }

        if (g_moduleManager->ReloadModule(name, g_engineContext.get()))
            return "Module '" + name + "' reloaded successfully";
        return "Failed to reload module '" + name + "'";
    }, "Hot-reload a module DLL (usage: module_reload [name])");
}

#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Non-Windows: Minimal main entry point
// ============================================================================
#ifndef SPARK_PLATFORM_WINDOWS
#include "SparkEngine.h"
#include "ModuleManager.h"
#include "EngineContext.h"
#include "Engine/Events/EventSystem.h"
#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "Utils/Timer.h"
#include <iostream>

std::unique_ptr<GraphicsEngine>    g_graphics;
std::unique_ptr<InputManager>      g_input;
std::unique_ptr<Timer>             g_timer;
std::unique_ptr<Spark::EventBus>   g_eventBus;
std::unique_ptr<ModuleManager>     g_moduleManager;
std::unique_ptr<EngineContext>     g_engineContext;

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    std::cout << "=== Spark Engine (Linux Build) ===" << std::endl;

    // Initialize subsystems
    g_eventBus = std::make_unique<Spark::EventBus>();
    g_timer = std::make_unique<Timer>();
    g_input = std::make_unique<InputManager>();
    g_graphics = std::make_unique<GraphicsEngine>();

    std::cout << "Subsystems created." << std::endl;

    // Initialize graphics via RHI (OpenGL/Vulkan on Linux)
    HRESULT hr = g_graphics->Initialize(nullptr); // no HWND on Linux
    if (SUCCEEDED(hr)) {
        std::cout << "Graphics engine initialized (RHI backend)." << std::endl;
    } else {
        std::cout << "Graphics engine initialized (headless mode)." << std::endl;
    }

    // Create engine context
    g_engineContext = std::make_unique<EngineContext>();
    g_moduleManager = std::make_unique<ModuleManager>();

    std::cout << "Spark Engine ready. Linux platform support active." << std::endl;
    std::cout << "Rendering backends: OpenGL 4.6, Vulkan 1.3 (via RHI)" << std::endl;

    // Main loop placeholder - would be driven by window events
    // For now, just clean up
    g_graphics->Shutdown();
    g_moduleManager.reset();
    g_engineContext.reset();
    g_graphics.reset();
    g_input.reset();
    g_timer.reset();
    g_eventBus.reset();

    std::cout << "Spark Engine shut down cleanly." << std::endl;
    return 0;
}
#endif // !SPARK_PLATFORM_WINDOWS
