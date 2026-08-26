/**
 * @file EditorProcessLaunch.h
 * @brief Shared CreateProcessW helper for editor panels that spawn a separate
 *        SparkEngine.exe instance (out-of-process play testing).
 * @author Spark Engine Team
 * @date 2026
 *
 * Factored out of GameModuleSelectorPanel (W9's "Launch Game" / "Launch Dedicated")
 * so PlayControlPanel (W13) and any future launcher panel reuse one CreateProcessW
 * path instead of duplicating it. GameModuleSelectorPanel itself was updated to call
 * this helper too.
 *
 * Also centralizes the two bits of "exe-lookup" logic every launcher panel needs:
 * the editor's own executable directory, and locating SparkEngine.exe next to it
 * (GetEditorExecutableDirectory / FindEngineExecutable) — previously each panel
 * carried its own private copy of both.
 */

#pragma once

#include <filesystem>
#include <string>

namespace SparkEditor
{

    /// @brief Outcome of a CreateProcessW-based launch attempt. On non-Windows
    /// builds LaunchEditorProcess always returns success=false with an explanatory
    /// error (process launch is a Windows-only feature here).
    struct ProcessLaunchResult
    {
        bool success = false;
        void* processHandle = nullptr; ///< HANDLE, owned by the caller — CloseHandle() once no longer polled.
        unsigned long pid = 0;
        std::string error; ///< Human-readable failure reason (empty on success).
    };

    /**
     * @brief CreateProcessW wrapper shared by every editor panel that spawns a
     * separate SparkEngine.exe instance.
     *
     * @param exePath    Full path to SparkEngine.exe (used as CreateProcessW's
     *                   lpApplicationName so the launch doesn't depend on PATH).
     * @param commandLine The FULL command line including the quoted exe path at the
     *                   front (CreateProcessW convention) — build it with
     *                   BuildGameLaunchCommandLine() below for the common
     *                   "-game <dll>" case.
     * @param workingDir Working directory for the new process (exec_audit.log /
     *                   server.log land here relative to cwd).
     *
     * Console-subsystem children are launched without a visible console window so
     * editor-managed services do not steal focus. Native game/render windows are
     * unaffected. The thread handle is closed internally; the process handle is
     * left open for the caller to poll (PollProcessExited) / terminate
     * (TerminateEditorProcess) / eventually CloseHandle.
     */
    ProcessLaunchResult LaunchEditorProcess(const std::filesystem::path& exePath, const std::wstring& commandLine,
                                            const std::filesystem::path& workingDir);

    /// @brief Non-blocking poll of a handle from LaunchEditorProcess. Returns true
    /// (and sets outExitCode) once the process has exited; false while still running
    /// or if processHandle is null.
    bool PollProcessExited(void* processHandle, unsigned long& outExitCode);

    /// @brief Force-terminate a process launched via LaunchEditorProcess (used by
    /// "Stop All"). Safe to call on an already-exited handle or nullptr (no-op).
    void TerminateEditorProcess(void* processHandle, unsigned int exitCode = 1);

    /**
     * @brief Build a `"<engineExe>" -game <dll> [-headless] [-exec <cfg>]
     * [-manifest <json>] [extraArgs]`
     * command line for CreateProcessW.
     *
     * @param dll       Game module DLL path. Quoted with normal Win32 argv rules, so
     *                  spaces and Unicode are preserved.
     * @param headless  Appends " -headless".
     * @param execCfg   Optional scripted-console-playback file; pass an empty path to
     *                  omit "-exec". Quoted with the same Win32 argv rules as dll.
     * @param manifest  Optional explicit module manifest. Editor project launches
     *                  should pass the active project's spark.modules.json here.
     * @param extraArgs Appended verbatim after -exec/-headless (e.g. "-test-seconds
     *                  30"). Must already be space-safe internally (a single space
     *                  between tokens is fine); path arguments are quoted internally.
     * @param outError  Set to a human-readable reason on failure. Cleared on success.
     * @return The full command line, or an empty string on failure (check outError).
     */
    std::wstring BuildGameLaunchCommandLine(const std::filesystem::path& engineExe, const std::filesystem::path& dll,
                                            bool headless, const std::filesystem::path& execCfg,
                                            const std::filesystem::path& manifest, const std::wstring& extraArgs,
                                            std::string& outError);

    /// @brief Compatibility overload for launchers without an explicit manifest.
    std::wstring BuildGameLaunchCommandLine(const std::filesystem::path& engineExe, const std::filesystem::path& dll,
                                            bool headless, const std::filesystem::path& execCfg,
                                            const std::wstring& extraArgs, std::string& outError);

    /// @brief Directory containing the current (editor) executable. Shared so every
    /// panel that needs its own exe's folder (module scan dir, manifest dir, log
    /// path, engine exe lookup, ...) doesn't carry a private GetModuleFileNameW copy.
    std::string GetEditorExecutableDirectory();

    /**
     * @brief Locate SparkEngine.exe next to the current (editor) executable.
     *
     * Shared "exe-lookup" step used by every launcher panel before building a
     * command line and calling LaunchEditorProcess.
     *
     * @return true with outExePath set on success; false with outError set
     *         (human-readable) if SparkEngine.exe isn't present next to the editor.
     */
    bool FindEngineExecutable(std::filesystem::path& outExePath, std::string& outError);

} // namespace SparkEditor
