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
#include <atomic>
#include <memory>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include <iostream>

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
#include "Graphics/GraphicsConsoleCommands.h"

// -----------------------------------------------------------------------------
// Missing module startup warnings
// -----------------------------------------------------------------------------
static void LogMissingModuleWarnings()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    int missingCount = 0;

#ifndef SPARK_BULLET_PHYSICS_AVAILABLE
    console.LogWarning(
        "[MISSING MODULE] Bullet Physics — rigid body simulation, collision detection, and raycasting are DISABLED.");
    console.LogWarning(
        "                 Physics-dependent features (gravity, projectiles, triggers) will not function.");
    ++missingCount;
#endif

#ifndef SPARK_MINIZ_AVAILABLE
    console.LogWarning("[MISSING MODULE] miniz — crash dump compression and save file compression are DISABLED.");
    console.LogWarning("                 CrashHandler is using a stub. Save files will not be compressed.");
    ++missingCount;
#endif

#ifndef SPARK_SDL2_AVAILABLE
#ifndef SPARK_PLATFORM_WINDOWS
    console.LogWarning("[MISSING MODULE] SDL2 — cross-platform windowing and input are DISABLED.");
    console.LogWarning("                 Install libsdl2-dev and rebuild with -DENABLE_SDL2=ON for windowed mode.");
    ++missingCount;
#endif
#endif

    if (missingCount > 0)
    {
        console.LogWarning("------------------------------------------------------------");
        console.LogWarning(std::to_string(missingCount) + " module(s) missing. Expect degraded functionality.");
        console.LogWarning("Run: git submodule update --init --recursive");
        console.LogWarning("Then rebuild to restore full engine features.");
        console.LogWarning("See README.md 'Dependencies' section for details.");
        console.LogWarning("------------------------------------------------------------");
    }
}

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------
constexpr int MAX_LOADSTRING = 100;

HINSTANCE g_hInst;
WCHAR g_szTitle[MAX_LOADSTRING];
WCHAR g_szClass[MAX_LOADSTRING];

// Engine subsystems
std::unique_ptr<GraphicsEngine> g_graphics;
std::unique_ptr<InputManager> g_input;
std::unique_ptr<Timer> g_timer;
std::unique_ptr<Spark::EventBus> g_eventBus;
std::unique_ptr<ModuleManager> g_moduleManager;
extern std::unique_ptr<EngineContext> g_engineContext; // defined in EngineContext.cpp
std::unique_ptr<AudioEngine> g_audioEngine;
std::unique_ptr<PhysicsSystem> g_physicsOwned;

#ifdef SPARK_HEADLESS_SUPPORT
// g_headlessMode is defined in EngineContext.cpp (SparkEngineLib)
static std::atomic<bool> g_shutdownRequested{false};

/**
 * @brief Parse command line for -headless or -dedicated flags
 */
static bool ParseHeadlessFlag(LPWSTR cmdLine)
{
    std::wstring cmd(cmdLine);
    return cmd.find(L"-headless") != std::wstring::npos || cmd.find(L"-dedicated") != std::wstring::npos;
}

/**
 * @brief Console Ctrl handler for graceful headless shutdown (Windows)
 */
static BOOL WINAPI HeadlessCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_BREAK_EVENT)
    {
        g_shutdownRequested = true;
        return TRUE;
    }
    return FALSE;
}
#endif // SPARK_HEADLESS_SUPPORT

// Win32 forward declarations
ATOM MyRegisterClass(HINSTANCE);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

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
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
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

    // GitHub Issue upload — reads token from SPARK_GITHUB_TOKEN env var
    const char* ghRepo = std::getenv("SPARK_GITHUB_REPO");
    const char* ghToken = std::getenv("SPARK_GITHUB_TOKEN");
    if (ghRepo && ghToken)
    {
        crashCfg.githubRepo = ghRepo;
        crashCfg.githubToken = ghToken;
    }

    InstallCrashHandler(crashCfg);

    // 2. Check for headless/dedicated server mode
