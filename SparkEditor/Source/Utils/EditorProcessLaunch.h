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
#include <functional>
#include <string>

namespace SparkEditor
{

    /// @brief Outcome of a platform process launch attempt. Handles are opaque:
    /// Win32 uses native HANDLE values while POSIX uses an owned process record.
    struct ProcessLaunchResult
    {
        bool success = false;
        void* processHandle = nullptr; ///< Opaque owned process handle; close with CloseEditorProcessHandles().
        void* jobHandle = nullptr;     ///< Optional owned Job Object handle for process-tree lifetime control.
        unsigned long pid = 0;
        std::string error; ///< Human-readable failure reason (empty on success).
    };

    /**
     * @brief Platform launch wrapper shared by every editor panel that spawns a
     * separate SparkEngine instance.
     *
     * @param exePath    Full path to SparkEngine.exe (used as CreateProcessW's
     *                   lpApplicationName so the launch doesn't depend on PATH).
     * @param commandLine The FULL command line including the executable path at the
     *                   front — build it with
     *                   BuildGameLaunchCommandLine() below for the common
     *                   "-game <dll>" case.
     * @param workingDir Working directory for the new process (exec_audit.log /
     *                   server.log land here relative to cwd).
     *
     * POSIX launches parse this command into argv and call execv directly; shell
     * metacharacters are never evaluated implicitly. Console-subsystem children are
     * launched without a visible console window so
     * editor-managed services do not steal focus. Native game/render windows are
     * unaffected. The thread handle is closed internally; the process handle is
     * left open for the caller to poll (PollProcessExited) / terminate
     * (TerminateEditorProcess) / eventually CloseEditorProcessHandles.
     */
    ProcessLaunchResult LaunchEditorProcess(const std::filesystem::path& exePath, const std::wstring& commandLine,
                                            const std::filesystem::path& workingDir);

    /**
     * @brief Launch an editor-managed process tree.
     *
     * Windows uses a kill-on-close Job Object. POSIX establishes a dedicated
     * process group before exec. Returned opaque handles are owned by the caller.
     */
    ProcessLaunchResult LaunchOwnedEditorProcess(const std::filesystem::path& exePath, const std::wstring& commandLine,
                                                 const std::filesystem::path& workingDir);

    /// @brief Non-blocking poll of a handle from LaunchEditorProcess. Returns true
    /// (and sets outExitCode) once the process has exited; false while still running
    /// or if processHandle is null.
    bool PollProcessExited(void* processHandle, unsigned long& outExitCode);

    /// @brief Force-terminate a process launched via LaunchEditorProcess (used by
    /// "Stop All"). Safe to call on an already-exited handle or nullptr (no-op).
    void TerminateEditorProcess(void* processHandle, unsigned int exitCode = 1);

    enum class EditorProcessStopResult
    {
        NotRunning,
        Graceful,
        Terminated,
        Failed
    };

    /// @brief Close process/job handles without requesting shutdown. Closing a
    /// kill-on-close job also terminates any descendants that outlived the root.
    void CloseEditorProcessHandles(void* processHandle, void* jobHandle);

    /**
     * @brief Request graceful window closure, wait a bounded interval, then
     * terminate the complete owned job tree if the root remains alive.
     * Handles are always closed before returning.
     */
    EditorProcessStopResult StopEditorProcessTree(void* processHandle, void* jobHandle, unsigned long pid,
                                                  unsigned long gracePeriodMs = 1500, unsigned int exitCode = 1);

    /// @brief Injectable operations used by OwnedEditorProcess. Production uses
    /// the platform helpers above; tests can provide deterministic fakes.
    struct EditorProcessOperations
    {
        std::function<bool(void*, unsigned long&)> poll;
        std::function<EditorProcessStopResult(void*, void*, unsigned long, unsigned long)> stopAndClose;
        std::function<void(void*, void*)> close;
    };

    /**
     * @brief Single-process-tree RAII owner for editor-launched children.
     *
     * Adopt() safely stops any previously tracked tree before replacing it.
     * Poll() closes both handles when the root exits (which also cleans up any
     * remaining descendants), and destruction performs a bounded stop.
     */
    class OwnedEditorProcess
    {
      public:
        explicit OwnedEditorProcess(EditorProcessOperations operations = {});
        ~OwnedEditorProcess();

        OwnedEditorProcess(const OwnedEditorProcess&) = delete;
        OwnedEditorProcess& operator=(const OwnedEditorProcess&) = delete;

        bool Adopt(ProcessLaunchResult launch);
        bool Poll(unsigned long& outExitCode);
        EditorProcessStopResult Stop(unsigned long gracePeriodMs = 1500);

        [[nodiscard]] bool IsRunning() const noexcept { return m_processHandle != nullptr; }
        [[nodiscard]] unsigned long GetPid() const noexcept { return m_pid; }

      private:
        void Clear() noexcept;

        EditorProcessOperations m_operations;
        void* m_processHandle = nullptr;
        void* m_jobHandle = nullptr;
        unsigned long m_pid = 0;
    };

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
