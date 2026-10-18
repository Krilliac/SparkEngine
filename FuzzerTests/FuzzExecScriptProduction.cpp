/**
 * @file FuzzExecScriptProduction.cpp
 * @brief libc++-compiled production adapter for the `-exec` script libFuzzer harness.
 *
 * `-exec <file>` schedules every line of a script as a console command, and
 * ExecScriptPlayer::LoadFile hands the (MaxScriptBytes-capped) file text to
 * Spark::ParseExecScript. The adapter wraps the fuzz bytes in the same
 * istringstream and calls the shipped parser directly. RunDueAt compares
 * atSec against the wall clock and frame against the frame counter, and the
 * parser stable-sorts by one key, so the adapter aborts, and libFuzzer
 * records a crash rather than a silent pass, when:
 *  - an entry's atSec is neither the -1 frame-entry sentinel nor a finite
 *    value >= 0, or its frame is negative (a NaN or infinite key would also
 *    break the sort's strict weak ordering),
 *  - a command is empty, contains a line feed, or ends in '\r' or ' ' (the
 *    parser strips those, so a survivor means a line was split or joined),
 *  - the schedule is not ordered by the documented key
 *    (atSec >= 0 ? atSec : frame / 60.0),
 *  - there are more commands than input lines.
 */

#include "FuzzExecScriptProduction.h"

#include "Core/ExecScript.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzExecScript: ParseExecScript violated: %s\n", what);
        std::abort();
    }

    double SortKey(const Spark::ScriptedCommand& command)
    {
        return command.atSec >= 0.0 ? command.atSec : command.frame / 60.0;
    }

    void CheckSchedule(const std::vector<Spark::ScriptedCommand>& commands, const std::string& text)
    {
        const std::size_t lineCount = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
        if (commands.size() > lineCount)
            InvariantFailure("more commands than input lines");

        for (const Spark::ScriptedCommand& command : commands)
        {
            const bool frameEntry = command.atSec == -1.0;
            if (!frameEntry && !(std::isfinite(command.atSec) && command.atSec >= 0.0))
                InvariantFailure("atSec is neither the -1 sentinel nor a finite time >= 0");
            if (command.frame < 0)
                InvariantFailure("negative frame");
            if (command.command.empty())
                InvariantFailure("empty command");
            if (command.command.find('\n') != std::string::npos)
                InvariantFailure("command contains a line feed");
            if (command.command.back() == '\r' || command.command.back() == ' ')
                InvariantFailure("command keeps a trailing CR or space");
        }

        const bool ordered = std::is_sorted(commands.begin(), commands.end(),
                                            [](const Spark::ScriptedCommand& a, const Spark::ScriptedCommand& b)
                                            { return SortKey(a) < SortKey(b); });
        if (!ordered)
            InvariantFailure("schedule is not ordered by due time");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production parser.
extern "C" int SparkFuzzParseExecScript(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    const std::string text = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);
    std::istringstream input(text);
    CheckSchedule(Spark::ParseExecScript(input), text);
    return 0;
}
