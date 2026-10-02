/**
 * @file CrashHandlerSupport.h
 * @brief Small, deterministic helpers for crash-reporter discovery and archive consent.
 */
#pragma once

#include "CrashArtifactDirectory.h"
#include "CrashRedactionContext.h"

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

    /// An incomplete directory scan must never authorize another queued manifest.
    inline size_t CrashManifestCountOrFull(size_t pendingCount, bool scanSucceeded) noexcept
    {
        return scanSucceeded ? std::min(pendingCount, kMaxPendingCrashManifests) : kMaxPendingCrashManifests;
    }

    /// Fixed-buffer JSON/name formatting for the fatal path: no allocation,
    /// locale, locks or stdio. A failed append invalidates the complete result.
    class SignalArtifactBuffer
    {
      public:
        SignalArtifactBuffer(char* data, size_t capacity) noexcept : m_data(data), m_capacity(capacity) {}
        void Append(std::string_view text) noexcept
        {
            if (!m_ok || !m_data || m_size >= m_capacity || text.size() >= m_capacity - m_size)
            {
                m_ok = false;
                return;
            }
            for (char value : text)
            {
                m_data[m_size++] = value;
            }
            m_data[m_size] = '\0';
        }
        void Number(std::uint64_t value, unsigned base = 10, size_t width = 1) noexcept
        {
            if ((base != 10 && base != 16) || width > 24)
            {
                m_ok = false;
                return;
            }
            constexpr char digits[] = "0123456789abcdef";
            char reversed[24];
            size_t count = 0;
            do
            {
                reversed[count++] = digits[value % base];
                value /= base;
            } while (value != 0);
            while (count < width)
            {
                reversed[count++] = '0';
            }
            while (count != 0)
            {
                Append(std::string_view(&reversed[--count], 1));
            }
        }
        void Quoted(std::string_view text) noexcept
        {
            constexpr char hex[] = "0123456789abcdef";
            Append("\"");
            for (unsigned char value : text)
            {
                if (value == '"' || value == '\\')
                {
                    const char escaped[] = {'\\', static_cast<char>(value)};
                    Append(std::string_view(escaped, sizeof(escaped)));
                }
                else if (value < 0x20)
                {
                    const char escaped[] = {'\\', 'u', '0', '0', hex[value >> 4], hex[value & 15]};
                    Append(std::string_view(escaped, sizeof(escaped)));
                }
                else
                {
                    const char character = static_cast<char>(value);
                    Append(std::string_view(&character, 1));
                }
            }
            Append("\"");
        }
        [[nodiscard]] size_t Size() const noexcept { return m_ok ? m_size : 0; }

      private:
        char* m_data;
        size_t m_capacity;
        size_t m_size = 0;
        bool m_ok = true;
    };

    struct SignalCrashManifest
    {
        std::uint64_t processId = 0;
        std::uint64_t epochSeconds = 0;
        std::string_view logName;
        std::string_view coreHintName;
        std::string_view title;
        bool requireConsent = true;
        bool allowScreenshotRefusal = true;
        bool promptUserDescription = true;
        bool fullMemoryDump = false;
    };

    /// UTC ISO8601 from the crash-time clock, bounded to years 1970..9999.
    /// Returns 0 on overflow; callers must not publish a partial manifest.
    inline size_t FormatSignalCrashManifest(const SignalCrashManifest& manifest, char* output, size_t capacity) noexcept
    {
        if (manifest.epochSeconds > 253402300799ULL || manifest.logName.empty())
        {
            return 0;
        }
        std::uint64_t days = manifest.epochSeconds / 86400;
        unsigned year = 1970;
        const auto leap = [](unsigned value) { return value % 4 == 0 && (value % 100 != 0 || value % 400 == 0); };
        while (days >= (leap(year) ? 366u : 365u))
        {
            days -= leap(year) ? 366u : 365u;
            ++year;
        }
        constexpr unsigned monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        unsigned month = 0;
        while (days >= monthDays[month] + (month == 1 && leap(year) ? 1u : 0u))
        {
            days -= monthDays[month] + (month == 1 && leap(year) ? 1u : 0u);
            ++month;
        }
        SignalArtifactBuffer text(output, capacity);
        text.Append("{\n  \"enginePID\": \"");
        text.Number(manifest.processId);
        text.Append("\",\n  \"timestamp\": \"");
        text.Number(year, 10, 4);
        text.Append("-");
        text.Number(month + 1, 10, 2);
        text.Append("-");
        text.Number(days + 1, 10, 2);
        text.Append("T");
        text.Number(manifest.epochSeconds / 3600 % 24, 10, 2);
        text.Append(":");
        text.Number(manifest.epochSeconds / 60 % 60, 10, 2);
        text.Append(":");
        text.Number(manifest.epochSeconds % 60, 10, 2);
        text.Append("Z\",\n  \"dumpFile\": ");
        text.Quoted(manifest.coreHintName);
        text.Append(",\n  \"logFile\": ");
        text.Quoted(manifest.logName);
        text.Append(",\n  \"screenshotFile\": \"\",\n  \"zipFile\": \"\",\n  \"crashTitle\": ");
        text.Quoted(manifest.title);
        text.Append(",\n  \"requireConsent\": ");
        text.Append(manifest.requireConsent ? "true" : "false");
        text.Append(",\n  \"allowScreenshotRefusal\": ");
        text.Append(manifest.allowScreenshotRefusal ? "true" : "false");
        text.Append(",\n  \"promptUserDescription\": ");
        text.Append(manifest.promptUserDescription ? "true" : "false");
        text.Append(",\n  \"fullMemoryDump\": ");
        text.Append(manifest.fullMemoryDump ? "true" : "false");
        text.Append("\n}\n");
        return text.Size();
    }

    /// One slot remains available for a fatal signal without directory scans
    /// in the handler. Only the owning process writes this private queue.
    inline bool HasNonfatalCrashManifestCapacity(size_t pendingCount, bool reserveFatal) noexcept
    {
        return pendingCount < kMaxPendingCrashManifests - (reserveFatal ? 1u : 0u);
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