#ifdef SPARK_HEADLESS_SUPPORT
    g_headlessMode = ParseHeadlessFlag(lpCmdLine);
    if (g_headlessMode)
    {
        // Allocate a console for stdout/stderr output
        AllocConsole();
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);

        // Install Ctrl+C handler for graceful shutdown
        SetConsoleCtrlHandler(HeadlessCtrlHandler, TRUE);

        std::cout << "=== Spark Engine (Headless/Dedicated Server) ===" << std::endl;

        // Initialize only the subsystems needed for headless operation
        g_timer = std::make_unique<Timer>();
        g_eventBus = std::make_unique<Spark::EventBus>();
        g_engineContext = std::make_unique<EngineContext>(nullptr, nullptr, g_timer.get(), g_eventBus.get());

        // Physics
        {
            extern PhysicsSystem* g_physicsSystem;
            g_physicsOwned = std::make_unique<PhysicsSystem>();
            g_physicsSystem = g_physicsOwned.get();
            g_engineContext->SetPhysics(g_physicsOwned.get());
        }

        // Module loading
        g_moduleManager = std::make_unique<ModuleManager>();

        auto& console = Spark::SimpleConsole::GetInstance();
        if (console.Initialize())
            console.LogSuccess("Headless server console initialized");

        if (LoadGameModules(*g_moduleManager, lpCmdLine))
        {
            g_moduleManager->InitializeAll(g_engineContext.get());
            console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
        }
        else
        {
            console.LogWarning("No game modules found. Running engine-only headless mode.");
        }

        // SaveSystem
        Spark::SaveSystem::GetInstance().Initialize("Saves");
        console.LogInfo("SaveSystem initialized");

        // Register console commands (no graphics commands in headless)
        RegisterEngineConsoleCommands();

        // Fixed 60 Hz server loop
        constexpr auto TICK_INTERVAL = std::chrono::microseconds(16667);
        console.LogInfo("Starting headless server loop (60 Hz)...");
        console.LogInfo("Press Ctrl+C or type 'quit' to stop.");

        while (!g_shutdownRequested)
        {
            auto tickStart = std::chrono::steady_clock::now();

            float dt = g_timer ? g_timer->GetDeltaTime() : (1.0f / 60.0f);

            if (g_moduleManager && g_moduleManager->HasModules())
                g_moduleManager->UpdateAll(dt);

            console.Update();

            auto elapsed = std::chrono::steady_clock::now() - tickStart;
            if (elapsed < TICK_INTERVAL)
                std::this_thread::sleep_for(TICK_INTERVAL - elapsed);
        }

        // Shutdown
        console.LogInfo("Headless server shutting down...");

        if (g_moduleManager)
        {
            g_moduleManager->ShutdownAll();
            g_moduleManager->UnloadAll();
            g_moduleManager.reset();
        }

        if (g_physicsOwned)
        {
            extern PhysicsSystem* g_physicsSystem;
            g_physicsSystem = nullptr;
            g_physicsOwned->Shutdown();
            g_physicsOwned.reset();
        }

        g_engineContext.reset();
        g_eventBus.reset();
        g_timer.reset();

        console.Shutdown();

        FreeConsole();
        return 0;
    }
