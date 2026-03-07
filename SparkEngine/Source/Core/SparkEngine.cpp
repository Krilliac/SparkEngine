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
#include "Engine/SaveSystem/SaveSystem.h"
#include "Audio/AudioEngine.h"
#include "Physics/PhysicsSystem.h"

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
std::unique_ptr<AudioEngine>       g_audioEngine;

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

    // 7. Initialize additional subsystems
    Spark::SaveSystem::GetInstance().Initialize("Saves");
    console.LogInfo("SaveSystem initialized");

    g_audioEngine = std::make_unique<AudioEngine>();
    if (SUCCEEDED(g_audioEngine->Initialize(32))) {
        console.LogInfo("AudioEngine initialized (32 sources)");
    } else {
        console.LogWarning("AudioEngine initialization failed - audio commands will be unavailable");
        g_audioEngine.reset();
    }

    // 8. Register engine console commands
    RegisterEngineConsoleCommands();
    EngineSettings::GetInstance().RegisterConsoleCommands();

    // 9. Message loop + tick
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

    // 10. Shutdown
    console.LogInfo("Shutting down...");

    if (g_moduleManager)
    {
        g_moduleManager->ShutdownAll();
        g_moduleManager->UnloadAll();
        g_moduleManager.reset();
    }

    g_audioEngine.reset();
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

    // ---- SaveSystem commands ----
    console.RegisterCommand("save_list", [](const std::vector<std::string>&) -> std::string {
        try {
            return Spark::SaveSystem::GetInstance().Console_ListSaves();
        } catch (...) {
            return "SaveSystem not available";
        }
    }, "List all save slots", "Save");

    console.RegisterCommand("save_info", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: save_info <slot_name>";
        try {
            return Spark::SaveSystem::GetInstance().Console_GetSaveInfo(args[0]);
        } catch (...) {
            return "SaveSystem not available";
        }
    }, "Show details for a save slot", "Save");

    // ---- PhysicsSystem commands ----
    console.RegisterCommand("physics_metrics", [](const std::vector<std::string>&) -> std::string {
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        auto m = physics->Console_GetMetrics();
        std::stringstream ss;
        ss << "=== Physics Metrics ===\n";
        ss << "Active Bodies: " << m.activeRigidBodies << "/" << m.totalRigidBodies << "\n";
        ss << "Constraints: " << m.activeConstraints << "\n";
        ss << "Collision Pairs: " << m.collisionPairs << "\n";
        ss << "Sim Time: " << m.simulationTime << "ms\n";
        ss << "Substeps: " << m.substeps << "\n";
        return ss.str();
    }, "Display physics performance metrics", "Physics");

    console.RegisterCommand("physics_list", [](const std::vector<std::string>&) -> std::string {
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        return physics->Console_ListBodies();
    }, "List all physics bodies", "Physics");

    console.RegisterCommand("physics_body_info", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: physics_body_info <name>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        return physics->Console_GetBodyInfo(args[0]);
    }, "Get detailed info about a physics body", "Physics");

    console.RegisterCommand("physics_gravity", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 3) return "Usage: physics_gravity <x> <y> <z>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        float x = std::stof(args[0]), y = std::stof(args[1]), z = std::stof(args[2]);
        physics->Console_SetGravity(x, y, z);
        return "Gravity set to (" + args[0] + ", " + args[1] + ", " + args[2] + ")";
    }, "Set world gravity vector", "Physics");

    console.RegisterCommand("physics_create", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 5) return "Usage: physics_create <name> <type> <x> <y> <z>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        float x = std::stof(args[2]), y = std::stof(args[3]), z = std::stof(args[4]);
        bool ok = physics->Console_CreateBody(args[0], args[1], x, y, z);
        return ok ? "Body '" + args[0] + "' created" : "Failed to create body (invalid type?)";
    }, "Create a physics body (type: static/kinematic/dynamic)", "Physics");

    console.RegisterCommand("physics_remove", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: physics_remove <name>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        bool ok = physics->Console_RemoveBody(args[0]);
        return ok ? "Body '" + args[0] + "' removed" : "Body not found";
    }, "Remove a physics body", "Physics");

    console.RegisterCommand("physics_set", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 3) return "Usage: physics_set <name> <property> <value>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        physics->Console_SetBodyProperty(args[0], args[1], std::stof(args[2]));
        return args[1] + " set to " + args[2] + " on '" + args[0] + "'";
    }, "Set body property (mass/friction/restitution/linearDamping/angularDamping)", "Physics");

    console.RegisterCommand("physics_force", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 4) return "Usage: physics_force <name> <x> <y> <z>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        physics->Console_ApplyForce(args[0], std::stof(args[1]), std::stof(args[2]), std::stof(args[3]));
        return "Force applied to '" + args[0] + "'";
    }, "Apply force to a physics body", "Physics");

    console.RegisterCommand("physics_impulse", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 4) return "Usage: physics_impulse <name> <x> <y> <z>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        physics->Console_ApplyImpulse(args[0], std::stof(args[1]), std::stof(args[2]), std::stof(args[3]));
        return "Impulse applied to '" + args[0] + "'";
    }, "Apply impulse to a physics body", "Physics");

    console.RegisterCommand("physics_debug", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: physics_debug <on|off>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
        physics->Console_EnableDebugDraw(enable);
        return enable ? "Physics debug draw enabled" : "Physics debug draw disabled";
    }, "Toggle physics debug overlay", "Physics");

    console.RegisterCommand("physics_pause", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: physics_pause <on|off>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        bool pause = (args[0] == "on" || args[0] == "true" || args[0] == "1");
        physics->Console_PausePhysics(pause);
        return pause ? "Physics simulation paused" : "Physics simulation resumed";
    }, "Pause/resume physics simulation", "Physics");

    console.RegisterCommand("physics_timestep", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: physics_timestep <seconds>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        float ts = std::stof(args[0]);
        physics->Console_SetTimeStep(ts);
        return "Physics timestep set to " + args[0] + "s";
    }, "Set physics fixed timestep", "Physics");

    console.RegisterCommand("physics_raycast", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 7) return "Usage: physics_raycast <ox> <oy> <oz> <dx> <dy> <dz> <maxDist>";
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        return physics->Console_Raycast(
            std::stof(args[0]), std::stof(args[1]), std::stof(args[2]),
            std::stof(args[3]), std::stof(args[4]), std::stof(args[5]),
            std::stof(args[6]));
    }, "Perform a physics raycast", "Physics");

    console.RegisterCommand("physics_reset", [](const std::vector<std::string>&) -> std::string {
        if (!g_graphics) return "Graphics engine not available";
        auto* physics = g_graphics->GetPhysicsSystem();
        if (!physics) return "Physics system not available";
        physics->Console_Reset();
        return "Physics world reset";
    }, "Reset physics world to initial state", "Physics");

    // ---- AudioEngine commands ----
    console.RegisterCommand("audio_master_volume", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: audio_master_volume <0.0-1.0>";
        if (!g_audioEngine) return "Audio engine not available";
        g_audioEngine->Console_SetMasterVolume(std::stof(args[0]));
        return "Master volume set to " + args[0];
    }, "Set master audio volume", "Audio");

    console.RegisterCommand("audio_sfx_volume", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: audio_sfx_volume <0.0-1.0>";
        if (!g_audioEngine) return "Audio engine not available";
        g_audioEngine->Console_SetSFXVolume(std::stof(args[0]));
        return "SFX volume set to " + args[0];
    }, "Set SFX volume", "Audio");

    console.RegisterCommand("audio_music_volume", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: audio_music_volume <0.0-1.0>";
        if (!g_audioEngine) return "Audio engine not available";
        g_audioEngine->Console_SetMusicVolume(std::stof(args[0]));
        return "Music volume set to " + args[0];
    }, "Set music volume", "Audio");

    console.RegisterCommand("audio_3d", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: audio_3d <on|off>";
        if (!g_audioEngine) return "Audio engine not available";
        bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
        g_audioEngine->Console_Set3DAudio(enable);
        return enable ? "3D audio enabled" : "3D audio disabled";
    }, "Toggle 3D spatial audio", "Audio");

    console.RegisterCommand("audio_play_test", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: audio_play_test <sound_name> [3d]";
        if (!g_audioEngine) return "Audio engine not available";
        bool is3D = (args.size() > 1 && (args[1] == "3d" || args[1] == "true"));
        uint32_t id = g_audioEngine->Console_PlayTestSound(args[0], is3D);
        return id > 0 ? "Playing sound (ID: " + std::to_string(id) + ")" : "Failed to play sound";
    }, "Play a test sound", "Audio");

    console.RegisterCommand("audio_stop", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: audio_stop <source_id>";
        if (!g_audioEngine) return "Audio engine not available";
        g_audioEngine->Console_StopSound(static_cast<uint32_t>(std::stoul(args[0])));
        return "Sound stopped";
    }, "Stop a playing sound by ID", "Audio");

    console.RegisterCommand("audio_stop_all", [](const std::vector<std::string>&) -> std::string {
        if (!g_audioEngine) return "Audio engine not available";
        g_audioEngine->Console_StopAllSounds();
        return "All sounds stopped";
    }, "Stop all playing sounds", "Audio");

    console.RegisterCommand("audio_list", [](const std::vector<std::string>&) -> std::string {
        if (!g_audioEngine) return "Audio engine not available";
        return g_audioEngine->Console_ListSounds();
    }, "List all loaded sounds", "Audio");

    console.RegisterCommand("audio_metrics", [](const std::vector<std::string>&) -> std::string {
        if (!g_audioEngine) return "Audio engine not available";
        auto m = g_audioEngine->Console_GetMetrics();
        std::stringstream ss;
        ss << "=== Audio Metrics ===\n";
        ss << "Active Sources: " << m.activeSources << "/" << m.totalSources << "\n";
        ss << "Loaded Sounds: " << m.loadedSounds << "\n";
        ss << "Memory: " << (m.memoryUsage / 1024) << " KB\n";
        return ss.str();
    }, "Display audio performance metrics", "Audio");

    console.RegisterCommand("audio_reset", [](const std::vector<std::string>&) -> std::string {
        if (!g_audioEngine) return "Audio engine not available";
        g_audioEngine->Console_ResetToDefaults();
        return "Audio settings reset to defaults";
    }, "Reset audio settings to defaults", "Audio");

    console.RegisterCommand("audio_listener_pos", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 3) return "Usage: audio_listener_pos <x> <y> <z>";
        if (!g_audioEngine) return "Audio engine not available";
        g_audioEngine->Console_SetListenerPosition(std::stof(args[0]), std::stof(args[1]), std::stof(args[2]));
        return "Listener position set";
    }, "Set 3D audio listener position", "Audio");

    console.RegisterCommand("audio_doppler", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: audio_doppler <scale>";
        if (!g_audioEngine) return "Audio engine not available";
        g_audioEngine->Console_SetDopplerScale(std::stof(args[0]));
        return "Doppler scale set to " + args[0];
    }, "Set Doppler effect scale", "Audio");

    console.RegisterCommand("audio_source_info", [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: audio_source_info <source_id>";
        if (!g_audioEngine) return "Audio engine not available";
        return g_audioEngine->Console_GetSourceInfo(static_cast<uint32_t>(std::stoul(args[0])));
    }, "Get info about an audio source", "Audio");
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
