/**
 * @file LauncherBackendLinux.cpp
 * @brief SDL2 + OpenGL3 backend for SparkLauncher (Linux/macOS): DPI-scaled, centred,
 *        minimum-size window styled by the Spark UI kit.
 * @author Spark Engine Team
 * @date 2025
 */

#ifndef _WIN32

#include "LauncherBackend.h"
#include "LauncherViews.h"

#include "SparkUI/Dpi.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Tokens.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace SparkLauncher
{
    namespace
    {
        SDL_Window* g_window = nullptr;
        SDL_GLContext g_glContext = nullptr;
        bool g_sdlInitialized = false;
        bool g_sdlBackendReady = false;
        bool g_openGlBackendReady = false;
        bool g_quit = false;

        int ScaleToDevice(int logical, float scale)
        {
            return static_cast<int>(std::lround(static_cast<float>(logical) * scale));
        }

        /// Resize the window to spec x its DPI scale (capped to the display's usable area), centre it
        /// and set the matching minimum size.
        void ApplyScaledGeometry(const BackendWindowSpec& spec, float scale)
        {
            int width = ScaleToDevice(spec.width, scale);
            int height = ScaleToDevice(spec.height, scale);
            SDL_Rect usable{};
            const int display = SDL_GetWindowDisplayIndex(g_window);
            if (display >= 0 && SDL_GetDisplayUsableBounds(display, &usable) == 0 && usable.w > 0 && usable.h > 0)
            {
                width = std::min(width, usable.w);
                height = std::min(height, usable.h);
            }
            SDL_SetWindowMinimumSize(g_window, ScaleToDevice(spec.minWidth, scale),
                                     ScaleToDevice(spec.minHeight, scale));
            SDL_SetWindowSize(g_window, width, height);
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    } // namespace

    bool Backend_Init(const BackendWindowSpec& spec)
    {
        Spark::Ui::EnableProcessDpiAwareness();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
        {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        g_sdlInitialized = true;

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifdef __APPLE__
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif

        // Created hidden at the logical size; the DPI of the display it lands on decides the real size.
        g_window =
            SDL_CreateWindow(spec.title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, spec.width, spec.height,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
        if (!g_window)
        {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            Backend_Shutdown();
            return false;
        }

        g_glContext = SDL_GL_CreateContext(g_window);
        if (!g_glContext)
        {
            std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
            Backend_Shutdown();
            return false;
        }
        SDL_GL_MakeCurrent(g_window, g_glContext);
        SDL_GL_SetSwapInterval(1);

        const float dpiScale = Spark::Ui::WindowDpiScale(g_window);
        ApplyScaledGeometry(spec, dpiScale);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; // don't persist window layout for a modal launcher
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        InstallLauncherStyle(dpiScale, Spark::Ui::Appearance::Dark);

        g_sdlBackendReady = ImGui_ImplSDL2_InitForOpenGL(g_window, g_glContext);
        g_openGlBackendReady = g_sdlBackendReady && ImGui_ImplOpenGL3_Init("#version 330 core");
        if (!g_openGlBackendReady)
        {
            std::fprintf(stderr, "ImGui platform/renderer backend initialization failed\n");
            Backend_Shutdown();
            return false;
        }
        SDL_ShowWindow(g_window);
        return true;
    }

    bool Backend_BeginFrame()
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT)
                g_quit = true;
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                     e.window.windowID == SDL_GetWindowID(g_window))
                g_quit = true;
#if SDL_VERSION_ATLEAST(2, 0, 18)
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED &&
                     e.window.windowID == SDL_GetWindowID(g_window))
                Spark::Ui::RequestScale(Spark::Ui::WindowDpiScale(g_window), Spark::Ui::UserScale());
#endif
        }
        if (g_quit)
            return false;

        // A display change received above is applied here, between frames.
        Spark::Ui::BeginFrameScale();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        return true;
    }

    void Backend_EndFrame()
    {
        ImGui::Render();
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(g_window, &w, &h);
        glViewport(0, 0, w, h);
        const Spark::Ui::Rgba sunken = Spark::Ui::Get(Spark::Ui::ActiveTokens(), Spark::Ui::Role::SurfaceSunken);
        glClearColor(sunken.r, sunken.g, sunken.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(g_window);
    }

    void Backend_Shutdown()
    {
        if (g_openGlBackendReady)
        {
            ImGui_ImplOpenGL3_Shutdown();
            g_openGlBackendReady = false;
        }
        if (g_sdlBackendReady)
        {
            ImGui_ImplSDL2_Shutdown();
            g_sdlBackendReady = false;
        }
        if (ImGui::GetCurrentContext())
        {
            ImGui::DestroyContext();
        }
        if (g_glContext)
        {
            SDL_GL_DeleteContext(g_glContext);
            g_glContext = nullptr;
        }
        if (g_window)
        {
            SDL_DestroyWindow(g_window);
            g_window = nullptr;
        }
        if (g_sdlInitialized)
        {
            SDL_Quit();
            g_sdlInitialized = false;
        }
    }

    void Backend_ShowFatalError(const char* title, const char* message)
    {
        // SDL documents this as usable without SDL_Init; with no display it fails and is ignored.
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, nullptr);
    }
} // namespace SparkLauncher

#endif // !_WIN32
