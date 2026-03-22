/**
 * @file EditorApplication.h
 * @brief Main SparkEditor application class with cross-platform graphics and ImGui integration
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <string>
#include <memory>
#include <chrono>

#include "EditorPluginManager.h"

#ifdef _WIN32
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#else
struct SDL_Window;
typedef void* SDL_GLContext;
#endif

// Forward declarations
namespace SparkEditor
{
    class EditorUI;
}

namespace SparkEditor
{

    /**
 * @brief Configuration for the editor
 */
    struct EditorConfig
    {
        std::string projectPath = ".";           ///< Root directory of the project being edited.
        std::string layoutDirectory = "Layouts"; ///< Directory for saved ImGui layout files (.ini).
        std::string logDirectory = "Logs";       ///< Directory for editor log output files.
        bool enableLogging = true;               ///< Whether to write log files to disk.
        bool startMaximized = true;              ///< Whether to maximize the window on startup.
        float autoSaveInterval = 30.0f;          ///< Seconds between auto-save of the current scene.
        int windowWidth = 1600;                  ///< Initial window width (pixels).
        int windowHeight = 900;                  ///< Initial window height (pixels).
    };

    /**
 * @brief Main SparkEditor application class with full GUI support
 */
    class EditorApplication
    {
      public:
        /**
     * @brief Performance metrics structure
     */
        struct PerformanceMetrics
        {
            float fps = 0.0f;       ///< Frames per second (smoothed over recent frames).
            float frameTime = 0.0f; ///< Last frame duration in milliseconds.
        };

      public:
        EditorApplication();
        ~EditorApplication();

        bool Initialize(const EditorConfig& config);
        int Run();
        void Shutdown();

        bool IsRunning() const { return m_isRunning; }
        void RequestExit();

        PerformanceMetrics GetPerformanceMetrics() const;
        EditorPluginManager& GetPluginManager() { return m_pluginManager; }
        void OnWindowResize(int width, int height);
        bool OnShutdownRequested();
        void SetWindowTitle(const std::string& title);

#ifdef _WIN32
        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

      private:
        bool CreateMainWindow(const EditorConfig& config);
        bool InitializeGraphics();
        bool InitializeImGui();
        bool ProcessMessages();
        void Update(float deltaTime);
        void Render();
        void UpdatePerformanceMetrics();

      private:
        // Configuration and state
        EditorConfig m_config;
        bool m_isInitialized = false;
        bool m_isRunning = false;

        // Window management
        int m_windowWidth = 1600;
        int m_windowHeight = 900;

#ifdef _WIN32
        HWND m_hwnd = nullptr;

        // DirectX 11 resources
        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
#else
        SDL_Window* m_window = nullptr;
        SDL_GLContext m_glContext = nullptr;
#endif

        // UI system
        std::unique_ptr<EditorUI> m_ui;

        // Plugin system
        EditorPluginManager m_pluginManager;

        // Performance tracking
        PerformanceMetrics m_performanceMetrics;
        std::chrono::high_resolution_clock::time_point m_startTime;
        std::chrono::high_resolution_clock::time_point m_lastFrameTime;
    };

} // namespace SparkEditor
