// TestFilesystemLinks.cpp - the shared directory-link helper (TestFilesystemLinks.h)
// and its own checks. The checks pin that the helper really produces a junction
// on Windows: a helper that silently fell back to a plain directory would make
// every link-refusal test that uses it pass without testing anything.

#include "TestFilesystemLinks.h"

#include "TestFramework.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace fs = std::filesystem;

namespace SparkTestLinks
{
    namespace
    {
#if defined(_WIN32)
        // Writes a mount-point reparse buffer (the documented REPARSE_DATA_BUFFER
        // MountPointReparseBuffer layout, which user-mode headers do not declare)
        // onto the empty directory @p link.
        bool SetMountPoint(const fs::path& link, const fs::path& absoluteTarget)
        {
            std::wstring printName = absoluteTarget.native();
            // A mount-point target carries no trailing separator unless it is a drive root.
            while (printName.size() > 3 && (printName.back() == L'\\' || printName.back() == L'/'))
                printName.pop_back();
            if (printName.rfind(L"\\\\?\\", 0) == 0)
                printName.erase(0, 4);
            const std::wstring substituteName = L"\\??\\" + printName;

            const std::size_t substituteBytes = substituteName.size() * sizeof(wchar_t);
            const std::size_t printBytes = printName.size() * sizeof(wchar_t);
            // Header: ReparseTag (DWORD), ReparseDataLength, Reserved (USHORT each).
            constexpr std::size_t kHeaderBytes = sizeof(DWORD) + 2 * sizeof(USHORT);
            // Payload: four USHORT offsets/lengths, then both names, each NUL-terminated.
            const std::size_t dataBytes = 4 * sizeof(USHORT) + substituteBytes + printBytes + 2 * sizeof(wchar_t);
            if (dataBytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE - kHeaderBytes)
                return false;

            std::vector<unsigned char> buffer(kHeaderBytes + dataBytes, 0);
            const auto putUShort = [&buffer](std::size_t offset, std::size_t value)
            {
                const USHORT narrow = static_cast<USHORT>(value);
                std::memcpy(buffer.data() + offset, &narrow, sizeof(narrow));
            };
            const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
            std::memcpy(buffer.data(), &tag, sizeof(tag));
            putUShort(4, dataBytes);
            putUShort(8, 0);                                  // SubstituteNameOffset
            putUShort(10, substituteBytes);                   // SubstituteNameLength
            putUShort(12, substituteBytes + sizeof(wchar_t)); // PrintNameOffset
            putUShort(14, printBytes);                        // PrintNameLength
            constexpr std::size_t kPathBufferOffset = kHeaderBytes + 4 * sizeof(USHORT);
            std::memcpy(buffer.data() + kPathBufferOffset, substituteName.data(), substituteBytes);
            std::memcpy(buffer.data() + kPathBufferOffset + substituteBytes + sizeof(wchar_t), printName.data(),
                        printBytes);

            HANDLE handle = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return false;
            DWORD returned = 0;
            const BOOL set = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                                               static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
            ::CloseHandle(handle);
            return set != FALSE;
        }
#endif
    } // namespace

    bool MakeDirectoryLink(const fs::path& target, const fs::path& link)
    {
        std::error_code error;
        const fs::path absoluteTarget = fs::absolute(target, error).lexically_normal();
        if (error || absoluteTarget.empty())
            return false;
#if defined(_WIN32)
        if (!fs::create_directory(link, error) || error)
            return false;
        if (!SetMountPoint(link, absoluteTarget))
        {
            ::RemoveDirectoryW(link.c_str());
            return false;
        }
        return IsDirectoryLink(link);
#else
        fs::create_directory_symlink(absoluteTarget, link, error);
        return !error && IsDirectoryLink(link);
#endif
    }

    bool IsDirectoryLink(const fs::path& path)
    {
#if defined(_WIN32)
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
        std::error_code error;
        return fs::is_symlink(fs::symlink_status(path, error)) && !error;
#endif
    }

