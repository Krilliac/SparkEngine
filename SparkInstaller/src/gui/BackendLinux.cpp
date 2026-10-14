/**
 * @file BackendLinux.cpp
 * @brief SDL2 + OpenGL 3.3 backend of the installer GUI for Linux and macOS: a centred,
 *        fixed-size window scaled by the display DPI.
 */

#ifndef _WIN32

#include "WizardGui.h"

#include "SparkUI/Dpi.h"
#include "SparkUI/Runtime.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <SDL.h>
#include <SDL_opengl.h>

#include <cmath>
#include <cstdio>

namespace SparkInstaller
{
    namespace
    {
        SDL_Window* g_window = nullptr;
        SDL_GLContext g_glContext = nullptr;
        bool g_sdlReady = false;
        bool g_sdlBackendReady = false;
        bool g_glBackendReady = false;
        bool g_quit = false;
        bool g_closeRequested = false;
        float g_dpiScale = 1.0f;

        // macOS reports the Retina factor through the drawable size, which the ImGui SDL2 backend
        // already applies as its framebuffer scale; scaling the UI again would double it.
        float QueryDpiScale()
        {
#ifdef __APPLE__
            return 1.0f;
#else
            return Spark::Ui::WindowDpiScale(g_window);
#endif
        }
    } // namespace

    bool GuiBackend_Init(int width, int height, const char* title)
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
        {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        g_sdlReady = true;

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#ifdef __APPLE__
        // macOS only offers core profiles above 2.1 to forward-compatible contexts.
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

        // Created hidden: the size is corrected for the display DPI before the first show.
        g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
        if (!g_window)
        {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            GuiBackend_Shutdown();
            return false;
        }

        g_glContext = SDL_GL_CreateContext(g_window);
        if (!g_glContext)
        {
            std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
            GuiBackend_Shutdown();
            return false;
        }
        SDL_GL_MakeCurrent(g_window, g_glContext);
        SDL_GL_SetSwapInterval(1);

        g_dpiScale = QueryDpiScale();
        if (g_dpiScale > 1.0f)
        {
            SDL_SetWindowSize(g_window, static_cast<int>(std::lround(static_cast<float>(width) * g_dpiScale)),
                              static_cast<int>(std::lround(static_cast<float>(height) * g_dpiScale)));
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        if (!ImGui_ImplSDL2_InitForOpenGL(g_window, g_glContext))
        {
            std::fprintf(stderr, "ImGui_ImplSDL2_InitForOpenGL failed\n");
            GuiBackend_Shutdown();
            return false;
        }
        g_sdlBackendReady = true;
        if (!ImGui_ImplOpenGL3_Init("#version 330 core"))
        {
            std::fprintf(stderr, "ImGui_ImplOpenGL3_Init failed\n");
            GuiBackend_Shutdown();
            return false;
        }
        g_glBackendReady = true;

        SDL_ShowWindow(g_window);
        return true;
    }

    float GuiBackend_DpiScale()
    {
        return g_dpiScale;
    }

    GuiSystemPreferences GuiBackend_QuerySystemPreferences()
    {
        // SDL2 does not expose the desktop's reduced-motion or high-contrast settings.
        return GuiSystemPreferences{};
    }

    std::filesystem::path GuiBackend_ExecutableDir()
    {
        char* base = SDL_GetBasePath();
        if (base == nullptr)
            return {};
        std::filesystem::path dir(base);
        SDL_free(base);
        // SDL ends the base path with a separator, which would leave an empty filename.
        return dir.has_filename() ? dir : dir.parent_path();
    }

    bool GuiBackend_BeginFrame()
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT)
                g_quit = true;
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                     e.window.windowID == SDL_GetWindowID(g_window))
                g_closeRequested = true;
        }
        if (g_quit)
            return false;

        // A window dragged to a monitor with another DPI changes scale between frames.
        const float dpiScale = QueryDpiScale();
        if (std::fabs(dpiScale - g_dpiScale) > 0.01f)
        {
            g_dpiScale = dpiScale;
            Spark::Ui::RequestScale(g_dpiScale, 1.0f);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        // Apply a pending DPI change before NewFrame so the scale never changes mid-frame.
        Spark::Ui::BeginFrameScale();
        ImGui::NewFrame();
        return true;
    }

    void GuiBackend_EndFrame()
    {
        ImGui::Render();
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(g_window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(g_window);
    }

    bool GuiBackend_ConsumeCloseRequest()
    {
        const bool requested = g_closeRequested;
        g_closeRequested = false;
        return requested;
    }

    void GuiBackend_Quit()
    {
        g_quit = true;
    }

    void GuiBackend_Shutdown()
    {
        if (g_glBackendReady)
        {
            ImGui_ImplOpenGL3_Shutdown();
            g_glBackendReady = false;
        }
        if (g_sdlBackendReady)
        {
            ImGui_ImplSDL2_Shutdown();
            g_sdlBackendReady = false;
        }
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
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
        if (g_sdlReady)
        {
            SDL_Quit();
            g_sdlReady = false;
        }
        g_quit = false;
        g_closeRequested = false;
    }
} // namespace SparkInstaller

#endif // !_WIN32