#endif // SPARK_HEADLESS_SUPPORT

    // 2b. Class & window title (normal windowed mode)
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
    g_engineContext = std::make_unique<EngineContext>(g_graphics.get(), g_input.get(), g_timer.get(), g_eventBus.get());

    // 5b. Create PhysicsSystem (owned here, not by GraphicsEngine)
    {
        extern PhysicsSystem* g_physicsSystem; // raw global defined in PhysicsSystem.cpp
        g_physicsOwned = std::make_unique<PhysicsSystem>();
        g_physicsSystem = g_physicsOwned.get();
        g_engineContext->SetPhysics(g_physicsOwned.get());
        if (g_graphics)
            g_graphics->SetPhysicsSystem(g_physicsOwned.get());
    }

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
            if (hWnd)
                SetWindowTextW(hWnd, title.c_str());
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
    if (SUCCEEDED(g_audioEngine->Initialize(32)))
    {
        console.LogInfo("AudioEngine initialized (32 sources)");
        g_engineContext->SetAudio(g_audioEngine.get());
    }
    else
    {
        console.LogWarning("AudioEngine initialization failed - audio commands will be unavailable");
        g_audioEngine.reset();
    }

    // 8. Register engine console commands
    RegisterEngineConsoleCommands();
    EngineSettings::GetInstance().RegisterConsoleCommands();

    // 8b. Log warnings for missing third-party modules
    LogMissingModuleWarnings();

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

            if (g_input)
                g_input->Update();

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

    // Shut down physics before graphics (physics was extracted from GraphicsEngine)
    if (g_physicsOwned)
    {
        extern PhysicsSystem* g_physicsSystem;
        g_physicsSystem = nullptr;
        g_physicsOwned->Shutdown();
        g_physicsOwned.reset();
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

    HWND hWnd = CreateWindowW(g_szClass, g_szTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, winW, winH, nullptr, nullptr,
                              hInst, nullptr);

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
    if (console.Initialize())
    {
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
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        if (g_input)
            g_input->HandleMessage(msg, wParam, lParam);
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
    if (msg == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
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

    // Graphics console commands are now registered in GraphicsConsoleCommands.cpp
    if (g_graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*g_graphics);

    console.RegisterCommand(
        "module_info",
        [](const std::vector<std::string>&) -> std::string
        {
            if (!g_moduleManager || !g_moduleManager->HasModules())
                return "No modules loaded";
            std::stringstream ss;
            ss << "=== Loaded Modules (" << g_moduleManager->GetModuleCount() << ") ===\n";
            // Show primary module info
            auto* primary = g_moduleManager->GetPrimaryModule();
            if (primary)
            {
                auto info = primary->GetModuleInfo();
                ss << "Primary: " << info.name << " v" << info.version << " (loadOrder=" << info.loadOrder << ")\n";
            }
            return ss.str();
        },
        "Show loaded module info");

    console.RegisterCommand(
        "module_reload",
        [](const std::vector<std::string>& args) -> std::string
        {
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
                if (!primary)
                    return "No primary module to reload";
                name = primary->GetModuleInfo().name;
            }

            if (g_moduleManager->ReloadModule(name, g_engineContext.get()))
                return "Module '" + name + "' reloaded successfully";
            return "Failed to reload module '" + name + "'";
        },
        "Hot-reload a module DLL (usage: module_reload [name])");

    // ---- SaveSystem commands ----
    console.RegisterCommand(
        "save_list",
        [](const std::vector<std::string>&) -> std::string
        {
            try
            {
                return Spark::SaveSystem::GetInstance().Console_ListSaves();
            }
            catch (...)
            {
                return "SaveSystem not available";
            }
        },
        "List all save slots", "Save");

    console.RegisterCommand(
        "save_info",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: save_info <slot_name>";
            try
            {
                return Spark::SaveSystem::GetInstance().Console_GetSaveInfo(args[0]);
            }
            catch (...)
            {
                return "SaveSystem not available";
            }
        },
        "Show details for a save slot", "Save");

    // ---- PhysicsSystem commands ----
    console.RegisterCommand(
        "physics_metrics",
        [](const std::vector<std::string>&) -> std::string
        {
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            auto m = physics->Console_GetMetrics();
            std::stringstream ss;
            ss << "=== Physics Metrics ===\n";
            ss << "Active Bodies: " << m.activeRigidBodies << "/" << m.totalRigidBodies << "\n";
            ss << "Constraints: " << m.activeConstraints << "\n";
            ss << "Collision Pairs: " << m.collisionPairs << "\n";
            ss << "Sim Time: " << m.simulationTime << "ms\n";
            ss << "Substeps: " << m.substeps << "\n";
            return ss.str();
        },
        "Display physics performance metrics", "Physics");

    console.RegisterCommand(
        "physics_list",
        [](const std::vector<std::string>&) -> std::string
        {
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            return physics->Console_ListBodies();
        },
        "List all physics bodies", "Physics");

    console.RegisterCommand(
        "physics_body_info",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: physics_body_info <name>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            return physics->Console_GetBodyInfo(args[0]);
        },
        "Get detailed info about a physics body", "Physics");

    console.RegisterCommand(
        "physics_gravity",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 3)
                return "Usage: physics_gravity <x> <y> <z>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            float x = std::stof(args[0]), y = std::stof(args[1]), z = std::stof(args[2]);
            physics->Console_SetGravity(x, y, z);
            return "Gravity set to (" + args[0] + ", " + args[1] + ", " + args[2] + ")";
        },
        "Set world gravity vector", "Physics");

    console.RegisterCommand(
        "physics_create",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 5)
                return "Usage: physics_create <name> <type> <x> <y> <z>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            float x = std::stof(args[2]), y = std::stof(args[3]), z = std::stof(args[4]);
            bool ok = physics->Console_CreateBody(args[0], args[1], x, y, z);
            return ok ? "Body '" + args[0] + "' created" : "Failed to create body (invalid type?)";
        },
        "Create a physics body (type: static/kinematic/dynamic)", "Physics");

    console.RegisterCommand(
        "physics_remove",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: physics_remove <name>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            bool ok = physics->Console_RemoveBody(args[0]);
            return ok ? "Body '" + args[0] + "' removed" : "Body not found";
        },
        "Remove a physics body", "Physics");

    console.RegisterCommand(
        "physics_set",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 3)
                return "Usage: physics_set <name> <property> <value>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            physics->Console_SetBodyProperty(args[0], args[1], std::stof(args[2]));
            return args[1] + " set to " + args[2] + " on '" + args[0] + "'";
        },
        "Set body property (mass/friction/restitution/linearDamping/angularDamping)", "Physics");

    console.RegisterCommand(
        "physics_force",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 4)
                return "Usage: physics_force <name> <x> <y> <z>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            physics->Console_ApplyForce(args[0], std::stof(args[1]), std::stof(args[2]), std::stof(args[3]));
            return "Force applied to '" + args[0] + "'";
        },
        "Apply force to a physics body", "Physics");

    console.RegisterCommand(
        "physics_impulse",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 4)
                return "Usage: physics_impulse <name> <x> <y> <z>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            physics->Console_ApplyImpulse(args[0], std::stof(args[1]), std::stof(args[2]), std::stof(args[3]));
            return "Impulse applied to '" + args[0] + "'";
        },
        "Apply impulse to a physics body", "Physics");

    console.RegisterCommand(
        "physics_debug",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: physics_debug <on|off>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
            physics->Console_EnableDebugDraw(enable);
            return enable ? "Physics debug draw enabled" : "Physics debug draw disabled";
        },
        "Toggle physics debug overlay", "Physics");

    console.RegisterCommand(
        "physics_pause",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: physics_pause <on|off>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            bool pause = (args[0] == "on" || args[0] == "true" || args[0] == "1");
            physics->Console_PausePhysics(pause);
            return pause ? "Physics simulation paused" : "Physics simulation resumed";
        },
        "Pause/resume physics simulation", "Physics");

    console.RegisterCommand(
        "physics_timestep",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: physics_timestep <seconds>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            float ts = std::stof(args[0]);
            physics->Console_SetTimeStep(ts);
            return "Physics timestep set to " + args[0] + "s";
        },
        "Set physics fixed timestep", "Physics");

    console.RegisterCommand(
        "physics_raycast",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 7)
                return "Usage: physics_raycast <ox> <oy> <oz> <dx> <dy> <dz> <maxDist>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            return physics->Console_Raycast(std::stof(args[0]), std::stof(args[1]), std::stof(args[2]),
                                            std::stof(args[3]), std::stof(args[4]), std::stof(args[5]),
                                            std::stof(args[6]));
        },
        "Perform a physics raycast", "Physics");

    console.RegisterCommand(
        "physics_reset",
        [](const std::vector<std::string>&) -> std::string
        {
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            physics->Console_Reset();
            return "Physics world reset";
        },
        "Reset physics world to initial state", "Physics");

    // ---- AudioEngine commands ----
    console.RegisterCommand(
        "audio_master_volume",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: audio_master_volume <0.0-1.0>";
            if (!g_audioEngine)
                return "Audio engine not available";
            g_audioEngine->Console_SetMasterVolume(std::stof(args[0]));
            return "Master volume set to " + args[0];
        },
        "Set master audio volume", "Audio");

    console.RegisterCommand(
        "audio_sfx_volume",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: audio_sfx_volume <0.0-1.0>";
            if (!g_audioEngine)
                return "Audio engine not available";
            g_audioEngine->Console_SetSFXVolume(std::stof(args[0]));
            return "SFX volume set to " + args[0];
        },
        "Set SFX volume", "Audio");

    console.RegisterCommand(
        "audio_music_volume",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: audio_music_volume <0.0-1.0>";
            if (!g_audioEngine)
                return "Audio engine not available";
            g_audioEngine->Console_SetMusicVolume(std::stof(args[0]));
            return "Music volume set to " + args[0];
        },
        "Set music volume", "Audio");

    console.RegisterCommand(
        "audio_3d",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: audio_3d <on|off>";
            if (!g_audioEngine)
                return "Audio engine not available";
            bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
            g_audioEngine->Console_Set3DAudio(enable);
            return enable ? "3D audio enabled" : "3D audio disabled";
        },
        "Toggle 3D spatial audio", "Audio");

    console.RegisterCommand(
        "audio_play_test",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: audio_play_test <sound_name> [3d]";
            if (!g_audioEngine)
                return "Audio engine not available";
            bool is3D = (args.size() > 1 && (args[1] == "3d" || args[1] == "true"));
            uint32_t id = g_audioEngine->Console_PlayTestSound(args[0], is3D);
            return id > 0 ? "Playing sound (ID: " + std::to_string(id) + ")" : "Failed to play sound";
        },
        "Play a test sound", "Audio");

    console.RegisterCommand(
        "audio_stop",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: audio_stop <source_id>";
            if (!g_audioEngine)
                return "Audio engine not available";
            g_audioEngine->Console_StopSound(static_cast<uint32_t>(std::stoul(args[0])));
            return "Sound stopped";
        },
        "Stop a playing sound by ID", "Audio");

    console.RegisterCommand(
        "audio_stop_all",
        [](const std::vector<std::string>&) -> std::string
        {
            if (!g_audioEngine)
                return "Audio engine not available";
            g_audioEngine->Console_StopAllSounds();
            return "All sounds stopped";
        },
        "Stop all playing sounds", "Audio");

    console.RegisterCommand(
        "audio_list",
        [](const std::vector<std::string>&) -> std::string
        {
            if (!g_audioEngine)
                return "Audio engine not available";
            return g_audioEngine->Console_ListSounds();
        },
        "List all loaded sounds", "Audio");

    console.RegisterCommand(
        "audio_metrics",
        [](const std::vector<std::string>&) -> std::string
        {
            if (!g_audioEngine)
                return "Audio engine not available";
            auto m = g_audioEngine->Console_GetMetrics();
            std::stringstream ss;
            ss << "=== Audio Metrics ===\n";
            ss << "Active Sources: " << m.activeSources << "/" << m.totalSources << "\n";
            ss << "Loaded Sounds: " << m.loadedSounds << "\n";
            ss << "Memory: " << (m.memoryUsage / 1024) << " KB\n";
            return ss.str();
        },
        "Display audio performance metrics", "Audio");

    console.RegisterCommand(
        "audio_reset",
        [](const std::vector<std::string>&) -> std::string
        {
            if (!g_audioEngine)
                return "Audio engine not available";
            g_audioEngine->Console_ResetToDefaults();
            return "Audio settings reset to defaults";
        },
        "Reset audio settings to defaults", "Audio");

    console.RegisterCommand(
        "audio_listener_pos",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 3)
                return "Usage: audio_listener_pos <x> <y> <z>";
            if (!g_audioEngine)
                return "Audio engine not available";
            g_audioEngine->Console_SetListenerPosition(std::stof(args[0]), std::stof(args[1]), std::stof(args[2]));
            return "Listener position set";
        },
        "Set 3D audio listener position", "Audio");

    console.RegisterCommand(
        "audio_doppler",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: audio_doppler <scale>";
            if (!g_audioEngine)
                return "Audio engine not available";
            g_audioEngine->Console_SetDopplerScale(std::stof(args[0]));
            return "Doppler scale set to " + args[0];
        },
        "Set Doppler effect scale", "Audio");

    console.RegisterCommand(
        "audio_source_info",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: audio_source_info <source_id>";
            if (!g_audioEngine)
                return "Audio engine not available";
            return g_audioEngine->Console_GetSourceInfo(static_cast<uint32_t>(std::stoul(args[0])));
        },
        "Get info about an audio source", "Audio");
}