    bool RemoveDirectoryLink(const fs::path& link)
    {
        if (!IsDirectoryLink(link))
            return false;
#if defined(_WIN32)
        // RemoveDirectoryW deletes a directory reparse point itself; the target is never
        // entered. A file symlink is a non-directory reparse point and needs DeleteFileW.
        return ::RemoveDirectoryW(link.c_str()) != FALSE || ::DeleteFileW(link.c_str()) != FALSE;
#else
        std::error_code error;
        return fs::remove(link, error) && !error;
#endif
    }
} // namespace SparkTestLinks

namespace
{
    // <temp>/spark_fs_links_<tag>_<nonce>/{target/keep.txt}
    class LinkScratch
    {
      public:
        explicit LinkScratch(const char* tag)
        {
            std::error_code error;
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            m_root =
                fs::temp_directory_path(error) / (std::string("spark_fs_links_") + tag + "_" + std::to_string(nonce));
            fs::create_directories(m_root / "target", error);
            std::ofstream(m_root / "target" / "keep.txt", std::ios::binary) << "keep";
        }
        ~LinkScratch()
        {
            // Links are only created directly under the root: unlink them first so
            // remove_all can never reach through one.
            std::error_code error;
            for (fs::directory_iterator it(m_root, error), end; !error && it != end; it.increment(error))
                SparkTestLinks::RemoveDirectoryLink(it->path());
            error.clear();
            fs::remove_all(m_root, error);
        }
        const fs::path& Root() const { return m_root; }

      private:
        fs::path m_root;
    };

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
} // namespace

TEST(FsLinkHelper_DirectoryLinkResolvesToTargetAndIsALink)
{
    LinkScratch scratch("resolve");
    const fs::path link = scratch.Root() / "link";
    ASSERT_TRUE(SparkTestLinks::MakeDirectoryLink(scratch.Root() / "target", link));

    EXPECT_TRUE(SparkTestLinks::IsDirectoryLink(link));
    EXPECT_FALSE(SparkTestLinks::IsDirectoryLink(scratch.Root() / "target"));
    EXPECT_EQ(ReadAll(link / "keep.txt"), std::string("keep"));

    std::error_code error;
    EXPECT_TRUE(fs::equivalent(link, scratch.Root() / "target", error));
    EXPECT_FALSE(static_cast<bool>(error));

#if defined(_WIN32)
    // It must be a junction, not a symlink: that is the unprivileged link an
    // attacker can plant, and the one an is_symlink()-only guard misses.
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(link.c_str(), &data);
    ASSERT_TRUE(find != INVALID_HANDLE_VALUE);
    ::FindClose(find);
    EXPECT_TRUE((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    EXPECT_TRUE(data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
#if defined(_MSC_VER)
    // MSVC's std::filesystem reports it as file_type::junction, not as a symlink.
    EXPECT_FALSE(fs::is_symlink(fs::symlink_status(link, error)));
#endif
#else
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link, error)));
#endif

    // A second link over an existing name is refused, and leaves the name alone.
    EXPECT_FALSE(SparkTestLinks::MakeDirectoryLink(scratch.Root() / "target", link));
    EXPECT_EQ(ReadAll(link / "keep.txt"), std::string("keep"));
}

TEST(FsLinkHelper_RemoveDirectoryLinkKeepsTarget)
{
    LinkScratch scratch("remove");
    const fs::path link = scratch.Root() / "link";
    ASSERT_TRUE(SparkTestLinks::MakeDirectoryLink(scratch.Root() / "target", link));

    EXPECT_TRUE(SparkTestLinks::RemoveDirectoryLink(link));
    std::error_code error;
    EXPECT_FALSE(fs::exists(fs::symlink_status(link, error)));
    EXPECT_EQ(ReadAll(scratch.Root() / "target" / "keep.txt"), std::string("keep"));

    // A real directory is not a link and is never removed by the helper.
    EXPECT_FALSE(SparkTestLinks::RemoveDirectoryLink(scratch.Root() / "target"));
    EXPECT_TRUE(fs::is_directory(scratch.Root() / "target"));
}
