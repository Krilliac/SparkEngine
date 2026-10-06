/**
 * @file ExecScriptParse.cpp
 * @brief `-exec` script parsing (Spark::ParseExecScript).
 *
 * Kept apart from ExecScriptPlayer so the parser links without SimpleConsole:
 * the SEC-120 fuzz target (FuzzerTests/FuzzExecScript.cpp) compiles this file alone.
 */
#include "ExecScript.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <locale>
#include <sstream>

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
            // libc++ 18 (the clang-tidy lane and Clang on Linux/macOS) deletes
            // the floating-point std::from_chars overload. The scanned span is
            // digits and dots only, so a classic-locale, no-skip stream that
            // must consume all of it keeps the strict whole-span contract.
            double seconds = 0.0;
            std::istringstream secondsStream(line.substr(1, idx - 1));
            secondsStream.imbue(std::locale::classic());
            secondsStream >> std::noskipws >> seconds;
            if (secondsStream.fail() || secondsStream.peek() != std::char_traits<char>::eof())
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
} // namespace Spark
