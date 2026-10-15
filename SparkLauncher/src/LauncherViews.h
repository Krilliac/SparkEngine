/**
 * @file LauncherViews.h
 * @brief SparkLauncher drawing: Ember Signal style install, the left rail and the Projects and
 *        New Project pages.
 *
 * LauncherApp owns all launcher state and performs every action (open, spawn, remove, create);
 * this file only draws that state and calls those actions. Splitting the two keeps the
 * inventoried LauncherApp.cpp (template.json reader call site) free of layout code.
 *
 * Thread affinity: UI thread only, on the ImGui context the platform backend created.
 * Ownership: LauncherViewState is a plain value owned by LauncherApp; the views keep no state of
 * their own beyond ImGui's per-window storage.
 */

#pragma once

#include "SparkUI/Tokens.h"

#include <array>
#include <string>

namespace SparkLauncher
{
    class LauncherApp;

    /// @brief Transient page state that only the views read and write (search text, pending confirm).
    struct LauncherViewState
    {
        std::array<char, 256> search{};  ///< Projects page filter (UTF-8), matched against name and path.
        std::string pendingRemovalPath;  ///< Recent-project path awaiting the remove confirmation.
        std::string pendingRemovalName;  ///< Display name shown in the remove confirmation.
        bool openRemovalConfirm = false; ///< Open the confirmation popup on the next draw of the page.
    };

    /**
     * @brief Style the current ImGui context for the launcher and load its fonts.
     *
     * Installs the Ember tokens for @p appearance with the Comfortable metrics profile and a 16 px
     * body size at @p dpiScale, then loads the Spark faces from EditorAssets/Fonts next to the
     * launcher executable (the embedded default font when that directory is missing). Call once,
     * after ImGui::CreateContext() and before the renderer backend creates its device objects.
     */
    void InstallLauncherStyle(float dpiScale, Spark::Ui::Appearance appearance);

    /**
     * @brief Re-theme the launcher for @p appearance after an OS light/dark or High Contrast change.
     *
     * Switches to the same Ember token set InstallLauncherStyle() builds for @p appearance, at the
     * current scale; fonts are not reloaded. Call between frames on the UI thread, after
     * InstallLauncherStyle().
     */
    void SetLauncherAppearance(Spark::Ui::Appearance appearance);

    /// @brief Draw the whole launcher for one frame: rail, status banner and the active page.
    void DrawLauncher(LauncherApp& app);
} // namespace SparkLauncher
