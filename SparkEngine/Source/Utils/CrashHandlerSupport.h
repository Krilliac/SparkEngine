/**
 * @file CrashHandlerSupport.h
 * @brief Small, deterministic helpers for crash-reporter discovery and archive consent.
 */
#pragma once

#include "CrashArtifactDirectory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
