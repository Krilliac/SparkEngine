/**
 * @file FuzzInstallStateProduction.cpp
 * @brief libc++-compiled production adapter for the SparkInstaller marker harness.
 *
 * Every input is written twice, once as <dir>/.sparkengine-install.json for
 * InstallState::Load (the reader Installer::Run, the preflight and both wizards use
 * to detect and describe an existing install) and once as
 * <dir>/.sparkengine-install.pending for InstallState::ReadPendingMarker (the reader
 * a resumed install trusts for its ref and commit). A violation of either reader's
 * contract aborts so libFuzzer records a crash rather than a silent pass:
 *  - a rejected read leaves every output exactly as it was,
 *  - an accepted state has schema 1, and its "schema" member is literally the token 1
 *    (an independent scan, so a reader that truncates 4294967297 to 1 is caught),
 *  - an accepted state survives Save -> Load unchanged (every field
 *    but destination, which Save takes from its argument). The writer escapes '"',
 *    '\\', CR, LF and TAB, so this fixed point catches a reader that accepts what the
 *    writer cannot reproduce or reads back an escape differently,
 *  - an accepted pending marker has a non-empty ref and commit holding no CR, LF or
 *    NUL, and WritePendingMarker -> ReadPendingMarker reproduces both, unless the
 *    pair is longer than the writer's documented 4 KiB framing bound.
 */

#include "FuzzInstallStateProduction.h"

