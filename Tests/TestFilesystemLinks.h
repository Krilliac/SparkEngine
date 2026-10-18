// TestFilesystemLinks.h - directory links that every test host can create.
//
// Tests that prove a "never follow a link" guard used to call
// std::filesystem::create_directory_symlink and SKIP when it failed. On
// Windows that call needs Developer Mode or SeCreateSymbolicLinkPrivilege, so
// on the primary platform (and its CI runners) the guards were never run. An
// NTFS junction (a mount-point reparse point) needs no privilege, and it is
// also the link an attacker can actually plant there. std::filesystem reports
// a junction as file_type::junction on MSVC, not as a symlink, so a guard that
// only asks is_symlink() lets it through.
//
// These helpers therefore create a junction on Windows (always, even when a
// symlink would be allowed, so the junction shape is what gets exercised) and
// a directory symlink everywhere else. A failure to create one is a real test
// failure, not a skip.
//
// Thread affinity: any thread. The functions touch only the paths they are given.
#pragma once

#include <filesystem>

namespace SparkTestLinks
{
    /// @brief Create @p link as a directory link to @p target.
    /// @param target Directory the link points at; made absolute before use. It need not exist.
    /// @param link Path of the new link. Must not exist; its parent must.
    /// @return true once @p link exists as a link (NTFS junction on Windows, symlink elsewhere).
    bool MakeDirectoryLink(const std::filesystem::path& target, const std::filesystem::path& link);

    /// @brief True when @p path is itself a link: a symlink, or on Windows any reparse point.
    bool IsDirectoryLink(const std::filesystem::path& path);

    /// @brief Remove the link @p link itself, never the directory it points at.
    /// @return true when @p link was a link and is gone.
    bool RemoveDirectoryLink(const std::filesystem::path& link);
} // namespace SparkTestLinks