#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Non-Windows: main entry point with SDL2 window + event loop
// ============================================================================
#ifndef SPARK_PLATFORM_WINDOWS
#include "SparkEngine.h"
#include "ModuleManager.h"
#include "EngineContext.h"
#include "EngineSettings.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "Audio/AudioEngine.h"
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
#include "Physics/PhysicsSystem.h"
#endif
#include "Utils/Timer.h"
#include "Utils/SparkConsole.h"
#include "Graphics/GraphicsConsoleCommands.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>

#ifdef SPARK_SDL2_AVAILABLE
#include <SDL.h>
#endif

std::unique_ptr<GraphicsEngine> g_graphics;
std::unique_ptr<InputManager> g_input;
std::unique_ptr<Timer> g_timer;
std::unique_ptr<Spark::EventBus> g_eventBus;
std::unique_ptr<ModuleManager> g_moduleManager;
extern std::unique_ptr<EngineContext> g_engineContext; // defined in EngineContext.cpp
std::unique_ptr<AudioEngine> g_audioEngine;
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
std::unique_ptr<PhysicsSystem> g_physicsOwned;
#endif

// -----------------------------------------------------------------------------
// Missing module startup warnings (Linux)
// -----------------------------------------------------------------------------
static void LogMissingModuleWarnings()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    int missingCount = 0;

