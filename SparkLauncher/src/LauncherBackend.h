/**
 * @file LauncherBackend.h
 * @brief Platform backend API for SparkLauncher (Win32/D3D11 or SDL2/OpenGL).
 * @author Spark Engine Team
 * @date 2025
 *
 * Thread affinity: every function runs on the main (UI) thread. Ownership: the backend owns the
 * native window, the graphics device and the ImGui context between Backend_Init and
 * Backend_Shutdown.
 */

#pragma once

namespace SparkLauncher
{
    /// @brief Launcher window geometry in logical (100 %) pixels; the backend multiplies by the DPI scale.
    struct BackendWindowSpec
    {
        int width = 1100;
        int height = 700;
        int minWidth = 760;
        int minHeight = 520;
        const char* title = "Spark Launcher";
    };

    /**
     * @brief Make the process DPI aware, open the centred window, create the ImGui context and
     *        install the launcher style and fonts (LauncherViews.h).
     * @return False when the window or graphics device cannot be created.
     */
    bool Backend_Init(const BackendWindowSpec& spec);
    void Backend_Shutdown();
    bool Backend_BeginFrame(); // returns false when user requested quit
    void Backend_EndFrame();

    /**
     * @brief Tell the user that the launcher cannot start: a modal message box (MessageBoxW on
     *        Windows, an SDL message box elsewhere). Callable before Backend_Init and after
     *        Backend_Shutdown; failures to show the box are ignored.
     */
    void Backend_ShowFatalError(const char* title, const char* message);
} // namespace SparkLauncher
