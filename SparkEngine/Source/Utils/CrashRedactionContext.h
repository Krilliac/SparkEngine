/**
 * @file CrashRedactionContext.h
 * @brief OS-derived context construction for crash-text redaction
 */

#pragma once

#include "CrashRedaction.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace Spark::CrashHandlerDetail
{

    /** @brief Convert wide text to UTF-8 with an exactly sized Windows destination. */
    inline std::string WideToUtf8(std::wstring_view input)
    {
#ifdef _WIN32
        if (input.empty() || input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            return {};
        const int sourceLength = static_cast<int>(input.size());
        const int outputLength = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), sourceLength, nullptr,
                                                     0, nullptr, nullptr);
        if (outputLength <= 0)
            return {};
        std::string output(static_cast<size_t>(outputLength), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), sourceLength, output.data(), outputLength,
                                nullptr, nullptr) != outputLength)
        {
            return {};
        }
        return output;
#else
        std::string output;
        for (const wchar_t character : input)
        {
            const std::uint32_t codePoint = static_cast<std::uint32_t>(character);
            if (codePoint < 0x80)
                output.push_back(static_cast<char>(codePoint));
            else if (codePoint < 0x800)
            {
                output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else if (codePoint < 0x10000)
            {
                output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else
            {
                output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
        }
        return output;
#endif
    }

#ifdef _WIN32
    namespace Private
    {
        /// CSIDL rather than a KNOWNFOLDERID: no GUID symbol to link, so this
        /// builds identically under MSVC, clang-cl and MinGW.
        inline std::string ShellFolderPathUtf8(int folderId)
        {
            wchar_t buffer[MAX_PATH] = {};
            if (FAILED(SHGetFolderPathW(nullptr, folderId, nullptr, SHGFP_TYPE_CURRENT, buffer)))
                return {};
            return WideToUtf8(buffer);
        }

        inline std::string CurrentAccountNameUtf8()
        {
            wchar_t buffer[256] = {};
            DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
            if (!GetUserNameW(buffer, &size))
                return {};
            return WideToUtf8(buffer);
        }

        inline std::string CurrentMachineNameUtf8()
        {
            wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
            if (!GetComputerNameW(buffer, &size))
                return {};
            return WideToUtf8(buffer);
        }
    } // namespace Private
#endif

    /**
     * @brief Build the redaction context for the machine this process runs on
     *
     * Roots are sorted longest-first so a nested one (%LOCALAPPDATA%, %TEMP%) is
     * collapsed before the profile directory that contains it.
     *
     * The environment is not a trustworthy source on its own: a service, a
     * session-0 process, or a launcher that sanitizes its child's environment
     * leaves USERPROFILE/USERNAME unset, and the context would come back empty —
     * turning redaction into a silent no-op on the way to a public issue tracker.
     * Every environment-derived value therefore has an OS-derived fallback, and
     * HasRedactionRules() lets the caller refuse transport if even those fail.
     */
    inline CrashRedactionContext MakeCrashRedactionContext()
    {
        const auto readEnvironment = [](const char* name) -> std::string
        {
            const char* value = std::getenv(name);
            return value ? std::string(value) : std::string{};
        };

        CrashRedactionContext context;

        const auto appendRoot = [&context](std::string path, std::string_view token)
        {
            if (path.empty())
                return;
            for (const auto& existing : context.pathTokens)
            {
                if (MatchesIgnoringCaseAndSeparators(existing.first, 0, path) && existing.first.size() == path.size())
                    return;
            }
            context.pathTokens.emplace_back(std::move(path), std::string(token));
        };
#ifdef _WIN32
        const std::array<std::pair<const char*, const char*>, 4> roots = {{
            {"TEMP", "%TEMP%"},
            {"LOCALAPPDATA", "%LOCALAPPDATA%"},
            {"APPDATA", "%APPDATA%"},
            {"USERPROFILE", "%USERPROFILE%"},
        }};
        context.userName = readEnvironment("USERNAME");
        context.machineName = readEnvironment("COMPUTERNAME");
#else
        const std::array<std::pair<const char*, const char*>, 2> roots = {{
            {"XDG_DATA_HOME", "$XDG_DATA_HOME"},
            {"HOME", "$HOME"},
        }};
        context.userName = readEnvironment("USER");
        context.machineName = readEnvironment("HOSTNAME");
#endif
        for (const auto& [variable, token] : roots)
        {
            appendRoot(readEnvironment(variable), token);
        }

        // OS-derived fallbacks for everything the environment failed to supply.
#ifdef _WIN32
        appendRoot(Private::ShellFolderPathUtf8(CSIDL_LOCAL_APPDATA), "%LOCALAPPDATA%");
        appendRoot(Private::ShellFolderPathUtf8(CSIDL_APPDATA), "%APPDATA%");
        appendRoot(Private::ShellFolderPathUtf8(CSIDL_PROFILE), "%USERPROFILE%");
        if (context.userName.empty())
            context.userName = Private::CurrentAccountNameUtf8();
        if (context.machineName.empty())
            context.machineName = Private::CurrentMachineNameUtf8();
#else
        if (const passwd* entry = getpwuid(geteuid()); entry)
        {
            if (entry->pw_dir)
                appendRoot(std::string(entry->pw_dir), "$HOME");
            if (context.userName.empty() && entry->pw_name)
                context.userName = entry->pw_name;
        }
        if (context.machineName.empty())
        {
            char hostName[256] = {};
            if (gethostname(hostName, sizeof(hostName) - 1) == 0)
                context.machineName = hostName;
        }
#endif

        std::stable_sort(context.pathTokens.begin(), context.pathTokens.end(),
                         [](const auto& lhs, const auto& rhs) { return lhs.first.size() > rhs.first.size(); });
        return context;
    }

} // namespace Spark::CrashHandlerDetail