#ifndef SPARK_BULLET_PHYSICS_AVAILABLE
    console.LogWarning(
        "[MISSING MODULE] Bullet Physics — rigid body simulation, collision detection, and raycasting are DISABLED.");
    console.LogWarning(
        "                 Physics-dependent features (gravity, projectiles, triggers) will not function.");
    ++missingCount;
#endif

#ifndef SPARK_MINIZ_AVAILABLE
    console.LogWarning("[MISSING MODULE] miniz — crash dump compression and save file compression are DISABLED.");
    console.LogWarning("                 CrashHandler is using a stub. Save files will not be compressed.");
    ++missingCount;
#endif

#ifndef SPARK_SDL2_AVAILABLE
    console.LogWarning("[MISSING MODULE] SDL2 — cross-platform windowing and input are DISABLED.");
    console.LogWarning("                 Install libsdl2-dev and rebuild with -DENABLE_SDL2=ON for windowed mode.");
    ++missingCount;
#endif

    if (missingCount > 0)
    {
        console.LogWarning("------------------------------------------------------------");
        console.LogWarning(std::to_string(missingCount) + " module(s) missing. Expect degraded functionality.");
        console.LogWarning("Run: git submodule update --init --recursive");
        console.LogWarning("Then rebuild to restore full engine features.");
        console.LogWarning("See README.md 'Dependencies' section for details.");
        console.LogWarning("------------------------------------------------------------");
    }
}

static std::atomic<bool> g_shutdownRequested{false};

static void SignalHandler(int)
{
    g_shutdownRequested.store(true, std::memory_order_relaxed);
}

static bool ParseFlag(int argc, char* argv[], const char* flag)
{
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], flag) == 0)
            return true;
    }
    return false;
}

static std::filesystem::path GetExecutableDirectoryLinux()
{
    // /proc/self/exe is the canonical way on Linux
    std::error_code ec;
    auto exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return exePath.parent_path();
    return std::filesystem::current_path();
}

static std::string FindGameModuleFromArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; ++i)
    {
        if (strcmp(argv[i], "-game") == 0)
        {
            std::string path = argv[i + 1];
            if (std::filesystem::exists(path))
                return path;
        }
    }
    return "";
}

static bool LoadGameModulesLinux(ModuleManager& manager, int argc, char* argv[])
{
    auto exeDir = GetExecutableDirectoryLinux();

    // 1. Check command line for specific module
    std::string cmdLineModule = FindGameModuleFromArgs(argc, argv);
    if (!cmdLineModule.empty())
        return manager.LoadModule(cmdLineModule);

    // 2. Check for module manifest
    auto manifestPath = exeDir / "spark.modules.json";
    if (std::filesystem::exists(manifestPath))
        return manager.LoadModulesFromManifest(manifestPath.string());

    // 3. Fall back to directory scan
    return manager.LoadModulesFromDirectory(exeDir.string());
}

// Console command registration (engine-side, mirrors the Windows version)
static void RegisterEngineConsoleCommandsLinux()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    if (g_graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*g_graphics);

    console.RegisterCommand(
        "module_info",
        [](const std::vector<std::string>&) -> std::string
        {
            if (!g_moduleManager || !g_moduleManager->HasModules())
                return "No modules loaded";
            std::stringstream ss;
            ss << "=== Loaded Modules (" << g_moduleManager->GetModuleCount() << ") ===\n";
            auto* primary = g_moduleManager->GetPrimaryModule();
            if (primary)
            {
                auto info = primary->GetModuleInfo();
                ss << "Primary: " << info.name << " v" << info.version << " (loadOrder=" << info.loadOrder << ")\n";
            }
            return ss.str();
        },
        "Show loaded module info");

    console.RegisterCommand(
        "module_reload",
        [](const std::vector<std::string>& args) -> std::string
        {
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
                auto* primary = g_moduleManager->GetPrimaryModule();
                if (!primary)
                    return "No primary module to reload";
                name = primary->GetModuleInfo().name;
            }
            if (g_moduleManager->ReloadModule(name, g_engineContext.get()))
                return "Module '" + name + "' reloaded successfully";
            return "Failed to reload module '" + name + "'";
        },
        "Hot-reload a module SO (usage: module_reload [name])");

    // SaveSystem
    console.RegisterCommand(
        "save_list",
        [](const std::vector<std::string>&) -> std::string
        {
            try
            {
                return Spark::SaveSystem::GetInstance().Console_ListSaves();
            }
            catch (...)
            {
                return "SaveSystem not available";
            }
        },
        "List all save slots", "Save");

