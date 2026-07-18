/**
 * @file SparkEngineLinuxSDL2.cpp
 * @brief SDL2 windowed-mode bring-up: window/backend selection and RunSDL2Windowed (Linux + macOS).
 *
 * Split from SparkEngineLinux.cpp to keep files under the ~500-line guideline.
 * Contains graphics-backend selection (Vulkan/OpenGL/Metal), SDL window and GL
 * context creation with the Vulkan→OpenGL fallback, and subsystem initialization
 * for the windowed path. The event pump and main loop live in
 * SparkEngineLinuxSDL2Events.cpp; shared helpers in SparkEngineLinuxInit.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "SparkEngineLinuxInternal.h"
#include "SparkEngineMacOS.h"
#include "EngineRuntime.h"
#include "ModuleManager.h"
#include "EngineContext.h"
#include "EngineSettings.h"
#include "Engine/Events/EventSystem.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/RHI/RHIBridge.h"
#include "Graphics/RHI/RHIFactory.h"
#include "Graphics/RHI/RHIDevice.h"
#include "Input/InputManager.h"
#include "Utils/Timer.h"
#include "Utils/SparkConsole.h"
#include "Utils/Logger.h"
#include "Utils/LogMacros.h" // SPARK_LOG_*
#include "Utils/FreezeDetector.h"
#include "Utils/HitchDetector.h"
#include "Utils/BenchmarkFramework.h"
#include "Utils/AssetStallDetector.h"
#include "Core/AssetValidator.h"
#include "Utils/NetworkHealthMonitor.h"
#include "Utils/GPUResourceLeakDetector.h"
#include "Utils/InvalidStateDetector.h"
#include "Utils/Assert.h"
#ifdef SPARK_SDL2_AVAILABLE
#include <SDL.h>
#include <SDL_vulkan.h>
#endif
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>

#ifndef SPARK_PLATFORM_WINDOWS
#ifdef SPARK_SDL2_AVAILABLE

/**
 * @brief Construct and initialize GraphicsEngine against the given window /
 *        native render handle. Logs the outcome via SimpleConsole.
 *
 * Extracted so the caller (RunSDL2Windowed) can detect a backend fallback
 * (e.g. Vulkan requested → OpenGL selected by RHIBridge) and recreate the
 * window + graphics pair with the correct SDL window flag.
 */
static HRESULT InitializeGraphicsForWindow(SDL_Window* window, void* nativeRenderHandle)
{
    GetEngineRuntime().graphics = std::make_unique<GraphicsEngine>();

    // On macOS+Metal the RHI needs an NSView/CAMetalLayer, not the SDL_Window.
    // nativeRenderHandle overrides the window when set; otherwise the window
    // itself is passed through (Vulkan/OpenGL/Linux paths).
    void* rhiHandle = nativeRenderHandle ? nativeRenderHandle : static_cast<void*>(window);
    HRESULT hr = GetEngineRuntime().graphics->Initialize(static_cast<Spark::NativeWindowHandle>(rhiHandle));
    auto& console = Spark::SimpleConsole::GetInstance();
    if (SUCCEEDED(hr))
        console.LogInfo("Graphics engine initialized (RHI backend).");
    else
        console.LogWarning("Graphics engine initialization deferred (headless fallback).");
    return hr;
}

/**
 * @brief Initialize SDL2 windowed-mode subsystems: input, engine context,
 *        modules, audio, and console commands.
 *
 * GraphicsEngine must already be constructed and initialized by the caller.
 * The caller owns that step because a Vulkan→OpenGL fallback may require
 * recreating the window before graphics init succeeds against the right
 * surface type.
 *
 * @param window The SDL2 window (already created by the caller).
 * @param argc Argument count from main().
 * @param argv Argument values from main().
 */
