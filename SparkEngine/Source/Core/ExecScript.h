/**
 * @file ExecScript.h
 * @brief Scripted console playback for automated runs: `-exec <file>`, `-exec-audit <path>`, `-test-seconds N`.
 *
 * Shared by the Windows entry point (windowed and headless loops) and the
 * Linux headless loop so a scripted smoke behaves the same on both platforms.
 *
 * Script format: each non-empty, non-`#` line is `<frame> <console command>`
 * or `t<seconds> <console command>`. Frame entries run once the main loop
 * reaches that frame; t-entries run once that much wall-clock time has
 * elapsed since the loop started. Time entries exist because frame rate
 * varies wildly (vsync, window occlusion) while gameplay runs on real dt, so
 * wall-clock scheduling keeps automated smokes deterministic. Lines without a
 * prefix run at frame 0. When both forms are mixed, ordering assumes 60 fps
 * for the frame entries.
 *
 * Every executed command is appended to an audit file (default
 * `exec_audit.log` in the working directory, which the package smokes read).
 * Arguments of sensitive console commands (credentials such as `tf_login`) are
 * redacted in both the `[exec]` console line and the audit file.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace Spark
{
    class SimpleConsole;

    /// @brief One scheduled console command from an `-exec` script.
    struct ScriptedCommand
    {
        int frame = 0;       ///< Frame at which a frame entry becomes due.
        double atSec = -1.0; ///< >= 0: wall-clock scheduled entry ("t<seconds>" prefix).
        std::string command; ///< Console command line, without the schedule prefix.
    };

    /**
     * @brief Parse an `-exec` script into commands ordered by due time.
     *
     * Accepts LF and CRLF line endings, skips blank and `#` comment lines, and
     * stable-sorts so entries that share a due time keep their file order.
     */
    std::vector<ScriptedCommand> ParseExecScript(std::istream& input);

    /**
     * @brief Plays an `-exec` script against the console from the main loop.
     *
     * One instance per process, owned by the entry point. Not thread-safe:
     * every method is called from the main loop thread.
     */
    class ExecScriptPlayer
    {
      public:
        /// Audit file used when `-exec-audit` is not given; the package smokes read this name.
        static constexpr const char* DefaultAuditFileName = "exec_audit.log";

        /// Largest script LoadFile accepts. Real timelines are a few KiB; the bound keeps a
        /// wrong path (a log, a device, a pipe that never ends) from being read into memory.
        static constexpr size_t MaxScriptBytes = size_t{1} << 20;

        /**
         * @brief Load and schedule a script file (UTF-8 path).
         * @return false (with a console error, schedule left unchanged) when the path is a
         *         directory, cannot be opened or read, or holds more than MaxScriptBytes.
         */
        bool LoadFile(const std::string& utf8Path, SimpleConsole& console);

        /// @brief Replace the schedule with already-parsed commands (restarts playback).
        void Load(std::vector<ScriptedCommand> commands);

        /**
         * @brief `-exec-audit <path>`: direct the audit trail to @p utf8Path instead of `exec_audit.log`.
         *
         * A relative path is resolved against the current directory at call time, so
         * processes launched from one directory can each keep a separate trail.
         */
        void SetAuditPath(const std::string& utf8Path);

        /// @brief `-test-seconds N`: wall-clock limit after which the loop should exit (<= 0 disables).
        void SetTestSecondsLimit(double seconds) { m_testSecondsLimit = seconds > 0.0 ? seconds : 0.0; }
        double GetTestSecondsLimit() const { return m_testSecondsLimit; }

        /**
         * @brief Wall-clock seconds since the first call.
         *
         * The clock starts lazily at the first due-check of the main loop, so
         * boot time is excluded from both t-entries and `-test-seconds`.
         */
        double ElapsedSeconds();

        /// @brief True once a `-test-seconds` limit is set and has elapsed.
        bool TestSecondsLimitReached();

        /// @brief Execute every command due at @p frameCount on the process wall clock.
        size_t RunDue(int frameCount, SimpleConsole& console)
        {
            return RunDueAt(frameCount, ElapsedSeconds(), console);
        }

        /**
         * @brief Execute every command due at @p frameCount / @p elapsedSeconds, in schedule order.
         * @return Number of commands executed by this call.
         */
        size_t RunDueAt(int frameCount, double elapsedSeconds, SimpleConsole& console);

        /// @brief Commands not executed yet.
        size_t GetPendingCount() const { return m_commands.size() - m_next; }

      private:
        void AppendAudit(int frameCount, double elapsedSeconds, bool ok, const std::string& shownCommand,
                         SimpleConsole& console) const;

        std::vector<ScriptedCommand> m_commands;
        size_t m_next = 0;
        std::filesystem::path m_auditPath = DefaultAuditFileName;
        double m_testSecondsLimit = 0.0;
        std::optional<std::chrono::steady_clock::time_point> m_clockStart;
    };
} // namespace Spark