#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    // PhysicsSystem commands
    console.RegisterCommand(
        "physics_metrics",
        [](const std::vector<std::string>&) -> std::string
        {
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            auto m = physics->Console_GetMetrics();
            std::stringstream ss;
            ss << "=== Physics Metrics ===\n";
            ss << "Active Bodies: " << m.activeRigidBodies << "/" << m.totalRigidBodies << "\n";
            ss << "Constraints: " << m.activeConstraints << "\n";
            ss << "Collision Pairs: " << m.collisionPairs << "\n";
            ss << "Sim Time: " << m.simulationTime << "ms\n";
            ss << "Substeps: " << m.substeps << "\n";
            return ss.str();
        },
        "Display physics performance metrics", "Physics");

    console.RegisterCommand(
        "physics_gravity",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 3)
                return "Usage: physics_gravity <x> <y> <z>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            float x = std::stof(args[0]), y = std::stof(args[1]), z = std::stof(args[2]);
            physics->Console_SetGravity(x, y, z);
            return "Gravity set to (" + args[0] + ", " + args[1] + ", " + args[2] + ")";
        },
        "Set world gravity vector", "Physics");

    console.RegisterCommand(
        "physics_debug",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: physics_debug <on|off>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            bool enable = (args[0] == "on" || args[0] == "true" || args[0] == "1");
            physics->Console_EnableDebugDraw(enable);
            return enable ? "Physics debug draw enabled" : "Physics debug draw disabled";
        },
        "Toggle physics debug overlay", "Physics");

    console.RegisterCommand(
        "physics_pause",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: physics_pause <on|off>";
            if (!g_engineContext)
                return "Engine context not available";
            auto* physics = g_engineContext->GetPhysics();
            if (!physics)
                return "Physics system not available";
            bool pause = (args[0] == "on" || args[0] == "true" || args[0] == "1");
            physics->Console_PausePhysics(pause);
            return pause ? "Physics simulation paused" : "Physics simulation resumed";
        },
        "Pause/resume physics simulation", "Physics");
#endif // SPARK_BULLET_PHYSICS_AVAILABLE

    // AudioEngine commands
    console.RegisterCommand(
        "audio_master_volume",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: audio_master_volume <0.0-1.0>";
            if (!g_audioEngine)
                return "Audio engine not available";
            g_audioEngine->Console_SetMasterVolume(std::stof(args[0]));
            return "Master volume set to " + args[0];
        },
        "Set master audio volume", "Audio");
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

#ifdef SPARK_HEADLESS_SUPPORT
    bool headless = ParseFlag(argc, argv, "-headless") || ParseFlag(argc, argv, "-dedicated");
    g_headlessMode = headless;
#else
    bool headless = false;
#endif

    if (headless)
    {
#ifdef SPARK_HEADLESS_SUPPORT
        std::cout << "=== Spark Engine (Headless/Dedicated Server - Linux) ===" << std::endl;

        g_eventBus = std::make_unique<Spark::EventBus>();
        g_timer = std::make_unique<Timer>();
        g_engineContext = std::make_unique<EngineContext>(nullptr, nullptr, g_timer.get(), g_eventBus.get());

#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
        {
            extern PhysicsSystem* g_physicsSystem;
            g_physicsOwned = std::make_unique<PhysicsSystem>();
            g_physicsSystem = g_physicsOwned.get();
            g_engineContext->SetPhysics(g_physicsOwned.get());
        }
#endif

        g_moduleManager = std::make_unique<ModuleManager>();

        auto& console = Spark::SimpleConsole::GetInstance();
        if (console.Initialize())
            console.LogSuccess("Headless server console initialized");

        if (LoadGameModulesLinux(*g_moduleManager, argc, argv))
        {
            g_moduleManager->InitializeAll(g_engineContext.get());
            console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
        }
        else
        {
            console.LogWarning("No game modules found. Running engine-only headless mode.");
        }

        Spark::SaveSystem::GetInstance().Initialize("Saves");
        RegisterEngineConsoleCommandsLinux();
        LogMissingModuleWarnings();

        constexpr auto TICK_INTERVAL = std::chrono::microseconds(16667);
        console.LogInfo("Starting headless server loop (60 Hz)...");
        console.LogInfo("Press Ctrl+C to stop.");

        while (!g_shutdownRequested)
        {
            auto tickStart = std::chrono::steady_clock::now();
            float dt = g_timer ? g_timer->GetDeltaTime() : (1.0f / 60.0f);

            if (g_moduleManager && g_moduleManager->HasModules())
                g_moduleManager->UpdateAll(dt);

            console.Update();

            auto elapsed = std::chrono::steady_clock::now() - tickStart;
            if (elapsed < TICK_INTERVAL)
                std::this_thread::sleep_for(TICK_INTERVAL - elapsed);
        }

        console.LogInfo("Headless server shutting down...");

        if (g_moduleManager)
        {
            g_moduleManager->ShutdownAll();
            g_moduleManager->UnloadAll();
            g_moduleManager.reset();
        }

#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
        if (g_physicsOwned)
        {
            extern PhysicsSystem* g_physicsSystem;
            g_physicsSystem = nullptr;
            g_physicsOwned->Shutdown();
            g_physicsOwned.reset();
        }
#endif

        g_engineContext.reset();
        g_eventBus.reset();
        g_timer.reset();
        console.Shutdown();

        std::cout << "Headless server shut down cleanly." << std::endl;
        return 0;
#endif // SPARK_HEADLESS_SUPPORT
    }

    // =================================================================
    // Normal (windowed) mode - SDL2 window + event loop
    // =================================================================
    std::cout << "=== Spark Engine (Linux Build) ===" << std::endl;

