/**
 * @file EditorApplicationLinux.cpp
 * @brief Linux/SDL2+OpenGL editor application — window creation, graphics init, run loop, shutdown
 *
 * Contains SDL2 window, OpenGL 3.3 context, ImGui SDL2 backend,
 * and the SDL event loop. Windows counterpart lives in EditorApplicationWindows.cpp.
 * Shared code (Initialize, Update, RenderFrame) stays in EditorApplication.cpp.
 */
#include "EditorApplication.h"

#ifndef _WIN32

#include "EditorUI.h"
#include "EditorFonts.h"
#include "ProjectManager.h"
#include "EditorWindowManager.h"
#include "Core/FaultIsolation.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <SDL.h>
#include <SDL_opengl.h>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace SparkEditor
{

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

        // Keep per-user editor state out of the repository/current working
        // directory. ImGui retains this pointer until context shutdown.
        const std::filesystem::path editorDataDirectory = ProjectManager::GetEditorDataDirectory();
        std::error_code directoryError;
        std::filesystem::create_directories(editorDataDirectory, directoryError);
        m_imguiIniPath = (editorDataDirectory / "imgui.ini").string();
        io.IniFilename = m_imguiIniPath.c_str();

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
            ImGui::DestroyContext();
            return false;
        }

        const char* glslVersion = "#version 330 core";
        if (!ImGui_ImplOpenGL3_Init(glslVersion))
        {
            std::cerr << "Failed to initialize ImGui OpenGL3 backend\n";
            ImGui_ImplSDL2_Shutdown();
            ImGui::DestroyContext();
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
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Run() called but editor not initialized");
            console.LogCritical("EditorApplication::Run() called but editor not initialized!");
            return -1;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Starting editor main loop (SDL2/OpenGL)");
        console.LogInfo("Starting enhanced editor main loop...");

        auto lastTime = std::chrono::high_resolution_clock::now();
        int frameCount = 0;

        while (m_isRunning)
        {
            // Test mode frame limit
            if (m_config.testFrameLimit > 0 && frameCount >= m_config.testFrameLimit)
            {
                std::cout << "[TEST] Frame limit reached (" << m_config.testFrameLimit << " frames). Exiting.\n"
                          << std::flush;
                m_isRunning = false;
                break;
            }
            ++frameCount;

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
                if (OnShutdownRequested())
                    return false;
                break;

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
        if (m_pluginLifecycleStarted)
        {
            console.LogInfo("Shutting down editor plugins...");
            m_pluginManager.ShutdownAll();
            m_pluginLifecycleStarted = false;
            console.LogSuccess("Editor plugins shutdown complete");
        }

        if (m_ui)
        {
            console.LogInfo("Shutting down EditorUI...");
            m_ui->Shutdown();
            m_ui.reset();
            console.LogSuccess("EditorUI shutdown complete");
        }

        // Window manager shutdown after EditorUI so any auto-save picks
        // up the final panel state.
        if (m_windowManagerInitialized)
        {
            console.LogInfo("Shutting down window manager...");
            EditorWindowManager::GetInstance().Shutdown();
            m_windowManagerInitialized = false;
            console.LogSuccess("Window manager shutdown complete");
        }

        // Cleanup ImGui
        if (m_imguiInitialized)
        {
            console.LogInfo("Cleaning up Dear ImGui...");
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplSDL2_Shutdown();
            ImGui::DestroyContext();
            m_imguiInitialized = false;
            console.LogSuccess("Dear ImGui cleanup complete");
        }

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
        m_initializationStarted = false;
        console.LogSuccess("Enhanced editor shutdown complete");
    }

} // namespace SparkEditor

#endif // !_WIN32
