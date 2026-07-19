/**
 * @file SparkEngineLinuxSDL2Events.cpp
 * @brief SDL2 event translation, dispatch, and per-frame main loop (Linux + macOS).
 *
 * Split from SparkEngineLinux.cpp to keep files under the ~500-line guideline.
 * Contains the SDL keycode → Win32 VK translation, the SDL event dispatcher,
 * and RunSDL2MainLoop. Window creation and subsystem bring-up live in
 * SparkEngineLinuxSDL2.cpp; the shared TickFrame lives in SparkEngineLinuxInit.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "SparkEngineLinuxInternal.h"
#include "EngineRuntime.h"
#include "ModuleManager.h"
#include "Engine/Events/EventSystem.h"
#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "Utils/Timer.h"
#include "Utils/SparkConsole.h"
#ifdef SPARK_SDL2_AVAILABLE
#include <SDL.h>
#endif
#include <cstdint>
#include <format>

#ifndef SPARK_PLATFORM_WINDOWS
#ifdef SPARK_SDL2_AVAILABLE

/**
 * @brief Translate an SDL key symbol to a Win32 virtual key code.
 *
 * InputManager uses WM_KEYDOWN/WM_KEYUP style messages internally,
 * so SDL key events must be translated to VK_* codes for consistent handling.
 *
 * @return The corresponding VK_* code, or 0 if the key is not mapped.
 */
static int TranslateSDLKeyToVK(SDL_Keycode sym)
{
    // Alphabetic keys
    if (sym >= SDLK_a && sym <= SDLK_z)
        return 'A' + (sym - SDLK_a);

    // Numeric keys
    if (sym >= SDLK_0 && sym <= SDLK_9)
        return '0' + (sym - SDLK_0);

    // Function keys
    if (sym >= SDLK_F1 && sym <= SDLK_F12)
        return VK_F1 + (sym - SDLK_F1);

    // Named keys
    switch (sym)
    {
    case SDLK_SPACE:
        return VK_SPACE;
    case SDLK_ESCAPE:
        return VK_ESCAPE;
    case SDLK_RETURN:
        return VK_RETURN;
    case SDLK_TAB:
        return VK_TAB;
    case SDLK_BACKSPACE:
        return VK_BACK;
    case SDLK_UP:
        return VK_UP;
    case SDLK_DOWN:
        return VK_DOWN;
    case SDLK_LEFT:
        return VK_LEFT;
    case SDLK_RIGHT:
        return VK_RIGHT;
    case SDLK_LSHIFT:
        return VK_LSHIFT;
    case SDLK_RSHIFT:
        return VK_RSHIFT;
    case SDLK_LCTRL:
        return VK_LCONTROL;
    case SDLK_RCTRL:
        return VK_RCONTROL;
    case SDLK_LALT:
        return VK_LMENU;
    case SDLK_RALT:
        return VK_RMENU;
    case SDLK_DELETE:
        return VK_DELETE;
    default:
        return 0;
    }
}

/**
 * @brief Dispatch a single SDL event to the appropriate engine subsystem.
 *
 * Handles window close/resize, keyboard, and mouse events by translating
 * them into the InputManager's message format.
 *
 * @param event The SDL event to process.
 * @return false if the application should quit, true otherwise.
 */
static bool HandleSDLEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_QUIT:
        return false;

    case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_CLOSE)
            return false;
        if (event.window.event == SDL_WINDOWEVENT_RESIZED)
        {
            int w = event.window.data1;
            int h = event.window.data2;
            if (GetEngineRuntime().graphics)
                GetEngineRuntime().graphics->OnResize(w, h);
            if (GetEngineRuntime().moduleManager)
                GetEngineRuntime().moduleManager->ResizeAll(w, h);
            if (GetEngineRuntime().eventBus)
                GetEngineRuntime().eventBus->Publish(
                    Spark::WindowResizeEvent{static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
        }
        break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (GetEngineRuntime().input)
        {
            UINT msg = (event.type == SDL_KEYDOWN) ? WM_KEYDOWN : WM_KEYUP;
            int vk = TranslateSDLKeyToVK(event.key.keysym.sym);
            if (vk != 0)
                GetEngineRuntime().input->HandleMessage(msg, static_cast<WPARAM>(vk), 0);
        }
        break;

    case SDL_MOUSEMOTION:
        if (GetEngineRuntime().input)
            GetEngineRuntime().input->HandleMessage(
                WM_MOUSEMOVE, 0, static_cast<LPARAM>((event.motion.y << 16) | (event.motion.x & 0xFFFF)));
        break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (GetEngineRuntime().input)
        {
            UINT msg = 0;
            if (event.button.button == SDL_BUTTON_LEFT)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
            else if (event.button.button == SDL_BUTTON_RIGHT)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_RBUTTONDOWN : WM_RBUTTONUP;
            else if (event.button.button == SDL_BUTTON_MIDDLE)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_MBUTTONDOWN : WM_MBUTTONUP;
            if (msg)
                GetEngineRuntime().input->HandleMessage(msg, 0, 0);
        }
        break;
    }

    return true;
}

/**
 * @brief Run the SDL2 event pump and per-frame engine tick loop.
 *
 * Processes SDL events via HandleSDLEvent(), then calls TickFrame() for
 * the engine update. Returns when the window is closed or SIGINT is received.
 *
 * @param pollSdlEvents When false (SDL_Init failed), skip event polling and
 *        run a pure tick loop. The engine still ticks frames and respects
 *        SIGINT / test-frame-limit, just without SDL event input.
 */
void RunSDL2MainLoop(bool pollSdlEvents)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo(pollSdlEvents ? "Starting main engine loop (SDL2)..."
                                  : "Starting main engine loop (SDL2 uninitialized — tick only)...");

    if (g_testFrameLimit > 0)
        console.LogInfo(std::format("Test mode: will exit after {} frames", g_testFrameLimit));

    int frameCount = 0;

    while (!g_shutdownRequested)
    {
        if (g_testFrameLimit > 0 && frameCount >= g_testFrameLimit)
        {
            console.LogInfo(std::format("[TEST] Frame limit reached ({} frames). Exiting.", g_testFrameLimit));
            break;
        }

        bool running = true;

        if (pollSdlEvents)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (!HandleSDLEvent(event))
                {
                    running = false;
                    break;
                }
            }
        }

        if (!running)
            break;

        float dt = GetEngineRuntime().timer ? GetEngineRuntime().timer->GetDeltaTime() : 0.016f;
        TickFrame(dt);
        ++frameCount;
    }
}

#endif // SPARK_SDL2_AVAILABLE
#endif // !SPARK_PLATFORM_WINDOWS