#ifdef SPARK_SDL2_AVAILABLE
    // 1. Initialize SDL2
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)
    {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return -1;
    }

    // 2. Load engine settings
    auto& settings = EngineSettings::GetInstance();
    settings.Load();

    int winW = settings.Graphics().windowWidth;
    int winH = settings.Graphics().windowHeight;

    Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (settings.Graphics().fullscreen)
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    // Request an OpenGL context if the RHI may use it
    windowFlags |= SDL_WINDOW_OPENGL;

    SDL_Window* window =
        SDL_CreateWindow("Spark Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, windowFlags);
    if (!window)
    {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }

    // 3. Create engine subsystems
    g_timer = std::make_unique<Timer>();
    g_eventBus = std::make_unique<Spark::EventBus>();
    g_input = std::make_unique<InputManager>();
    g_input->Initialize(static_cast<HWND>(window));
    g_graphics = std::make_unique<GraphicsEngine>();

    HRESULT hr = g_graphics->Initialize(static_cast<Spark::NativeWindowHandle>(window));
    if (SUCCEEDED(hr))
        std::cout << "Graphics engine initialized (RHI backend)." << std::endl;
    else
        std::cout << "Graphics engine initialization deferred (headless fallback)." << std::endl;

    // 4. Engine context (service locator)
    g_engineContext = std::make_unique<EngineContext>(g_graphics.get(), g_input.get(), g_timer.get(), g_eventBus.get());

    // 5. Physics
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    {
        extern PhysicsSystem* g_physicsSystem;
        g_physicsOwned = std::make_unique<PhysicsSystem>();
        g_physicsSystem = g_physicsOwned.get();
        g_engineContext->SetPhysics(g_physicsOwned.get());
        if (g_graphics)
            g_graphics->SetPhysicsSystem(g_physicsOwned.get());
    }
#endif

    // 6. Module loading
    g_moduleManager = std::make_unique<ModuleManager>();

    auto& console = Spark::SimpleConsole::GetInstance();
    if (console.Initialize())
    {
        console.LogSuccess("Spark Engine runtime initialized (Linux/SDL2)");
        console.LogInfo("Settings loaded from " + settings.GetFilePath());
    }

    if (LoadGameModulesLinux(*g_moduleManager, argc, argv))
    {
        g_moduleManager->InitializeAll(g_engineContext.get());

        auto* primary = g_moduleManager->GetPrimaryModule();
        if (primary)
        {
            auto info = primary->GetModuleInfo();
            std::string title = std::string("Spark Engine - ") + info.name;
            SDL_SetWindowTitle(window, title.c_str());
        }

        console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only mode.");
        console.LogInfo("Place a game .so (e.g. libSparkGame.so) next to the engine executable,");
        console.LogInfo("use -game <path> on the command line,");
        console.LogInfo("or create a spark.modules.json manifest.");
    }

    // 7. Additional subsystems
    Spark::SaveSystem::GetInstance().Initialize("Saves");
    console.LogInfo("SaveSystem initialized");

    g_audioEngine = std::make_unique<AudioEngine>();
    if (SUCCEEDED(g_audioEngine->Initialize(32)))
    {
        console.LogInfo("AudioEngine initialized (32 sources)");
        g_engineContext->SetAudio(g_audioEngine.get());
    }
    else
    {
        console.LogWarning("AudioEngine initialization failed");
        g_audioEngine.reset();
    }

    // 8. Console commands
    RegisterEngineConsoleCommandsLinux();
    settings.RegisterConsoleCommands();

    // 8b. Log warnings for missing third-party modules
    LogMissingModuleWarnings();

    // 9. Main event loop (SDL2)
    console.LogInfo("Starting main engine loop (SDL2)...");
    bool running = true;

    while (running && !g_shutdownRequested)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE)
                    running = false;
                else if (event.window.event == SDL_WINDOWEVENT_RESIZED)
                {
                    int w = event.window.data1;
                    int h = event.window.data2;
                    if (g_graphics)
                        g_graphics->OnResize(w, h);
                    if (g_moduleManager)
                        g_moduleManager->ResizeAll(w, h);
                }
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (g_input)
                {
                    // Translate SDL key events to WM_KEYDOWN/WM_KEYUP style messages
                    // so InputManager::HandleMessage works consistently
                    UINT msg = (event.type == SDL_KEYDOWN) ? WM_KEYDOWN : WM_KEYUP;
                    int vk = 0;
                    auto sym = event.key.keysym.sym;
                    if (sym >= SDLK_a && sym <= SDLK_z)
                        vk = 'A' + (sym - SDLK_a);
                    else if (sym >= SDLK_0 && sym <= SDLK_9)
                        vk = '0' + (sym - SDLK_0);
                    else if (sym == SDLK_SPACE)
                        vk = VK_SPACE;
                    else if (sym == SDLK_ESCAPE)
                        vk = VK_ESCAPE;
                    else if (sym == SDLK_RETURN)
                        vk = VK_RETURN;
                    else if (sym == SDLK_TAB)
                        vk = VK_TAB;
                    else if (sym == SDLK_BACKSPACE)
                        vk = VK_BACK;
                    else if (sym == SDLK_UP)
                        vk = VK_UP;
                    else if (sym == SDLK_DOWN)
                        vk = VK_DOWN;
                    else if (sym == SDLK_LEFT)
                        vk = VK_LEFT;
                    else if (sym == SDLK_RIGHT)
                        vk = VK_RIGHT;
                    else if (sym == SDLK_LSHIFT)
                        vk = VK_LSHIFT;
                    else if (sym == SDLK_RSHIFT)
                        vk = VK_RSHIFT;
                    else if (sym == SDLK_LCTRL)
                        vk = VK_LCONTROL;
                    else if (sym == SDLK_RCTRL)
                        vk = VK_RCONTROL;
                    else if (sym == SDLK_LALT)
                        vk = VK_LMENU;
                    else if (sym == SDLK_RALT)
                        vk = VK_RMENU;
                    else if (sym == SDLK_DELETE)
                        vk = VK_DELETE;
                    else if (sym >= SDLK_F1 && sym <= SDLK_F12)
                        vk = VK_F1 + (sym - SDLK_F1);

                    if (vk != 0)
                        g_input->HandleMessage(msg, static_cast<WPARAM>(vk), 0);
                }
                break;

            case SDL_MOUSEMOTION:
                if (g_input)
                    g_input->HandleMessage(WM_MOUSEMOVE, 0,
                                           static_cast<LPARAM>((event.motion.y << 16) | (event.motion.x & 0xFFFF)));
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (g_input)
                {
                    UINT msg = 0;
                    if (event.button.button == SDL_BUTTON_LEFT)
                        msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                    else if (event.button.button == SDL_BUTTON_RIGHT)
                        msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                    else if (event.button.button == SDL_BUTTON_MIDDLE)
                        msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                    if (msg)
                        g_input->HandleMessage(msg, 0, 0);
                }
                break;
            }
        }

        if (!running)
            break;

        // Tick
        float dt = g_timer ? g_timer->GetDeltaTime() : 0.016f;

        if (g_input)
            g_input->Update();

        if (g_moduleManager && g_moduleManager->HasModules())
        {
            g_moduleManager->UpdateAll(dt);
            g_moduleManager->RenderAll();
        }
        else if (g_graphics)
        {
            g_graphics->BeginFrame();
            g_graphics->EndFrame();
        }

        console.Update();
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