static void InitializeSDL2Subsystems(SDL_Window* window, int argc, char* argv[])
{
    auto& settings = EngineSettings::GetInstance();

    // Core engine objects (graphics is already initialized by the caller)
    GetEngineRuntime().timer = std::make_unique<Timer>();
    GetEngineRuntime().eventBus = std::make_unique<Spark::EventBus>();
    GetEngineRuntime().input = std::make_unique<InputManager>();
    GetEngineRuntime().input->Initialize(static_cast<HWND>(window));

    // Engine context, physics, core subsystems, gameplay subsystems
    InitLinuxCoreSubsystems(/*registerGameplay=*/true);

    // Modules, audio, console commands
    InitLinuxModulesAndCommands(argc, argv, /*initAudio=*/true);

    // Update window title with primary module name (only if we have a window —
    // the windowless NullRHIDevice fallback path sets window = nullptr).
    if (window && GetEngineRuntime().moduleManager)
    {
        auto* primary = GetEngineRuntime().moduleManager->GetPrimaryModule();
        if (primary)
        {
            auto info = primary->GetModuleInfo();
            std::string title = std::string("Spark Engine - ") + info.name;
            SDL_SetWindowTitle(window, title.c_str());
        }
    }

    settings.RegisterConsoleCommands();
    Spark::FreezeDetector::GetInstance().RegisterConsoleCommands();
    Spark::FreezeDetector::GetInstance().Start();
    Spark::HitchDetector::GetInstance().RegisterConsoleCommands();
    Spark::BenchmarkFramework::GetInstance().RegisterConsoleCommands();
    Spark::AssetStallDetector::GetInstance().RegisterConsoleCommands();
    Spark::AssetValidator::GetInstance().RegisterConsoleCommands();
    Spark::NetworkHealthMonitor::GetInstance().RegisterConsoleCommands();
    Spark::GPUResourceLeakDetector::GetInstance().RegisterConsoleCommands();
    Spark::InvalidStateDetector::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();

    // Initialize console, debug, and gameplay systems in one call
    // (also publishes EngineStartEvent when complete)
    InitConsole();
}

/**
 * @brief Run the engine in SDL2 windowed mode (Linux).
 *
 * Creates an SDL2 window, initializes all engine subsystems, runs the
 * main loop, and cleans up SDL resources on exit.
 */