#include "InstallState.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace
{
    namespace fs = std::filesystem;
    using SparkInstaller::InstallState;

    constexpr std::size_t kMaxInputBytes = 64u * 1024u + 1u;
    // "ref=" + '\n' + "commit=" + '\n'; WritePendingMarker's bound is 4096 bytes.
    constexpr std::size_t kMaxPendingValueBytes = 4096u - 13u;
    std::atomic<std::uint64_t> s_directoryCounter{0};

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzInstallState: %s\n", what);
        std::abort();
    }

    /// A fresh directory for one reader; removed with its contents on scope exit.
    class ScopedDirectory
    {
      public:
        ScopedDirectory()
        {
            const auto suffix = s_directoryCounter.fetch_add(1, std::memory_order_relaxed);
            m_path = fs::temp_directory_path() /
                     ("spark-fuzz-install-state-" + std::to_string(getpid()) + "-" + std::to_string(suffix));
            std::error_code error;
            fs::remove_all(m_path, error);
            m_created = fs::create_directory(m_path, error) && !error;
        }
        ~ScopedDirectory()
        {
            std::error_code ignored;
            fs::remove_all(m_path, ignored);
        }
        ScopedDirectory(const ScopedDirectory&) = delete;
        ScopedDirectory& operator=(const ScopedDirectory&) = delete;

        bool Created() const { return m_created; }
        const fs::path& Path() const { return m_path; }

      private:
        fs::path m_path;
        bool m_created = false;
    };

    bool WriteBytes(const fs::path& path, std::string_view bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        return static_cast<bool>(file);
    }

    InstallState MakeSentinel()
    {
        InstallState sentinel;
        sentinel.schema = 77;
        sentinel.ref = "sentinel-ref";
        sentinel.commit = "sentinel-commit";
        sentinel.destination = "sentinel-destination";
        sentinel.generator = "sentinel-generator";
        sentinel.buildType = "sentinel-build-type";
        sentinel.builtAt = "sentinel-built-at";
        sentinel.installerVersion = "sentinel-version";
        sentinel.options["SENTINEL"] = true;
        return sentinel;
    }

    bool SameExceptDestination(const InstallState& left, const InstallState& right)
    {
        return left.schema == right.schema && left.ref == right.ref && left.commit == right.commit &&
               left.generator == right.generator && left.buildType == right.buildType &&
               left.builtAt == right.builtAt && left.installerVersion == right.installerVersion &&
               left.options == right.options;
    }

    bool IsJsonSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    /// Independent of the reader: every "schema" key followed by ':' and a number
    /// must hold exactly the token 1. A reader that range-reduces or truncates the
    /// number (atoi on 4294967297 yields 1 on LP64 glibc) fails this check.
    bool SchemaTokensAreExactlyOne(std::string_view bytes)
    {
        constexpr std::string_view kKey = "\"schema\"";
        for (std::size_t at = bytes.find(kKey); at != std::string_view::npos; at = bytes.find(kKey, at + 1))
        {
            std::size_t cursor = at + kKey.size();
            while (cursor < bytes.size() && IsJsonSpace(bytes[cursor]))
            {
                ++cursor;
            }
            if (cursor >= bytes.size() || bytes[cursor] != ':')
            {
                continue;
            }
            ++cursor;
            while (cursor < bytes.size() && IsJsonSpace(bytes[cursor]))
            {
                ++cursor;
            }
            if (cursor >= bytes.size() || (bytes[cursor] != '-' && (bytes[cursor] < '0' || bytes[cursor] > '9')))
            {
                continue;
            }
            if (bytes[cursor] != '1')
            {
                return false;
            }
            ++cursor;
            while (cursor < bytes.size() && IsJsonSpace(bytes[cursor]))
            {
                ++cursor;
            }
            if (cursor < bytes.size() && bytes[cursor] != ',' && bytes[cursor] != '}')
            {
                return false;
            }
        }
        return true;
    }

    void CheckInstallState(std::string_view bytes)
    {
        ScopedDirectory directory;
        if (!directory.Created() || !WriteBytes(directory.Path() / InstallState::FileName(), bytes))
        {
            return;
        }
        const InstallState sentinel = MakeSentinel();
        InstallState loaded = sentinel;
        if (!InstallState::Load(directory.Path().string(), loaded))
        {
            if (!SameExceptDestination(loaded, sentinel) || loaded.destination != sentinel.destination)
            {
                InvariantFailure("a rejected install state changed the output state");
            }
            return;
        }
        if (loaded.schema != 1 || !SchemaTokensAreExactlyOne(bytes))
        {
            InvariantFailure("an install state whose schema member is not the integer 1 was accepted");
        }

        ScopedDirectory rewritten;
        if (!rewritten.Created())
        {
            return;
        }
        if (!loaded.Save(rewritten.Path().string()))
        {
            InvariantFailure("an accepted install state could not be saved again");
        }
        InstallState reloaded = sentinel;
        if (!InstallState::Load(rewritten.Path().string(), reloaded))
        {
            InvariantFailure("Load rejected the file Save wrote for an accepted install state");
        }
        if (!SameExceptDestination(loaded, reloaded) || reloaded.destination != rewritten.Path().string())
        {
            InvariantFailure("an accepted install state changed across Save -> Load");
        }
    }

    bool IsMarkerValue(const std::string& value)
    {
        return !value.empty() && value.find_first_of(std::string_view("\r\n\0", 3)) == std::string::npos;
    }

    void CheckPendingMarker(std::string_view bytes)
    {
        ScopedDirectory directory;
        if (!directory.Created() || !WriteBytes(directory.Path() / InstallState::PendingFileName(), bytes))
        {
            return;
        }
        std::string ref = "sentinel-ref";
        std::string commit = "sentinel-commit";
        if (!InstallState::ReadPendingMarker(directory.Path().string(), ref, commit))
        {
            if (ref != "sentinel-ref" || commit != "sentinel-commit")
            {
                InvariantFailure("a rejected pending marker changed its outputs");
            }
            return;
        }
        if (!IsMarkerValue(ref) || !IsMarkerValue(commit))
        {
            InvariantFailure("an accepted pending marker holds an empty value or a CR, LF or NUL");
        }

        ScopedDirectory rewritten;
        if (!rewritten.Created())
        {
            return;
        }
        if (!InstallState::WritePendingMarker(rewritten.Path().string(), ref, commit))
        {
            if (ref.size() + commit.size() <= kMaxPendingValueBytes)
            {
                InvariantFailure("an accepted pending marker could not be written again");
            }
            return;
        }
        std::string reloadedRef;
        std::string reloadedCommit;
        if (!InstallState::ReadPendingMarker(rewritten.Path().string(), reloadedRef, reloadedCommit) ||
            reloadedRef != ref || reloadedCommit != commit)
        {
            InvariantFailure("an accepted pending marker changed across Write -> Read");
        }
    }
} // namespace

extern "C" int SparkFuzzLoadInstallState(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string_view bytes =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);
    CheckInstallState(bytes);
    CheckPendingMarker(bytes);
    return 0;
}
