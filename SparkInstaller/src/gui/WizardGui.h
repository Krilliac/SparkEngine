/**
 * @file WizardGui.h
 * @brief The SparkInstaller GUI wizard (Ember Signal) and the platform backend it runs on.
 *
 * Thread affinity: WizardGui::Run and every GuiBackend_* function belong to the thread that
 * calls main() (the window, the ImGui context and the Spark UI runtime live there). The install
 * itself runs on one worker thread owned by Run; the worker touches only the InstallerContext
 * and the mutex-guarded log, and is joined before Run returns.
 * Ownership: Run borrows the InstallerContext for its whole duration and restores the log sink
 * it found. A backend owns its window, graphics device and ImGui context from a successful
 * GuiBackend_Init until GuiBackend_Shutdown.
 */

#pragma once

#include "InstallerContext.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace SparkInstaller
{
    /// @brief Logical (100 %) client width of the wizard window.
    inline constexpr int kGuiWindowWidth = 860;
    /// @brief Logical (100 %) client height of the wizard window.
    inline constexpr int kGuiWindowHeight = 600;

    /// @brief The interactive ImGui wizard. It collects the install choices, runs the install
    ///        itself on a worker thread so it can show progress, and reports the result.
    class WizardGui
    {
      public:
        /// @brief Show the wizard until the user finishes or closes it.
        /// @details When an install ran, its exit code is available from ExitCode(). Closing the
        ///          window while an install is running waits for the current step to finish;
        ///          there is no cancel. The context's log sink is restored before returning.
        /// @return False when the window or graphics device could not be created (nothing was
        ///         shown and ExitCode() is 0); true when the wizard ran.
        bool Run(InstallerContext& ctx);

        /// @brief Exit code of the install the wizard ran (an Installer::Run code), or 0 when the
        ///        user left before starting one.
        int ExitCode() const noexcept { return m_exitCode; }

      private:
        int m_exitCode = 0;
    };

    /// @brief Desktop accessibility settings the wizard honours.
    struct GuiSystemPreferences
    {
        bool reduceMotion = false; ///< The desktop asks for no animation.
        bool highContrast = false; ///< A system high-contrast theme is active.
    };

    // Platform backends (implemented by one of BackendWindows.cpp or BackendLinux.cpp).

    /// @brief Create the DPI-aware, centred window, the graphics device and the ImGui context
    ///        with its platform and renderer backends.
    /// @param width Client width in logical pixels (scaled by the monitor DPI).
    /// @param height Client height in logical pixels.
    /// @return True on success. On failure everything created so far is already released and the
    ///         reason is on stderr; GuiBackend_Shutdown need not be called.
    bool GuiBackend_Init(int width, int height, const char* title);

    /// @brief DPI scale (1.0 = 96 DPI) of the window's monitor; 1.0 on macOS.
    float GuiBackend_DpiScale();

    /// @brief Read the desktop's reduced-motion and high-contrast settings.
    /// @details Linux and macOS report neither setting, so both are false there.
    GuiSystemPreferences GuiBackend_QuerySystemPreferences();

    /// @brief Directory of the running executable, where the wizard looks for staged fonts.
    std::filesystem::path GuiBackend_ExecutableDir();

    /// @brief Pump window events and start an ImGui frame.
    /// @details Applies a pending DPI change to the Spark UI runtime before ImGui::NewFrame().
    /// @return False once the window is gone and the wizard must stop.
    bool GuiBackend_BeginFrame();

    /// @brief Render the frame started by GuiBackend_BeginFrame and present it.
    void GuiBackend_EndFrame();

    /// @brief True once per user request to close the window (the title-bar close button).
    /// @details The window is not closed by the request; the wizard decides, so it can refuse
    ///          to close under a running install.
    bool GuiBackend_ConsumeCloseRequest();

    /// @brief Destroy the ImGui context, graphics device and window. Safe after a failed or
    ///        partial GuiBackend_Init and safe to call twice.
    void GuiBackend_Shutdown();

#ifdef _WIN32
    /// @brief Show the shell folder picker (IFileOpenDialog, folders only) owned by the window.
    /// @param startPath UTF-8 folder to start in; the nearest existing ancestor is used.
    /// @param chosen Receives the picked folder as UTF-8 on success.
    /// @return True when the user picked a folder.
    bool GuiBackend_PickFolder(const std::string& startPath, std::string& chosen);

    /// @brief Colour the title bar like the wizard (Windows 10 1809+ dark mode, Windows 11
    ///        caption colours). Unsupported attributes are skipped silently.
    /// @param captionRgb Caption background as 0xRRGGBB.
    /// @param textRgb Caption text as 0xRRGGBB.
    void GuiBackend_SetCaptionColors(std::uint32_t captionRgb, std::uint32_t textRgb);
#endif
} // namespace SparkInstaller
