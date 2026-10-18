/**
 * @file HeldHandles.h
 * @brief Owned OS handles and handle-based path checks for reads that must not be swapped
 *
 * A path check followed by an open by name is a race: whatever sits at the name when the
 * open happens is what gets read. The mod scanner (ModSystem.cpp) and the VFS file provider
 * (VirtualFileSystem.cpp) therefore open first and decide on the opened handle. These are
 * the shared pieces: a scoped handle/descriptor, and on Windows the final path of a handle
 * and a pinned (undeletable, unrenamable while open) directory open.
 *
 * Thread affinity: none; each object owns one handle and is not shared.
 * Ownership: ScopedHandle / ScopedFd close their handle on destruction; move-only (ScopedFd
 * is not movable).
 * Allocation: FinalPathOf allocates the returned string only.
 */

#pragma once

#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace Spark::HeldHandles
{
#ifdef _WIN32
    /// Owns one Win32 handle.
    class ScopedHandle
    {
      public:
        explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}
        ~ScopedHandle()
        {
            if (IsValid())
            {
                ::CloseHandle(m_handle);
            }
        }
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;
        ScopedHandle(ScopedHandle&& other) noexcept : m_handle(other.m_handle)
        {
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        ScopedHandle& operator=(ScopedHandle&&) = delete;

        [[nodiscard]] HANDLE Get() const { return m_handle; }
        [[nodiscard]] bool IsValid() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

      private:
        HANDLE m_handle;
    };

    /// NT-namespace final path of an open handle, or empty when it cannot be queried.
    inline std::wstring FinalPathOf(HANDLE handle)
    {
        std::wstring buffer(512, L'\0');
        for (;;)
        {
            const DWORD length = ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                             FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
            if (length == 0)
            {
                return {};
            }
            if (length < buffer.size())
            {
                buffer.resize(length);
                return buffer;
            }
            buffer.resize(static_cast<size_t>(length) + 1);
        }
    }

    /// True when @p child names an entry directly inside @p parent (both final paths).
    inline bool IsDirectChildPath(const std::wstring& child, std::wstring parent)
    {
        while (!parent.empty() && parent.back() == L'\\')
        {
            parent.pop_back();
        }
        return !parent.empty() && child.size() > parent.size() + 1 && child.compare(0, parent.size(), parent) == 0 &&
               child[parent.size()] == L'\\' && child.find(L'\\', parent.size() + 1) == std::wstring::npos;
    }

    /// True when @p child names an entry anywhere below @p parent (both final paths).
    inline bool IsDescendantPath(const std::wstring& child, std::wstring parent)
    {
        while (!parent.empty() && parent.back() == L'\\')
        {
            parent.pop_back();
        }
        return !parent.empty() && child.size() > parent.size() + 1 && child.compare(0, parent.size(), parent) == 0 &&
               child[parent.size()] == L'\\';
    }

    /// Opens a directory for listing without following a reparse point in its last component
    /// (unless @p allowReparse) and without FILE_SHARE_DELETE, which pins it: while the
    /// handle is open the directory cannot be renamed, deleted or replaced.
    inline ScopedHandle OpenPinnedDirectory(const std::filesystem::path& path, bool allowReparse)
    {
        const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (allowReparse ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
        ScopedHandle handle(::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, flags, nullptr));
        if (!handle.IsValid())
        {
            return ScopedHandle(INVALID_HANDLE_VALUE);
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(handle.Get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (!allowReparse && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0))
        {
            return ScopedHandle(INVALID_HANDLE_VALUE);
        }
        return handle;
    }
#else
    /// Owns one POSIX file descriptor.
    class ScopedFd
    {
      public:
        explicit ScopedFd(int fd) : m_fd(fd) {}
        ~ScopedFd()
        {
            if (m_fd >= 0)
            {
                ::close(m_fd);
            }
        }
        ScopedFd(const ScopedFd&) = delete;
        ScopedFd& operator=(const ScopedFd&) = delete;
        ScopedFd(ScopedFd&&) = delete;
        ScopedFd& operator=(ScopedFd&&) = delete;

        [[nodiscard]] int Get() const { return m_fd; }

      private:
        int m_fd;
    };
#endif
} // namespace Spark::HeldHandles