#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    if (g_physicsOwned)
    {
        extern PhysicsSystem* g_physicsSystem;
        g_physicsSystem = nullptr;
        g_physicsOwned->Shutdown();
        g_physicsOwned.reset();
    }
#endif

    g_engineContext.reset();
    g_eventBus.reset();
    g_input.reset();
    g_graphics.reset();
    g_timer.reset();

    console.Shutdown();

    SDL_DestroyWindow(window);
    SDL_Quit();

#else // !SPARK_SDL2_AVAILABLE
    // Fallback: no SDL2 available, run without a window (headless-like)
    std::cerr << "Warning: SDL2 not available. Running without a window." << std::endl;
    std::cerr << "Install SDL2 and rebuild with -DENABLE_SDL2=ON for windowed mode." << std::endl;

    g_eventBus = std::make_unique<Spark::EventBus>();
    g_timer = std::make_unique<Timer>();
    g_input = std::make_unique<InputManager>();
    g_graphics = std::make_unique<GraphicsEngine>();
    g_graphics->Initialize(nullptr);

    g_engineContext = std::make_unique<EngineContext>(g_graphics.get(), g_input.get(), g_timer.get(), g_eventBus.get());

#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    {
        extern PhysicsSystem* g_physicsSystem;
        g_physicsOwned = std::make_unique<PhysicsSystem>();
        g_physicsSystem = g_physicsOwned.get();
        g_engineContext->SetPhysics(g_physicsOwned.get());
    }
#endif

    g_moduleManager = std::make_unique<ModuleManager>();

    auto& console = Spark::SimpleConsole::GetInstance();
    console.Initialize();
    console.LogWarning("No SDL2 - engine will exit after initialization.");
    LogMissingModuleWarnings();

    if (LoadGameModulesLinux(*g_moduleManager, argc, argv))
    {
        g_moduleManager->InitializeAll(g_engineContext.get());
        console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
    }

    // Minimal loop - process a few ticks then exit
    for (int frame = 0; frame < 10 && !g_shutdownRequested; ++frame)
    {
        float dt = g_timer ? g_timer->GetDeltaTime() : 0.016f;
        if (g_moduleManager && g_moduleManager->HasModules())
            g_moduleManager->UpdateAll(dt);
        console.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (g_moduleManager)
    {
        g_moduleManager->ShutdownAll();
        g_moduleManager->UnloadAll();
        g_moduleManager.reset();
    }

#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    if (g_physicsOwned)
    {
        extern PhysicsSystem* g_physicsSystem;
        g_physicsSystem = nullptr;
        g_physicsOwned->Shutdown();
        g_physicsOwned.reset();
    }
#endif

    g_engineContext.reset();
    g_graphics.reset();
    g_input.reset();
    g_timer.reset();
    g_eventBus.reset();
    console.Shutdown();
#endif // SPARK_SDL2_AVAILABLE

    std::cout << "Spark Engine shut down cleanly." << std::endl;
    return 0;
}
#endif // !SPARK_PLATFORM_WINDOWS
