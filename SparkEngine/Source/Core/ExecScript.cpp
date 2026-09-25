/**
 * @file ExecScript.cpp
 * @brief `-exec` script parsing, due-order playback, and the redacted audit trail.
 */
#include "ExecScript.h"

#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>

namespace Spark
{
    namespace
    {
        bool IsDigit(char c)
        {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        }

        /// "t<seconds> <command>": fills @p out and returns true when @p line has that exact shape.
        bool ParseTimedEntry(const std::string& line, ScriptedCommand& out)
        {
            if (line.size() < 2 || line[0] != 't' || !IsDigit(line[1]))
                return false;
            size_t idx = 1;
            while (idx < line.size() && (IsDigit(line[idx]) || line[idx] == '.'))
                ++idx;
            if (idx >= line.size() || line[idx] != ' ')
                return false;
            double seconds = 0.0;
            const char* first = line.data() + 1;
            const char* last = line.data() + idx;
            const auto [end, error] = std::from_chars(first, last, seconds);
            if (error != std::errc{} || end != last)
                return false;
            out.atSec = seconds;
            out.command = line.substr(idx + 1);
            return true;
        }

        /// Optional leading "<frame> " prefix; a line without one (or with an unparseable one) runs at frame 0.
        ScriptedCommand ParseFrameEntry(const std::string& line)
        {
            ScriptedCommand entry;
            size_t idx = 0;
            while (idx < line.size() && IsDigit(line[idx]))
                ++idx;
            int frame = 0;
            if (idx > 0 && idx < line.size() && line[idx] == ' ' &&
                std::from_chars(line.data(), line.data() + idx, frame).ec == std::errc{})
            {
                entry.frame = frame;
                entry.command = line.substr(idx + 1);
            }
            else
            {
                entry.command = line;
            }
            return entry;
        }

        std::filesystem::path PathFromUtf8(const std::string& utf8Path)
        {
            return std::filesystem::path(std::u8string(utf8Path.begin(), utf8Path.end()));
        }
    } // namespace

    std::vector<ScriptedCommand> ParseExecScript(std::istream& input)
    {
        std::vector<ScriptedCommand> commands;
        std::string line;
        while (std::getline(input, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            ScriptedCommand timed;
            commands.push_back(ParseTimedEntry(line, timed) ? std::move(timed) : ParseFrameEntry(line));
        }

        // Unified ordering: t-entries by their time, frame entries at a nominal
        // 60 fps equivalence (scripts should stick to one form per phase).
        const auto sortKey = [](const ScriptedCommand& c) { return c.atSec >= 0.0 ? c.atSec : c.frame / 60.0; };
        std::stable_sort(commands.begin(), commands.end(),
                         [&sortKey](const ScriptedCommand& a, const ScriptedCommand& b)
                         { return sortKey(a) < sortKey(b); });
        return commands;
    }

    bool ExecScriptPlayer::LoadFile(const std::string& utf8Path, SimpleConsole& console)
    {
        std::ifstream file(PathFromUtf8(utf8Path));
        if (!file)
        {
            console.LogError("[exec] cannot open script: " + utf8Path);
            return false;
        }
        Load(ParseExecScript(file));
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
            console.LogInfo(std::format("[exec] frame {} (t={:.1f}s): {}", frameCount, elapsedSeconds, shown));
            const bool ok = console.ExecuteCommand(command);
            AppendAudit(frameCount, elapsedSeconds, ok, shown, console);
            ++m_next;
            ++executed;
        }
        return executed;
    }

    void ExecScriptPlayer::AppendAudit(int frameCount, double elapsedSeconds, bool ok, const std::string& shownCommand,
                                       SimpleConsole& console) const
    {
        // Automated smokes read this trail: the Windows GUI build has no
        // stdout and the file logger does not carry console traffic.
        std::ofstream audit(m_auditPath, std::ios::app);
        if (!audit)
            return;
        audit << "frame " << frameCount << " t=" << std::format("{:.1f}", elapsedSeconds) << "s | "
              << (ok ? "ok " : "ERR") << " | " << shownCommand << '\n';

        // Append the command's console output. The first scripted command dumps
        // the whole boot history (module-loading diagnostics); later ones append
        // just the most recent entries.
        const auto history = console.GetLogHistory();
        const size_t window = (m_next == 0) ? history.size() : 8;
        const size_t start = history.size() > window ? history.size() - window : 0;
        for (size_t i = start; i < history.size(); ++i)
            audit << "    > " << history[i].message << '\n';
    }
} // namespace Spark
