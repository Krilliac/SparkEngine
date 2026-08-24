/**
 * @file TFSavePaths.h
 * @brief One process-stable root for every TERRAFRONT persistence file.
 *
 * The root is resolved once on first use. `TF_SAVE_ROOT` may name an absolute
 * directory (recommended for dedicated servers) or a directory relative to
 * the process working directory. When it is unset, all stores use
 * `<working-directory>/Saves`.
 */
#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Terrafront::SavePaths
{
    /**
     * Non-blocking, process-wide ownership guard for one persistence file.
     *
     * The lock is held for the lifetime of an opened store, not merely around
     * the final rename. That deliberately serializes each store's initial read,
     * all in-memory mutations, and every commit, preventing a second authority
     * process from loading a stale snapshot and later replacing newer data.
     * The small `.lock` file is persistent; ownership is the OS handle/lock, so
     * a crashed process releases it automatically without stale-lock cleanup.
     */
    class ExclusiveFileLock
    {
      public:
        ExclusiveFileLock() = default;
        ExclusiveFileLock(const ExclusiveFileLock&) = delete;
        ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;
        ExclusiveFileLock(ExclusiveFileLock&&) = delete;
        ExclusiveFileLock& operator=(ExclusiveFileLock&&) = delete;
        ~ExclusiveFileLock() { Unlock(); }

        bool TryLock(const std::filesystem::path& target, std::error_code& ec) noexcept
        {
            if (IsLocked())
            {
                ec = std::make_error_code(std::errc::device_or_resource_busy);
                return false;
            }
            if (target.empty())
            {
                ec = std::make_error_code(std::errc::invalid_argument);
                return false;
            }

            m_lockPath = target;
            m_lockPath += ".lock";
#ifdef _WIN32
            m_handle = ::CreateFileW(m_lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_handle == INVALID_HANDLE_VALUE)
            {
                ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
                return false;
            }
#else
            m_fd = ::open(m_lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
            if (m_fd < 0)
            {
                ec = std::error_code(errno, std::generic_category());
                return false;
            }
            if (::flock(m_fd, LOCK_EX | LOCK_NB) != 0)
            {
                ec = std::error_code(errno, std::generic_category());
                ::close(m_fd);
                m_fd = -1;
                return false;
            }
#endif
            ec.clear();
            return true;
        }

        void Unlock() noexcept
        {
#ifdef _WIN32
            if (m_handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(m_handle);
                m_handle = INVALID_HANDLE_VALUE;
            }
#else
            if (m_fd >= 0)
            {
                (void)::flock(m_fd, LOCK_UN);
                (void)::close(m_fd);
                m_fd = -1;
            }
#endif
            m_lockPath.clear();
        }

        bool IsLocked() const noexcept
        {
#ifdef _WIN32
            return m_handle != INVALID_HANDLE_VALUE;
#else
            return m_fd >= 0;
#endif
        }

      private:
        std::filesystem::path m_lockPath;
#ifdef _WIN32
        HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
        int m_fd = -1;
#endif
    };

    /** Resolve a configured root against a known working directory. */
    inline std::filesystem::path ResolveRootWide(std::wstring_view configuredRoot,
                                                 const std::filesystem::path& workingDirectory)
    {
        if (configuredRoot.empty())
            return workingDirectory.empty() ? std::filesystem::path{} : (workingDirectory / "Saves").lexically_normal();

        // MinGW's std::filesystem classifies a backslash-form UNC path as
        // relative even on Windows. Preserve UNC/device roots rather than
        // accidentally placing them underneath the process working directory.
        const auto isSeparator = [](wchar_t value) { return value == L'\\' || value == L'/'; };
        const bool hasNetworkOrDeviceRoot =
            configuredRoot.size() >= 2 && isSeparator(configuredRoot[0]) && isSeparator(configuredRoot[1]);

        std::filesystem::path root{configuredRoot};
        if (root.is_relative() && !hasNetworkOrDeviceRoot)
        {
            if (workingDirectory.empty())
                return {};
            root = workingDirectory / root;
        }
#if defined(__MINGW32__)
        // lexically_normal() collapses MinGW's leading UNC separator pair
        // because libstdc++ does not parse it as a root name.
        if (hasNetworkOrDeviceRoot)
            return root;
#endif
        return root.lexically_normal();
    }

    /** UTF-8/ASCII convenience overload; Windows callers should prefer the wide overload. */
    inline std::filesystem::path ResolveRoot(std::string_view configuredRoot,
                                             const std::filesystem::path& workingDirectory)
    {
#ifdef _WIN32
        return ResolveRootWide(std::filesystem::u8path(std::string(configuredRoot)).wstring(), workingDirectory);
#else
        if (configuredRoot.empty())
            return workingDirectory.empty() ? std::filesystem::path{} : (workingDirectory / "Saves").lexically_normal();
        std::filesystem::path root{configuredRoot};
        if (root.is_relative())
        {
            if (workingDirectory.empty())
                return {};
            root = workingDirectory / root;
        }
        return root.lexically_normal();
#endif
    }

    /** Process-stable persistence root shared by account, world, and social stores. */
    inline const std::filesystem::path& Root()
    {
        static const std::filesystem::path root = []
        {
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
#ifdef _WIN32
            const wchar_t* configured = _wgetenv(L"TF_SAVE_ROOT");
            return ResolveRootWide(configured ? std::wstring_view(configured) : std::wstring_view{},
                                   ec ? std::filesystem::path{} : cwd);
#else
            const char* configured = std::getenv("TF_SAVE_ROOT");
            return ResolveRoot(configured ? std::string_view(configured) : std::string_view{},
                               ec ? std::filesystem::path{} : cwd);
#endif
        }();
        return root;
    }

    /**
     * Resolve one store filename under Root(). Only a leaf filename is
     * accepted so an accidentally user-derived value cannot escape the save
     * root. An invalid name returns an empty path and therefore fails closed.
     */
    inline std::filesystem::path File(std::string_view leafName)
    {
        const std::filesystem::path leaf{std::string(leafName)};
        if (leaf.empty() || leaf == "." || leaf == ".." || leaf.is_absolute() || leaf.has_root_name() ||
            leaf.has_parent_path() || leaf.filename() != leaf)
        {
            return {};
        }
        const std::filesystem::path& root = Root();
        return root.empty() ? std::filesystem::path{} : root / leaf;
    }

    /** Stable keys are deliberately filename-safe and never user paths. */
    inline bool IsValidContinentKey(std::string_view key)
    {
        if (key.empty() || key.size() > 64)
            return false;
        for (const unsigned char c : key)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return false;
        }
        return true;
    }

    /** Resolve a continent-qualified JSON store, e.g. terrafront_state.cindral_wastes.json. */
    inline std::filesystem::path ContinentFile(std::string_view stem, std::string_view continentKey)
    {
        if (!IsValidContinentKey(continentKey) || stem.empty())
            return {};
        for (const unsigned char c : stem)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return {};
        }
        return File(std::string(stem) + "." + std::string(continentKey) + ".json");
    }

    /** UTF-8 text for diagnostics only; filesystem calls must continue using path objects. */
    inline std::string Utf8ForLog(const std::filesystem::path& path)
    {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    /** Find a legacy/current corrupt-primary recovery backup beside a store. */
    inline bool FindRecoveryBackup(const std::filesystem::path& storePath, std::filesystem::path& found,
                                   std::error_code& ec)
    {
        found.clear();
        ec.clear();
        std::filesystem::path parent = storePath.parent_path();
        if (parent.empty())
            parent = ".";
        const std::string prefix = Utf8ForLog(storePath.filename()) + ".corrupt-";
        for (std::filesystem::directory_iterator
                 it(parent, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             !ec && it != end; it.increment(ec))
        {
            std::error_code typeEc;
            if (!it->is_regular_file(typeEc))
                continue;
            const std::string name = Utf8ForLog(it->path().filename());
            if (name.starts_with(prefix) && name.ends_with(".bak"))
            {
                found = it->path();
                return true;
            }
        }
        return false;
    }

    /** Replace destination with a completed temporary file without deleting destination first. */
    inline bool AtomicReplace(const std::filesystem::path& temporary, const std::filesystem::path& destination,
                              std::error_code& ec)
    {
#ifdef _WIN32
        if (::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            ec.clear();
            return true;
        }
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return false;
#else
        std::filesystem::rename(temporary, destination, ec);
        return !ec;
#endif
    }

    /** Legacy pre-unified save location beside the executable (migration input only). */
    inline std::filesystem::path LegacyExecutableFile(std::string_view leafName)
    {
        const std::filesystem::path leaf{std::string(leafName)};
        if (leaf.empty() || leaf.has_parent_path() || leaf.filename() != leaf)
            return {};
#ifdef _WIN32
        std::wstring buffer(512, L'\0');
        for (;;)
        {
            const DWORD size = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (size == 0)
                return {};
            if (size < buffer.size() - 1)
            {
                buffer.resize(size);
                return std::filesystem::path(buffer).parent_path() / "Saves" / leaf;
            }
            if (buffer.size() >= 32768)
                return {};
            buffer.resize(buffer.size() * 2);
        }
#else
        return {};
#endif
    }
} // namespace Terrafront::SavePaths
