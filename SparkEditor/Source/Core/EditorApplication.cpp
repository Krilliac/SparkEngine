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
        if (m_isInitialized)
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

        m_config = config;
        m_windowWidth = config.windowWidth;
        m_windowHeight = config.windowHeight;

        // Create main editor window
        console.LogInfo("Creating main editor window...");
        if (!CreateMainWindow(config))
        {
            console.LogError("Failed to create main editor window");
            return false;
        }
        console.LogSuccess("Main window created successfully (" + std::to_string(m_windowWidth) + "x" +
                           std::to_string(m_windowHeight) + ")");

        // Initialize graphics backend
        console.LogInfo("Initializing graphics backend...");
        if (!InitializeGraphics())
        {
            console.LogError("Failed to initialize graphics backend");
            return false;
        }
        console.LogSuccess("Graphics backend initialized successfully");

        // Initialize Dear ImGui
        console.LogInfo("Initializing Dear ImGui...");
        if (!InitializeImGui())
        {
            console.LogError("Failed to initialize Dear ImGui");
            return false;
        }
        console.LogSuccess("Dear ImGui initialized successfully");

        // Initialize window manager singleton — owns multi-monitor placement,
        // floating-window state, and on-disk layout persistence. Ordered
        // before EditorUI so panels can query / restore their window state
        // while they are being created.
        console.LogInfo("Initializing window manager...");
        EditorWindowManager::GetInstance().Initialize();
        console.LogSuccess("Window manager initialized");

        // Initialize EditorUI (this creates all panels including console)
        console.LogInfo("Initializing EditorUI...");
        m_ui = std::make_unique<EditorUI>();
        if (!m_ui->Initialize(config))
        {
            console.LogError("Failed to initialize EditorUI");
            return false;
        }
        // Give EditorUI access to the plugin manager for menu bar rendering
        m_ui->SetPluginManager(&m_pluginManager);
        console.LogSuccess("EditorUI initialized successfully");

#ifdef _WIN32
        // Pass the DirectX device to panels that need it (Scene View)
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Editor, m_device.Get(), false);
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Editor, m_context.Get(), false);
        if (m_device && m_context)
        {
            m_ui->SetGraphicsDevice(m_device.Get(), m_context.Get());
        }
#endif

        // Initialize plugin system
        console.LogInfo("Initializing editor plugins...");
        m_pluginManager.InitializeAll(this);
        console.LogSuccess("Editor plugins initialized");

        m_isInitialized = true;
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
                RequestExit();
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
        auto& console = Spark::SimpleConsole::GetInstance();

        // Check for unsaved scene changes
        if (m_ui && m_ui->IsSceneModified())
        {
            console.LogWarning("Exiting with unsaved scene changes in: " + m_ui->GetCurrentSceneName());
            // Allow exit but log the warning so user sees it in console
        }

        // Auto-save the project on close if one is open
        if (m_ui)
        {
            auto* pm = m_ui->GetProjectManager();
            if (pm && pm->HasOpenProject())
            {
                pm->SaveProject();
                console.LogInfo("Project auto-saved on exit");
            }
        }

        return true;
    }

} // namespace SparkEditor
