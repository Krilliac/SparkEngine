/**
 * @file ExecScript.cpp
 * @brief `-exec` due-order playback and the redacted audit trail (parsing: ExecScriptParse.cpp).
 */
#include "ExecScript.h"

#include "Utils/SparkConsole.h"

#include <format>
#include <fstream>
#include <sstream>

namespace Spark
{
    namespace
    {
        std::filesystem::path PathFromUtf8(const std::string& utf8Path)
        {
            return std::filesystem::path(std::u8string(utf8Path.begin(), utf8Path.end()));
        }
    } // namespace

    bool ExecScriptPlayer::LoadFile(const std::string& utf8Path, SimpleConsole& console)
    {
        const auto path = PathFromUtf8(utf8Path);
        std::error_code error;
        // A directory opens successfully on POSIX and reads as empty, which would
        // silently drop the whole timeline.
        if (std::filesystem::is_directory(path, error))
        {
            console.LogError("[exec] script path is a directory: " + utf8Path);
            return false;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            console.LogError("[exec] cannot open script: " + utf8Path);
            return false;
        }

        // Read at most one byte past the bound, so an endless source (a pipe or
        // /dev/zero) is refused without being drained.
        std::string text(MaxScriptBytes + 1, '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (file.bad())
        {
            console.LogError("[exec] cannot read script: " + utf8Path);
            return false;
        }
        text.resize(static_cast<size_t>(file.gcount()));
        if (text.size() > MaxScriptBytes)
        {
            console.LogError(
                std::format("[exec] script exceeds the {} byte limit, refusing: {}", MaxScriptBytes, utf8Path));
            return false;
        }

        std::istringstream input(std::move(text));
        Load(ParseExecScript(input));
        console.LogInfo(std::format("[exec] loaded {} scripted commands from {}", m_commands.size(), utf8Path));
        return true;
    }

    void ExecScriptPlayer::SetAuditPath(const std::string& utf8Path)
    {
        // Anchor to the launch directory now: a packaged runtime may change the
        // working directory before the first scripted command runs.
        std::error_code error;
        const auto requested = PathFromUtf8(utf8Path);
        const auto absolute = std::filesystem::absolute(requested, error);
        m_auditPath = error ? requested : absolute;
    }

    void ExecScriptPlayer::Load(std::vector<ScriptedCommand> commands)
    {
        m_commands = std::move(commands);
        m_next = 0;
    }

    double ExecScriptPlayer::ElapsedSeconds()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!m_clockStart)
            m_clockStart = now;
        return std::chrono::duration<double>(now - *m_clockStart).count();
    }

    bool ExecScriptPlayer::TestSecondsLimitReached()
    {
        return m_testSecondsLimit > 0.0 && ElapsedSeconds() >= m_testSecondsLimit;
    }

    size_t ExecScriptPlayer::RunDueAt(int frameCount, double elapsedSeconds, SimpleConsole& console)
    {
        const auto isDue = [&](const ScriptedCommand& sc)
        { return sc.atSec >= 0.0 ? elapsedSeconds >= sc.atSec : sc.frame <= frameCount; };

        size_t executed = 0;
        while (m_next < m_commands.size() && isDue(m_commands[m_next]))
        {
            const std::string& command = m_commands[m_next].command;
            // Credentials must never reach the console history or the audit file.
            const std::string shown = console.RedactSensitiveArguments(command);
            // The schedule index distinguishes repeated commands caught up in
            // one frame, including equal due times and rounded elapsed times.
            const std::string marker =
                std::format("[exec] frame {} (t={:.1f}s, entry={}): {}", frameCount, elapsedSeconds, m_next, shown);
            console.LogInfo(marker);
            const bool ok = console.ExecuteCommand(command);
            AppendAudit(frameCount, elapsedSeconds, ok, shown, marker, console);
            ++m_next;
            ++executed;
        }
        return executed;
    }

    void ExecScriptPlayer::AppendAudit(int frameCount, double elapsedSeconds, bool ok, const std::string& shownCommand,
                                       const std::string& marker, SimpleConsole& console) const
    {
        // Automated smokes read this trail: the Windows GUI build has no
        // stdout and the file logger does not carry console traffic. Binary mode
        // keeps the line format byte-identical (LF) on every platform; text mode
        // on Windows would emit CRLF and break exact-match consumers.
        std::ofstream audit(m_auditPath, std::ios::app | std::ios::binary);
        if (!audit)
            return;
        audit << "frame " << frameCount << " t=" << std::format("{:.1f}", elapsedSeconds) << "s | "
              << (ok ? "ok " : "ERR") << " | " << shownCommand << '\n';

        // Append the command's console output. The first scripted command dumps
        // the whole boot history (module-loading diagnostics); later ones append
        // everything from their own marker on (entry=N makes it unique), however
        // many lines the command printed. A marker the capped history already
        // evicted leaves the whole history, and the consumer's marker check fails.
        const auto history = console.GetLogHistory();
        size_t start = 0;
        if (m_next != 0)
        {
            for (size_t i = history.size(); i > 0; --i)
            {
                if (history[i - 1].message == marker)
                {
                    start = i - 1;
                    break;
                }
            }
        }
        for (size_t i = start; i < history.size(); ++i)
            audit << "    > " << history[i].message << '\n';
    }
} // namespace Spark
