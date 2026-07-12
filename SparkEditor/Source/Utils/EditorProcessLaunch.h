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
     * The thread handle is closed internally; the process handle is left open for
     * the caller to poll (PollProcessExited) / terminate (TerminateEditorProcess) /
     * eventually CloseHandle.
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
     * @brief Build a `"<engineExe>" -game <dll> [-headless] [-exec <cfg>] [extraArgs]`
     * command line for CreateProcessW.
     *
     * @param dll       Game module DLL path. Converted to its 8.3 short form when it
     *                  contains a space.
     * @param headless  Appends " -headless".
     * @param execCfg   Optional scripted-console-playback file; pass an empty path to
     *                  omit "-exec". Converted to its 8.3 short form when it contains
     *                  a space, same as dll.
     * @param extraArgs Appended verbatim after -exec/-headless (e.g. "-test-seconds
     *                  30"). Must already be space-safe internally (a single space
     *                  between tokens is fine — only PATH arguments need short-form
     *                  conversion, since the engine's -game/-exec parsers split on
     *                  the FIRST space and understand no quoting:
     *                  FindGameModuleFromCmdLine / LoadExecScriptFromCmdLine in
     *                  SparkEngineWindows.cpp).
     * @param outError  Set to a human-readable reason on failure (space-containing
     *                  path with no short form). Left untouched on success.
     * @return The full command line, or an empty string on failure (check outError).
     */
    std::wstring BuildGameLaunchCommandLine(const std::filesystem::path& engineExe, const std::filesystem::path& dll,
                                            bool headless, const std::filesystem::path& execCfg,
                                            const std::wstring& extraArgs, std::string& outError);

} // namespace SparkEditor
