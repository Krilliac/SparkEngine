/**
 * @file EditorApplication.cpp
 * @brief Implementation of the enhanced editor application class
 * @author Spark Engine Team
 * @date 2025
 *
 * @note MEMORY INTEGRITY GUARDS: NOT INCLUDED in editor code.
 * Reason: The editor is a development tool, not a shipping game client.
 * Protecting editor branches would interfere with normal development
 * workflows (breakpoints, hot-reload, live editing). Memory integrity
 * is enforced in the engine runtime and game modules only.
 */

#include "EditorApplication.h"
#include "EditorUI.h"
#include "EditorFonts.h"
#include "EditorWindowManager.h"
#include "Core/FaultIsolation.h"
#include "EditorCrashHandler.h"
#include "EditorPluginManager.h"
#include "ProjectManager.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include "Utils/LogMacros.h"
#include <memory>
#include <iostream>
#include <stdexcept>

// Dear ImGui includes
#include <imgui.h>

#ifdef _WIN32
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
using Microsoft::WRL::ComPtr;

// External ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#else
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <SDL.h>
#include <SDL_opengl.h>
#endif

#include <chrono>
#include <filesystem>

namespace SparkEditor
{

    // Out-of-class definition of the static application pointer. The
    // declaration lives in EditorApplication.h so WindowProc and other
    // methods defined in EditorApplicationWindows.cpp can reach it via
    // the class scope (EditorApplication::s_instance).
    EditorApplication* EditorApplication::s_instance = nullptr;

    EditorApplication::EditorApplication()
        : m_startTime(std::chrono::high_resolution_clock::now()), m_lastFrameTime(m_startTime)
    {
        s_instance = this;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorApplication constructed");
    }

    EditorApplication::~EditorApplication()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorApplication destructor called");
        if (m_isInitialized || m_initializationStarted)
        {
            Shutdown();
        }
        s_instance = nullptr;
    }

    bool EditorApplication::Initialize(const EditorConfig& config)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("Initializing Enhanced Spark Engine Editor...");

        if (m_isInitialized || m_initializationStarted)
        {
            console.LogError("EditorApplication is already initialized or initializing");
            return false;
        }
        m_initializationStarted = true;
        const auto failInitialization = [this]()
        {
            Shutdown();
            return false;
        };

        m_config = config;
        m_windowWidth = config.windowWidth;
        m_windowHeight = config.windowHeight;

        // Create main editor window
        console.LogInfo("Creating main editor window...");
        if (!CreateMainWindow(config))
        {
            console.LogError("Failed to create main editor window");
            return failInitialization();
        }
        console.LogSuccess("Main window created successfully (" + std::to_string(m_windowWidth) + "x" +
                           std::to_string(m_windowHeight) + ")");

        // Initialize graphics backend
        console.LogInfo("Initializing graphics backend...");
        if (!InitializeGraphics())
        {
            console.LogError("Failed to initialize graphics backend");
            return failInitialization();
        }
        console.LogSuccess("Graphics backend initialized successfully");

        // Initialize Dear ImGui
        console.LogInfo("Initializing Dear ImGui...");
        if (!InitializeImGui())
        {
            console.LogError("Failed to initialize Dear ImGui");
            return failInitialization();
        }
        m_imguiInitialized = true;
        console.LogSuccess("Dear ImGui initialized successfully");

        // Initialize window manager singleton — owns multi-monitor placement,
        // floating-window state, and on-disk layout persistence. Ordered
        // before EditorUI so panels can query / restore their window state
        // while they are being created.
        console.LogInfo("Initializing window manager...");
        m_windowManagerInitialized = true;
        EditorWindowManager::GetInstance().Initialize();
        console.LogSuccess("Window manager initialized");

        // Initialize EditorUI (this creates all panels including console)
        console.LogInfo("Initializing EditorUI...");
        m_ui = std::make_unique<EditorUI>();
        if (!m_ui->Initialize(config))
        {
            console.LogError("Failed to initialize EditorUI");
            return failInitialization();
        }
        // Give EditorUI access to the plugin manager for menu bar rendering
        m_ui->SetPluginManager(&m_pluginManager);
        console.LogSuccess("EditorUI initialized successfully");

#ifdef _WIN32
        // Pass the DirectX device to panels that need it (Scene View)
        if (!m_device || !m_context)
        {
            console.LogError("Graphics device/context became unavailable during editor initialization");
            return failInitialization();
        }
        m_ui->SetGraphicsDevice(m_device.Get(), m_context.Get());
