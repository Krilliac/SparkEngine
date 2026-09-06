/**
 * @file CrashHandlerSupport.h
 * @brief Small, deterministic helpers for crash-reporter discovery and archive consent.
 */
#pragma once

#include "CrashArtifactDirectory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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
    inline constexpr size_t kMaxPendingCrashManifests = 32;

    inline bool HasCrashManifestQueueCapacity(size_t pendingCount)
    {
        return pendingCount < kMaxPendingCrashManifests;
    }

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

    /** @brief A read-only reporter is useful only when the engine will not upload and can show UI. */
    inline bool ShouldLaunchReadOnlyReporter(bool enableCrashReporting, bool headlessMode)
    {
        return !enableCrashReporting && !headlessMode;
    }

    /** @brief Validate the fixed-width lowercase hexadecimal report identifier. */
    inline bool IsCrashReportId(std::string_view reportId)
    {
        if (reportId.size() != 16)
            return false;
        for (const char character : reportId)
        {
            if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
                return false;
        }
        return true;
    }

    /** @brief Build the only filename shape accepted for a ready crash manifest. */
    inline std::string CrashManifestReadyName(std::string_view reportId)
    {
        if (!IsCrashReportId(reportId))
            return {};
        return "crash_manifest_" + std::string(reportId) + ".json";
    }

    /** @brief Check a ready-manifest filename without accepting paths or alternate suffixes. */
    inline bool IsCrashManifestReadyName(std::string_view name)
    {
        constexpr std::string_view prefix = "crash_manifest_";
        constexpr std::string_view suffix = ".json";
        return name.size() == prefix.size() + 16 + suffix.size() && name.starts_with(prefix) &&
               name.ends_with(suffix) && IsCrashReportId(name.substr(prefix.size(), 16));
    }

    /** @brief Construct a native path from UTF-8 without using the Windows locale. */
    inline std::filesystem::path PathFromUtf8(std::string_view path)
    {
#ifdef _WIN32
        return std::filesystem::u8path(path.begin(), path.end());
#else
        return std::filesystem::path(path);
#endif
    }

    /** @brief Convert a native path to UTF-8 without using the Windows locale. */
    inline std::string PathToUtf8(const std::filesystem::path& path)
    {
#ifdef _WIN32
        const std::u8string utf8 = path.u8string();
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#else
        return path.string();
#endif
    }

    /**
     * @brief Permit full-memory capture only when no automatic transport is configured.
     *
     * Endpoint paths can themselves be bearer capabilities even without URL
     * user-info, queries, or fragments. Because a full dump retains the entire
     * crash configuration, keep it local rather than attempting to infer which
     * transport URLs contain reusable authority.
     */
    inline bool CanCaptureFullMemoryDump(bool requested, std::string_view githubToken, std::string_view smtpPassword,
                                         std::string_view uploadURL, std::string_view proxyURL)
    {
        return requested && githubToken.empty() && smtpPassword.empty() && uploadURL.empty() && proxyURL.empty();
    }

    /**
     * @brief Resolve the crash reporter from the trusted executable tree.
     *
     * The caller supplies the directory containing the running engine executable.
     * Only a reporter beside that executable or in its direct `bin` child is
     * accepted. The current working directory is never consulted, and symlinks
     * that resolve outside the trusted tree are rejected.
     */
    inline std::filesystem::path ResolveCrashReporterExecutable(const std::filesystem::path& executableDirectory)
    {
        namespace fs = std::filesystem;

        if (executableDirectory.empty())
            return {};

        std::error_code error;
        const fs::path canonicalDirectory = fs::canonical(executableDirectory, error);
        if (error || !fs::is_directory(canonicalDirectory, error) || error)
            return {};

#ifdef _WIN32
        constexpr auto reporterName = L"SparkCrashReporter.exe";
#else
        constexpr auto reporterName = "SparkCrashReporter";
#endif

        const std::array candidates = {
            canonicalDirectory / reporterName,
            canonicalDirectory / "bin" / reporterName,
        };

        for (const fs::path& candidate : candidates)
        {
            error.clear();
            if (!fs::is_regular_file(candidate, error) || error)
                continue;

            const fs::path canonicalCandidate = fs::canonical(candidate, error);
            if (error)
                continue;

            const fs::path parent = canonicalCandidate.parent_path();
            if (parent == canonicalDirectory || parent == canonicalDirectory / "bin")
                return canonicalCandidate;
        }

        return {};
    }

    /**
     * @brief What a crash report must not carry off the user's machine
     *
     * Populated from the environment, with OS-derived fallbacks, by
     * MakeCrashRedactionContext(); taken as a parameter so the redaction itself
     * is deterministic and testable.
     */
    struct CrashRedactionContext
    {
        /// Absolute directories to collapse, each with the token that replaces it.
        /// Ordered longest-first by MakeCrashRedactionContext so that
        /// %LOCALAPPDATA% wins over the %USERPROFILE% it sits inside.
        std::vector<std::pair<std::string, std::string>> pathTokens;
        std::string userName;    ///< Account name, replaced wherever it appears alone
        std::string machineName; ///< Host name, replaced wherever it appears alone
    };

    /// Names shorter than this are masked only as part of a path token: a one- or
    /// two-character account name occurs inside ordinary words, and substituting
    /// it everywhere would shred the diagnostic text.
    inline constexpr size_t kMinimumMaskableNameLength = 3;

    /**
     * @brief Whether this context can actually remove anything
     *
     * A context with no path roots and no maskable name makes RedactCrashText()
     * an identity function, so the caller would upload the profile path and
     * account name verbatim while believing the text had been redacted. Callers
     * must check this before transport rather than trusting the return value.
     */
    inline bool HasRedactionRules(const CrashRedactionContext& context)
    {
        for (const auto& entry : context.pathTokens)
        {
            if (!entry.first.empty())
                return true;
        }
        return context.userName.size() >= kMinimumMaskableNameLength ||
               context.machineName.size() >= kMinimumMaskableNameLength;
    }

    /** @brief Case-insensitive match of @p needle at @p position, treating '/' and '\\' as equal. */
    inline bool MatchesIgnoringCaseAndSeparators(std::string_view text, size_t position, std::string_view needle)
    {
        if (needle.empty() || position + needle.size() > text.size())
            return false;

        for (size_t i = 0; i < needle.size(); ++i)
        {
            const unsigned char textChar = static_cast<unsigned char>(text[position + i]);
            const unsigned char needleChar = static_cast<unsigned char>(needle[i]);
            const bool bothSeparators =
                (textChar == '\\' || textChar == '/') && (needleChar == '\\' || needleChar == '/');
            if (bothSeparators)
                continue;
            if (std::tolower(textChar) != std::tolower(needleChar))
                return false;
        }
        return true;
    }

    /**
     * @brief Replace every occurrence of @p needle in @p text with @p replacement
     *
     * Case-insensitive and separator-insensitive, so it catches the same path
     * written as C:\Users\name, c:/users/name, or C:\USERS\NAME.
     */
    inline std::string ReplaceAllIgnoringCase(std::string text, std::string_view needle, std::string_view replacement)
    {
        if (needle.empty())
            return text;

        std::string result;
        result.reserve(text.size());
        size_t index = 0;
        while (index < text.size())
        {
            if (MatchesIgnoringCaseAndSeparators(text, index, needle))
            {
                result.append(replacement);
                index += needle.size();
            }
            else
            {
                result.push_back(text[index]);
                ++index;
            }
        }
        return result;
    }

    /**
     * @brief Strip personally identifying data from crash text before transport
     *
     * The Windows crash log embeds the faulting module's full path, which starts
     * with the user's profile directory, and the report is posted to a public
     * GitHub issue. Collapse the known private roots to tokens and mask the bare
     * account and machine names; system paths such as C:\Windows\System32 are
     * kept because they identify nobody and are the diagnostic value of the log.
     *
     * @note Applied to the uploaded copy only — the local artifact on the user's
     *       own disk keeps its full paths.
     */
    inline std::string RedactCrashText(std::string text, const CrashRedactionContext& context)
    {
        for (const auto& [path, token] : context.pathTokens)
        {
            if (!path.empty())
                text = ReplaceAllIgnoringCase(std::move(text), path, token);
        }
        // Longest name first, for the same reason the path roots are: a host
        // name usually embeds the account name (jane -> JANE-DESKTOP), and
        // masking the account first leaves the host name unmatched and half of
        // it still in the uploaded report.
        std::array<std::pair<std::string_view, std::string_view>, 2> names = {{
            {context.userName, "<user>"},
            {context.machineName, "<machine>"},
        }};
        std::stable_sort(names.begin(), names.end(),
                         [](const auto& lhs, const auto& rhs) { return lhs.first.size() > rhs.first.size(); });
        for (const auto& [name, token] : names)
        {
            if (name.size() >= kMinimumMaskableNameLength)
                text = ReplaceAllIgnoringCase(std::move(text), name, token);
        }
        return text;
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

    /** @brief True when a screenshot may be archived without another user choice. */
    inline bool CanPackageScreenshotBeforeConsent(bool captureScreenshot, bool requireConsent, bool headlessMode)
    {
        return captureScreenshot && (headlessMode || !requireConsent);
    }

    /** @brief Build the exact allowlist used to create a crash-report archive. */
    template <typename PathString>
    std::vector<PathString> BuildCrashArchiveAllowlist(const PathString& dumpFile, const PathString& logFile,
                                                       const PathString& screenshotFile, bool includeScreenshot)
    {
        std::vector<PathString> files;
        if (!dumpFile.empty())
            files.push_back(dumpFile);
        if (!logFile.empty())
            files.push_back(logFile);
        if (includeScreenshot && !screenshotFile.empty())
            files.push_back(screenshotFile);
        return files;
    }
} // namespace Spark::CrashHandlerDetail