int RunSDL2Windowed(int argc, char* argv[])
{
    Spark::SimpleConsole::GetInstance().LogInfo("=== Spark Engine (Linux Build) ===");

    // Force SDL2 to use EGL instead of GLX on X11 so that OpenGLDevice's
    // existing "detect host-owned EGL context and reuse it" path (see
    // OpenGLDevice.cpp:884) kicks in. With the default GLX backend the
    // OpenGLDevice falls back to its own EGL pbuffer bootstrap, which
    // conflicts with SDL2's current GLX context and produces an
    // eglMakeCurrent EGL_BAD_ACCESS (0x3002) error before the engine
    // falls back to NullRHIDevice. Forcing EGL makes the host context
    // directly reusable and lets software rasterizers (Mesa llvmpipe)
    // drive the engine under Xvfb / Wayland without a GPU.
    SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");

    const bool sdlInitOk = (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) == 0);
    if (!sdlInitOk)
    {
        // SDL_Init can fail on sandboxed hosts with no display server, no
        // joystick subsystem, or when dbus/udev aren't reachable. Fall
        // through to the windowless / NullRHIDevice path instead of
        // aborting — the engine is still useful for running game logic,
        // physics, scripting, and networking headlessly.
        Spark::SimpleConsole::GetInstance().LogWarning(std::string("SDL_Init failed: ") + SDL_GetError() +
                                                       " — falling back to windowless / NullRHIDevice mode");
    }

    auto& settings = EngineSettings::GetInstance();
    settings.Load();

    int winW = g_windowWidthOverride > 0 ? g_windowWidthOverride : settings.Graphics().windowWidth;
    int winH = g_windowHeightOverride > 0 ? g_windowHeightOverride : settings.Graphics().windowHeight;

    // Decide which graphics backend to use *before* creating the window.
    // SDL2 requires the backend-specific flag at window creation time:
    // SDL_WINDOW_VULKAN for Vulkan, SDL_WINDOW_OPENGL for OpenGL. There
    // is no way to retrofit a Vulkan surface onto an OpenGL window (or
    // vice versa) after the fact, so this decision is one-shot.
    //
    // GetRecommendedBackend() honors SPARK_DISABLE_VULKAN / _OPENGL /
    // _D3D11 env-var escape hatches, so users who hit a broken Vulkan
    // ICD can set SPARK_DISABLE_VULKAN=1 and this branch will pick
    // OpenGL instead — matching what the RHIBridge will also choose.
    bool preferVulkan = false;
    bool preferMetal = sdlInitOk && Spark::MacOS::ShouldPreferMetal();
    if (sdlInitOk)
    {
        const char* sdlDriver = SDL_GetCurrentVideoDriver();
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SDL2 video driver: %s", sdlDriver ? sdlDriver : "<null>");

        const auto recommended = Spark::RHI::RHIBridge::GetRecommendedBackend();
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "RunSDL2Windowed: recommended backend = %s",
                       Spark::RHI::GetBackendName(recommended));

        // Drivers that don't support Vulkan at all (SDL_Vulkan_LoadLibrary
        // will always fail against them). Detect upfront so we don't even
        // attempt the Vulkan path — makes the log cleaner on headless CI
        // and sandboxed hosts where x11 isn't reachable.
        const bool driverHasVulkan =
            sdlDriver && (std::strcmp(sdlDriver, "x11") == 0 || std::strcmp(sdlDriver, "wayland") == 0 ||
                          std::strcmp(sdlDriver, "cocoa") == 0 || std::strcmp(sdlDriver, "windows") == 0 ||
                          std::strcmp(sdlDriver, "KMSDRM") == 0);

        if (!preferMetal && recommended == Spark::RHI::GraphicsBackend::Vulkan && driverHasVulkan)
        {
            // Try to load libvulkan through SDL. If it isn't installed
            // on this host, fall straight back to OpenGL and tell the
            // engine the same via the existing env-var opt-out, so
            // RHIBridge doesn't try to spin up VulkanDevice only to
            // discover libvulkan isn't there.
            if (SDL_Vulkan_LoadLibrary(nullptr) == 0)
            {
                preferVulkan = true;
                SPARK_LOG_INFO(Spark::LogCategory::Graphics, "SDL2 Vulkan loader initialized — creating Vulkan window");
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                               "SDL_Vulkan_LoadLibrary failed: %s — falling back to OpenGL", SDL_GetError());
                setenv("SPARK_DISABLE_VULKAN", "1", /*overwrite=*/1);
            }
        }
        else if (recommended == Spark::RHI::GraphicsBackend::Vulkan && !driverHasVulkan)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                           "SDL2 driver '%s' has no Vulkan support — falling back to OpenGL",
                           sdlDriver ? sdlDriver : "<null>");
            setenv("SPARK_DISABLE_VULKAN", "1", /*overwrite=*/1);
        }
    }

    if (sdlInitOk && !preferVulkan && !preferMetal)
    {
        // OpenGL path — set attributes before window creation (required
        // for Mesa llvmpipe and other software rasterizers).
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    }

    // If SDL itself failed to initialize, or RHIBridge already decided there
    // is no usable GPU backend (e.g. under gVisor or on a host missing
    // libEGL/libvulkan), skip SDL window creation entirely. SDL2's offscreen
    // video driver dlopen()s libEGL during SDL_CreateWindow — if libEGL is
    // missing the process exits with -1 without surfacing an error through
    // SDL_GetError(). Running windowless in that case lets the engine come up
    // on NullRHIDevice.
    const bool noGpuBackend =
        !sdlInitOk || ((!preferVulkan && !preferMetal) &&
                       (Spark::RHI::RHIBridge::GetRecommendedBackend() == Spark::RHI::GraphicsBackend::None));
    if (noGpuBackend)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "No GPU backend available — skipping SDL window creation, running on NullRHIDevice");
    }

    Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (settings.Graphics().fullscreen)
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (preferMetal)
        windowFlags |= Spark::MacOS::GetMetalWindowFlag();
    else if (preferVulkan)
        windowFlags |= SDL_WINDOW_VULKAN;
    else
        windowFlags |= SDL_WINDOW_OPENGL;

    SDL_Window* window = nullptr;
    if (!noGpuBackend)
    {
        window =
            SDL_CreateWindow("Spark Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, windowFlags);
        if (!window)
        {
            // Treat window-creation failure as recoverable: the engine will
            // initialize graphics against a null handle (NullRHIDevice) and
            // run the main loop windowless. This matches the headless
            // fallback the rest of the stack already handles.
            Spark::SimpleConsole::GetInstance().LogWarning(std::string("SDL_CreateWindow failed: ") + SDL_GetError() +
                                                           " — falling back to windowless / NullRHIDevice mode");
            if (preferVulkan)
            {
                SDL_Vulkan_UnloadLibrary();
                preferVulkan = false;
            }
        }
    }

    // On macOS, extract a Metal-capable view so MetalDevice can attach a
    // CAMetalLayer. The helper returns an NSView (opaque void*) whose layer
    // is already a CAMetalLayer; we pass it through as the NativeWindowHandle
    // so MetalSwapChain::ConfigureMetalLayer() reuses it. On non-macOS the
    // call is a no-op that returns nullptr.
    void* sdlMetalView = nullptr;
    if (preferMetal && window)
    {
        sdlMetalView = Spark::MacOS::CreateMetalView(window);
        if (!sdlMetalView)
        {
            // Metal framework unavailable / sandbox restriction. Tear down
            // the Metal window and continue windowless — RHIBridge will
            // select NullRHIDevice and the engine will run headlessly rather
            // than aborting.
            Spark::SimpleConsole::GetInstance().LogWarning(
                "Spark::MacOS::CreateMetalView returned null — running windowless on NullRHIDevice");
            SDL_DestroyWindow(window);
            window = nullptr;
            preferMetal = false;
        }
    }

    // OpenGL needs an SDL-owned GL context up front so the engine's
    // OpenGLDevice can detect it and skip its own EGL/GLX bootstrap.
    // Vulkan has no matching pre-init step — VulkanDevice pulls the
    // surface out of SDL_Vulkan_CreateSurface() inside CreateSwapChain().
    // Metal has no pre-init step either — the Metal view was created above.
    SDL_GLContext glContext = nullptr;
    if (!preferVulkan && !preferMetal && window)
    {
        glContext = SDL_GL_CreateContext(window);
        if (!glContext)
        {
            Spark::SimpleConsole::GetInstance().LogWarning(std::string("SDL_GL_CreateContext failed: ") +
                                                           SDL_GetError() + " — engine will try headless fallback");
        }
        else
        {
            SDL_GL_MakeCurrent(window, glContext);
            SDL_GL_SetSwapInterval(1);
            Spark::SimpleConsole::GetInstance().LogInfo("SDL2 OpenGL context created successfully");
        }
    }
    else
    {
        // Vulkan was preferred but RHIBridge may fall back to OpenGL if
        // VulkanDevice fails. Pre-set GL attributes so a context can be
        // created on a recreated window if needed (see fallback below).
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    }

    void* nativeRenderHandle = (preferMetal && sdlMetalView) ? sdlMetalView : nullptr;

    // Initialize graphics here (not inside InitializeSDL2Subsystems) so we
    // can detect the Vulkan→OpenGL fallback case below. SDL2 bakes the
    // backend choice into the window flags at creation time, so if
    // RHIBridge falls back from Vulkan to OpenGL we need to recreate the
    // window with SDL_WINDOW_OPENGL and re-run graphics init.
    InitializeGraphicsForWindow(window, nativeRenderHandle);

    if (preferVulkan)
    {
        auto* rhiDev = GetEngineRuntime().graphics->GetRHIDevice();
        auto* rhiBridge = GetEngineRuntime().graphics->GetRHIBridge();
        const bool vulkanActive = rhiDev && rhiDev->GetBackendType() == Spark::RHI::GraphicsBackend::Vulkan;
        const bool headless = rhiBridge && rhiBridge->IsHeadless();

        if (!vulkanActive && !headless)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "Vulkan requested but RHI selected %s — recreating window with SDL_WINDOW_OPENGL",
                           rhiDev ? Spark::RHI::GetBackendName(rhiDev->GetBackendType()) : "<null>");

            // Tear down graphics + Vulkan window before rebuilding.
            GetEngineRuntime().graphics->Shutdown();
            GetEngineRuntime().graphics.reset();
            SDL_DestroyWindow(window);
            SDL_Vulkan_UnloadLibrary();
            preferVulkan = false;

            windowFlags &= ~static_cast<Uint32>(SDL_WINDOW_VULKAN);
            windowFlags |= SDL_WINDOW_OPENGL;

            SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
            SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

            window = SDL_CreateWindow("Spark Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH,
                                      windowFlags);
            if (!window)
            {
                // Second-chance GL window creation also failed. Don't abort;
                // initialize graphics against a null handle and run the engine
                // on NullRHIDevice, matching the noGpuBackend path above.
                Spark::SimpleConsole::GetInstance().LogWarning(std::string("SDL_CreateWindow (GL fallback) failed: ") +
                                                               SDL_GetError() +
                                                               " — falling back to windowless / NullRHIDevice mode");
            }
            else
            {
                glContext = SDL_GL_CreateContext(window);
                if (!glContext)
                {
                    Spark::SimpleConsole::GetInstance().LogWarning(
                        std::string("SDL_GL_CreateContext (GL fallback) failed: ") + SDL_GetError() +
                        " — engine will try headless fallback");
                }
                else
                {
                    SDL_GL_MakeCurrent(window, glContext);
                    SDL_GL_SetSwapInterval(1);
                    Spark::SimpleConsole::GetInstance().LogInfo("SDL2 OpenGL context created after Vulkan fallback");
                }
            }

            // Metal view is only valid for a preferMetal path — Vulkan fallback
            // never creates one, so nativeRenderHandle stays null here.
            InitializeGraphicsForWindow(window, /*nativeRenderHandle=*/nullptr);
        }
    }

    InitializeSDL2Subsystems(window, argc, argv);
    RunSDL2MainLoop(/*pollSdlEvents=*/sdlInitOk);

    ShutdownLinux();
    if (glContext)
        SDL_GL_DeleteContext(glContext);
    Spark::MacOS::DestroyMetalView(sdlMetalView);
    if (window)
        SDL_DestroyWindow(window);
    if (sdlInitOk)
    {
        if (preferVulkan)
            SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
    }
    return 0;
}

#endif // SPARK_SDL2_AVAILABLE
#endif // !SPARK_PLATFORM_WINDOWS
