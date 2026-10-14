#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace SparkInstaller
{
    // Encoding contract for path strings in the installer core.
    //
    // Every std::string path the installer owns (InstallerContext::destination,
    // the configured build path, InstallState fields, log lines, GitRunner
    // destination arguments) is UTF-8: the TUI and GUI collect UTF-8 text and
    // main() converts the Windows command line to UTF-8. On Windows the narrow
    // std::filesystem::path constructor and path::string() use the ANSI code
    // page instead, so they must never touch these strings; convert with the
    // helpers below.
    //
    // SparkBuild::ProcessRunner launches children with CreateProcessA, so its
    // command lines and working directories are in the ANSI code page. Paths
    // cross that boundary through ProcessArgFromUtf8, which refuses a path the
    // code page cannot represent instead of running a child in the wrong place.
    //
    // Thread affinity: none (pure functions, no shared state, no allocation
    // beyond the returned values).

    // True when @p text is well-formed UTF-8 (no overlongs, surrogates or
    // code points above U+10FFFF).
    [[nodiscard]] bool IsValidUtf8(std::string_view text);

    // UTF-8 text -> filesystem path. Ill-formed input is converted with
    // U+FFFD replacements; callers that accept user input validate it first.
    [[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view utf8);

    // Filesystem path -> UTF-8 text. Never throws for unrepresentable names,
    // unlike path::string() on Windows.
    [[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);

    // UTF-8 text -> the narrow encoding SparkBuild::ProcessRunner passes to the
    // OS (the ANSI code page on Windows; the bytes unchanged elsewhere). On
    // Windows returns false, leaving @p narrow untouched, when the text is not
    // valid UTF-8 or the code page cannot represent it exactly.
    [[nodiscard]] bool ProcessArgFromUtf8(std::string_view utf8, std::string& narrow);

#ifdef _WIN32
    // UTF-16 -> UTF-8, for Windows APIs that report text as wide strings.
    [[nodiscard]] std::string Utf8FromWide(std::wstring_view wide);
#endif
} // namespace SparkInstaller
