#pragma once

/**
 * @file PathSecurity.h
 * @brief Search-order-safe tool resolution and identity-pinned paths (SparkBuildCore internal).
 *
 * Thread affinity: every function is async-safe; a PathPin is owned by one thread at a time.
 * Ownership: a PathPin owns one OS handle (Windows) or file descriptor (POSIX) and closes it on destruction.
 * Allocation: path strings only; nothing here is on a per-frame path.
 */

#include "Platform.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace SparkBuild::PathSecurity
{
    /**
     * @brief True when @p program is a bare name (no directory separator or drive), so the OS would search for it.
     */
    [[nodiscard]] bool IsBareProgramName(const std::string& program);

    /**
     * @brief Resolve a bare program name to an absolute file using only trusted directories.
     *
     * Windows' implicit CreateProcess search tries the directory of the running
     * executable and then the current directory before the system directories
     * and PATH, so a git.exe or cmake.exe planted beside the installer (or in
     * its working directory) would run instead of the real tool. This search
     * never looks there: it tries the system and Windows directories (Windows
     * only), then each absolute entry of @p searchPath in order. Empty and
     * relative entries (including ".") are skipped, since they name the current
     * directory. On Windows ".exe" is appended when the name has no extension,
     * matching CreateProcess. A candidate must be a regular file (and executable
     * on POSIX).
     * @param program Bare program name; any other shape returns an empty string.
     * @param searchPath PATH-format list (';' on Windows, ':' on POSIX).
     * @return The absolute path, or an empty string when nothing qualifies.
     */
    [[nodiscard]] std::string ResolveExecutableIn(const std::string& program, const std::string& searchPath);

    /// ResolveExecutableIn against the process PATH (POSIX default "/usr/bin:/bin" when PATH is unset).
    [[nodiscard]] std::string ResolveExecutable(const std::string& program);

    /**
     * @brief Holds an opened file or directory so later path-based steps provably use the same object.
     *
     * Windows: the handle is opened without FILE_SHARE_WRITE/FILE_SHARE_DELETE,
     * so while it is held nobody can modify, rename, delete, or replace the
     * entry (a directory pin is opened with write sharing so extraction into it
     * still works). POSIX cannot lock a name, so the descriptor itself is the
     * pin: BoundPath() names the opened object ("/dev/fd/N") and a child process
     * that inherits the descriptor reads exactly the bytes that were verified.
     * Symlinks, junctions and other reparse points are refused when opening.
     */
    class PathPin
    {
      public:
        enum class Kind
        {
            File,
            Directory
        };

        PathPin() = default;
        ~PathPin();
        PathPin(const PathPin&) = delete;
        PathPin& operator=(const PathPin&) = delete;

        /// Open @p path as @p kind. Fails (with @p error) on links, reparse points, or a type mismatch.
        [[nodiscard]] bool Open(const std::filesystem::path& path, Kind kind, std::string& error);

        /// True when @p path still names the pinned object (same volume/device and file id/inode, not a link).
        [[nodiscard]] bool StillNames(const std::filesystem::path& path) const;

        /// A path that resolves to the pinned object itself: the original path on Windows, "/dev/fd/N" on POSIX.
        /// POSIX file pins are rewound to offset 0 first, because /dev/fd may share the descriptor's offset.
        [[nodiscard]] std::filesystem::path BoundPath() const;

        [[nodiscard]] bool IsOpen() const;

#ifndef SPARK_PLATFORM_WINDOWS
        /// The pinned descriptor (close-on-exec); -1 when not open.
        [[nodiscard]] int Descriptor() const { return m_fd; }
#endif

      private:
        void Close();

        std::filesystem::path m_path;
#ifdef SPARK_PLATFORM_WINDOWS
        void* m_handle = nullptr; // HANDLE; kept opaque so this header does not pull in <windows.h>
        uint32_t m_volume = 0;
        uint64_t m_fileIndex = 0;
#else
        int m_fd = -1;
        uint64_t m_device = 0;
        uint64_t m_inode = 0;
#endif
    };
} // namespace SparkBuild::PathSecurity