#endif

        // Explicit plugin directories are project-rooted and bounded by the
        // discovery policy. Mark ownership active before loading so any
        // partial failure is rolled back by the normal initialization cleanup.
        m_pluginLifecycleStarted = true;
        const std::string activeProjectRoot = ProjectManager::GetActiveProjectPath();
        if (!config.editorPluginDirectories.empty() && activeProjectRoot.empty())
        {
            console.LogError("Editor plugin directories require a successfully opened active project");
            return failInitialization();
        }
        for (const std::string& pluginDirectory : config.editorPluginDirectories)
        {
            size_t loadedCount = 0;
            if (!m_pluginManager.LoadPluginsFromProjectDirectory(activeProjectRoot, pluginDirectory, &loadedCount))
            {
                console.LogError("Failed to load editor plugins from '" + pluginDirectory + "'");
                return failInitialization();
            }
            console.LogInfo("Loaded " + std::to_string(loadedCount) + " editor plugin(s) from '" + pluginDirectory +
                            "'");
        }

        // Initialize plugin system
        console.LogInfo("Initializing editor plugins...");
        if (!m_pluginManager.InitializeAll(this))
        {
            console.LogError("One or more editor plugins failed to initialize");
            return failInitialization();
        }
        console.LogSuccess("Editor plugins initialized");

        m_isInitialized = true;
        m_initializationStarted = false;
        m_isRunning = true;

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Editor initialization complete (window %dx%d)", m_windowWidth,
                       m_windowHeight);
        console.LogSuccess("Enhanced Editor initialization complete");
        console.LogInfo("SparkEditor is now ready for use");
        console.LogInfo("All editor operations will be logged to external console");

        return true;
    }

    // Platform-specific methods (CreateMainWindow, InitializeGraphics, Run, Shutdown)
    // live in EditorApplicationWindows.cpp and EditorApplicationLinux.cpp.

    // =========================================================================
    // Shared implementation (cross-platform)
    // =========================================================================

    void EditorApplication::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        // Update UI system
        if (m_ui)
        {
            SPARK_GUARDED_UPDATE("EditorUI", "Editor", { m_ui->Update(deltaTime); });

            // Check if user requested exit via File > Exit
            if (m_ui->IsExitRequested())
            {
                if (SaveProjectForShutdown())
                {
                    RequestExit();
                }
                else
                {
                    // Keep the editor alive when final persistence fails. The
                    // user can fix the problem and request exit again.
                    m_ui->ClearExitRequest();
                    m_ui->ShowNotification("Project save failed; exit cancelled", "error");
                }
            }
        }

        // Update editor plugins
        m_pluginManager.UpdateAll(deltaTime);
    }

    void EditorApplication::UpdatePerformanceMetrics()
    {
        static float frameTimeAccumulator = 0.0f;
        static int frameCount = 0;
        static auto lastUpdateTime = std::chrono::high_resolution_clock::now();

        auto currentTime = std::chrono::high_resolution_clock::now();
        float frameTime = std::chrono::duration<float>(currentTime - m_lastFrameTime).count();
        m_lastFrameTime = currentTime;

        frameTimeAccumulator += frameTime;
        frameCount++;

        // Update metrics every 0.5 seconds
        if (std::chrono::duration<float>(currentTime - lastUpdateTime).count() >= 0.5f)
        {
            m_performanceMetrics.fps = frameCount / frameTimeAccumulator;
            m_performanceMetrics.frameTime = (frameTimeAccumulator / frameCount) * 1000.0f;

            frameTimeAccumulator = 0.0f;
            frameCount = 0;
            lastUpdateTime = currentTime;
        }
    }

    void EditorApplication::RequestExit()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Exit requested");
        m_isRunning = false;
    }

    EditorApplication::PerformanceMetrics EditorApplication::GetPerformanceMetrics() const
    {
        return m_performanceMetrics;
    }

    bool EditorApplication::OnShutdownRequested()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutdown requested by user");
        if (m_ui && !m_ui->RequestExitWithConfirmation())
            return false;

        if (!SaveProjectForShutdown())
        {
            if (m_ui)
            {
                m_ui->ClearExitRequest();
                m_ui->ShowNotification("Project save failed; exit cancelled", "error");
            }
            return false;
        }
        return true;
    }

    bool EditorApplication::SaveProjectForShutdown()
    {
        if (!m_ui)
            return true;

        auto* pm = m_ui->GetProjectManager();
        if (!pm || !pm->HasOpenProject())
            return true;

        auto& console = Spark::SimpleConsole::GetInstance();
        if (!pm->SaveProject())
        {
            console.LogError("Failed to save project; shutdown cancelled");
            return false;
        }
        console.LogInfo("Project auto-saved on exit");
        return true;
    }

} // namespace SparkEditor
