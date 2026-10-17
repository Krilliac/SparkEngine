#include "ArchiveExtraction.h"
#include "Downloader.h"
#include "Platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <system_error>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace fs = std::filesystem;

namespace SparkBuild::ArchiveExtraction
{
    namespace
    {
        // Structural bounds. They are far above any archive SparkBuild pins
        // (MinGit, CMake) and exist so a hostile directory cannot force huge
        // allocations before the checksum-pinned content is even inspected.
        constexpr uint64_t kMaxZipEntries = 1u << 20;
        constexpr uint64_t kMaxCentralDirectoryBytes = 256ull * 1024 * 1024;
        constexpr size_t kEndOfCentralDirectorySize = 22;
        constexpr size_t kMaxZipCommentSize = 0xFFFF;
        constexpr size_t kCentralHeaderSize = 46;
        constexpr size_t kLocalHeaderSize = 30;

        constexpr uint32_t kLocalHeaderSignature = 0x04034b50;
        constexpr uint32_t kCentralHeaderSignature = 0x02014b50;
        constexpr uint32_t kEndOfCentralDirectorySignature = 0x06054b50;
        constexpr uint32_t kZip64EndSignature = 0x06064b50;
        constexpr uint32_t kZip64LocatorSignature = 0x07064b50;
        constexpr uint16_t kZip64ExtraFieldId = 0x0001;

        std::atomic<uint64_t> g_stagingSequence{0};

        uint16_t ReadU16(const std::vector<unsigned char>& bytes, size_t offset)
        {
            return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
        }

        uint32_t ReadU32(const std::vector<unsigned char>& bytes, size_t offset)
        {
            return static_cast<uint32_t>(ReadU16(bytes, offset)) |
                   (static_cast<uint32_t>(ReadU16(bytes, offset + 2)) << 16);
        }

        uint64_t ReadU64(const std::vector<unsigned char>& bytes, size_t offset)
        {
            return static_cast<uint64_t>(ReadU32(bytes, offset)) |
                   (static_cast<uint64_t>(ReadU32(bytes, offset + 4)) << 32);
        }

        bool ReadAt(std::ifstream& file, uint64_t offset, size_t size, std::vector<unsigned char>& out)
        {
            out.assign(size, 0);
            file.clear();
            file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            if (!file)
                return false;
            file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
            return static_cast<size_t>(file.gcount()) == size;
        }

        uint64_t CurrentProcessId()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            return static_cast<uint64_t>(GetCurrentProcessId());
#else
            return static_cast<uint64_t>(getpid());
#endif
        }

        // Component-wise prefix test on canonical paths; equality counts as inside.
        bool IsWithin(const fs::path& root, const fs::path& candidate)
        {
            auto rootIt = root.begin();
            auto candidateIt = candidate.begin();
            for (; rootIt != root.end(); ++rootIt, ++candidateIt)
            {
                if (candidateIt == candidate.end() || *rootIt != *candidateIt)
                    return false;
            }
            return true;
        }

        // Rename that fails instead of replacing an existing destination. The
        // kernel-level no-replace primitive is used where the platform has one;
        // the check-then-rename fallback only runs on filesystems without it.
        bool RenameNoReplace(const fs::path& from, const fs::path& to)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            // Without MOVEFILE_REPLACE_EXISTING, MoveFileExW refuses an existing target.
            return MoveFileExW(from.c_str(), to.c_str(), 0) != 0;
#else
#if defined(__linux__) && defined(SYS_renameat2)
            constexpr unsigned int kRenameNoReplace = 1u; // RENAME_NOREPLACE
            if (syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), kRenameNoReplace) == 0)
                return true;
            if (errno != EINVAL && errno != ENOSYS)
                return false;
#elif defined(__APPLE__)
            if (renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0)
                return true;
            if (errno != ENOTSUP)
                return false;
#endif
            std::error_code ec;
            if (fs::symlink_status(to, ec).type() != fs::file_type::not_found)
                return false;
            return std::rename(from.c_str(), to.c_str()) == 0;
#endif
        }

        struct ZipDirectoryLocation
        {
            uint64_t entryCount = 0;
            uint64_t offset = 0;
            uint64_t size = 0;
            uint64_t limit = 0; // first byte after the space the directory may occupy
        };

        bool LocateCentralDirectory(std::ifstream& file, uint64_t fileSize, ZipDirectoryLocation& location,
                                    std::string& error)
        {
            if (fileSize < kEndOfCentralDirectorySize)
            {
                error = "archive is too small to be a ZIP file";
                return false;
            }

            const size_t tailSize =
                static_cast<size_t>(std::min<uint64_t>(fileSize, kEndOfCentralDirectorySize + kMaxZipCommentSize));
            std::vector<unsigned char> tail;
            if (!ReadAt(file, fileSize - tailSize, tailSize, tail))
            {
                error = "could not read the ZIP end-of-central-directory region";
                return false;
            }

            // The record must end exactly at EOF (its comment runs to the end),
            // which rejects signatures planted inside a trailing comment.
            size_t recordOffset = tailSize;
            for (size_t candidate = tailSize - kEndOfCentralDirectorySize + 1; candidate-- > 0;)
            {
                if (ReadU32(tail, candidate) == kEndOfCentralDirectorySignature &&
                    ReadU16(tail, candidate + 20) == tailSize - candidate - kEndOfCentralDirectorySize)
                {
                    recordOffset = candidate;
                    break;
                }
            }
            if (recordOffset == tailSize)
            {
                error = "ZIP end-of-central-directory record not found";
                return false;
            }

            const uint64_t recordPosition = fileSize - tailSize + recordOffset;
            const uint16_t diskNumber = ReadU16(tail, recordOffset + 4);
            const uint16_t directoryDisk = ReadU16(tail, recordOffset + 6);
            const uint16_t diskEntries = ReadU16(tail, recordOffset + 8);
            location.entryCount = ReadU16(tail, recordOffset + 10);
            location.size = ReadU32(tail, recordOffset + 12);
            location.offset = ReadU32(tail, recordOffset + 16);
            location.limit = recordPosition;

            const bool needsZip64 = diskNumber == 0xFFFF || directoryDisk == 0xFFFF || diskEntries == 0xFFFF ||
                                    location.entryCount == 0xFFFF || location.size == 0xFFFFFFFF ||
                                    location.offset == 0xFFFFFFFF;
            if (!needsZip64)
            {
                if (diskNumber != 0 || directoryDisk != 0 || diskEntries != location.entryCount)
                {
                    error = "multi-volume ZIP archives are not supported";
                    return false;
                }
                return true;
            }

            std::vector<unsigned char> locator;
            if (recordPosition < 20 || !ReadAt(file, recordPosition - 20, 20, locator) ||
                ReadU32(locator, 0) != kZip64LocatorSignature || ReadU32(locator, 4) != 0 || ReadU32(locator, 16) != 1)
            {
                error = "ZIP64 end-of-central-directory locator is missing or multi-volume";
                return false;
            }
            const uint64_t zip64RecordPosition = ReadU64(locator, 8);
            std::vector<unsigned char> record;
            if (zip64RecordPosition > recordPosition - 20 || recordPosition - 20 - zip64RecordPosition < 56 ||
                !ReadAt(file, zip64RecordPosition, 56, record) || ReadU32(record, 0) != kZip64EndSignature ||
                ReadU32(record, 16) != 0 || ReadU32(record, 20) != 0 || ReadU64(record, 24) != ReadU64(record, 32))
            {
                error = "ZIP64 end-of-central-directory record is malformed or multi-volume";
                return false;
            }
            location.entryCount = ReadU64(record, 32);
            location.size = ReadU64(record, 40);
            location.offset = ReadU64(record, 48);
            location.limit = zip64RecordPosition;
            return true;
        }

        // Resolve a local-header offset stored in the ZIP64 extended-information field.
        bool ResolveZip64LocalOffset(const std::vector<unsigned char>& directory, size_t header, size_t extraStart,
                                     size_t extraLength, uint64_t& localOffset)
        {
            size_t cursor = extraStart;
            const size_t extraEnd = extraStart + extraLength;
            while (extraEnd - cursor >= 4)
            {
                const uint16_t fieldId = ReadU16(directory, cursor);
                const uint16_t fieldSize = ReadU16(directory, cursor + 2);
                cursor += 4;
                if (fieldSize > extraEnd - cursor)
                    return false;
                if (fieldId == kZip64ExtraFieldId)
                {
                    // Fields appear only for the 32-bit values that overflowed, in this order.
                    size_t skip = 0;
                    if (ReadU32(directory, header + 24) == 0xFFFFFFFF)
                        skip += 8;
                    if (ReadU32(directory, header + 20) == 0xFFFFFFFF)
                        skip += 8;
                    if (skip + 8 > fieldSize)
                        return false;
                    localOffset = ReadU64(directory, cursor + skip);
                    return true;
                }
                cursor += fieldSize;
            }
            return false;
        }

        // Lexical half of the symlink policy (see ValidateStagedTree). The
        // link's ancestors inside staging are real directories, so leading
        // '..' components are bounded by its depth; after the first plain name
        // only further plain names may follow, and each of those resolves
        // either to a real child or to another link held to the same rule.
        bool IsContainedLinkTarget(const fs::path& linkParentFromRoot, const fs::path& target)
        {
            if (target.empty() || target.has_root_name() || target.has_root_directory())
                return false;

            size_t depth = 0;
            for (const fs::path& component : linkParentFromRoot)
            {
                if (!component.empty() && component != ".")
                    ++depth;
            }

            bool descending = false;
            for (const fs::path& component : target)
            {
                if (component.empty() || component == ".")
                    continue;
                if (component == "..")
                {
                    if (descending || depth == 0)
                        return false;
                    --depth;
                }
                else
                {
                    descending = true;
                }
            }
            return true;
        }
    } // namespace

    bool IsSafeMemberName(std::string_view name, MemberSyntax syntax)
    {
        if (name.empty() || name.find('\0') != std::string_view::npos)
            return false;
        if (name.front() == '/' || name.front() == '\\')
            return false;
        if (name.size() >= 2 && name[1] == ':')
            return false; // drive-qualified path such as C:foo
        if (syntax == MemberSyntax::Zip && name.find_first_of(":\\") != std::string_view::npos)
            return false;

        size_t start = 0;
        while (start <= name.size())
        {
            const size_t end = name.find_first_of("/\\", start);
            const std::string_view component =
                name.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            if (component == "..")
                return false;
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        return true;
    }

    bool ValidateMemberNames(const std::vector<std::string>& names, MemberSyntax syntax, std::string& error)
    {
        if (names.empty())
        {
            error = "archive lists no members";
            return false;
        }
        for (const std::string& name : names)
        {
            if (!IsSafeMemberName(name, syntax))
            {
                error =
                    "archive member name is absolute, climbs with '..', or uses a forbidden character: '" + name + "'";
                return false;
            }
        }
        return true;
    }

    bool ListZipMembers(const fs::path& archive, std::vector<std::string>& names, std::string& error)
    {
        names.clear();
        std::error_code ec;
        const uint64_t fileSize = fs::file_size(archive, ec);
        std::ifstream file(archive, std::ios::binary);
        if (ec || !file)
        {
            error = "could not open ZIP archive";
            return false;
        }

        ZipDirectoryLocation location;
        if (!LocateCentralDirectory(file, fileSize, location, error))
            return false;
        if (location.entryCount > kMaxZipEntries || location.size > kMaxCentralDirectoryBytes ||
            location.offset > location.limit || location.size > location.limit - location.offset ||
            location.entryCount > location.size / kCentralHeaderSize)
        {
            error = "ZIP central directory bounds are invalid";
            return false;
        }

        std::vector<unsigned char> directory;
        if (!ReadAt(file, location.offset, static_cast<size_t>(location.size), directory))
        {
            error = "could not read the ZIP central directory";
            return false;
        }

        size_t cursor = 0;
        std::vector<unsigned char> localHeader;
        std::vector<unsigned char> localName;
        for (uint64_t entry = 0; entry < location.entryCount; ++entry)
        {
            if (directory.size() - cursor < kCentralHeaderSize || ReadU32(directory, cursor) != kCentralHeaderSignature)
            {
                error = "ZIP central directory entry is truncated or corrupt";
                return false;
            }
            const size_t nameLength = ReadU16(directory, cursor + 28);
            const size_t extraLength = ReadU16(directory, cursor + 30);
            const size_t commentLength = ReadU16(directory, cursor + 32);
            if (directory.size() - cursor - kCentralHeaderSize < nameLength + extraLength + commentLength)
            {
                error = "ZIP central directory entry overruns the directory";
                return false;
            }

            std::string name(reinterpret_cast<const char*>(&directory[cursor + kCentralHeaderSize]), nameLength);
            uint64_t localOffset = ReadU32(directory, cursor + 42);
            if (localOffset == 0xFFFFFFFF &&
                !ResolveZip64LocalOffset(directory, cursor, cursor + kCentralHeaderSize + nameLength, extraLength,
                                         localOffset))
            {
                error = "ZIP64 local header offset is missing for '" + name + "'";
                return false;
            }

            // Extractors differ on which copy of the name they trust, so both
            // must agree before the central-directory name is validated.
            if (localOffset > location.offset || location.offset - localOffset < kLocalHeaderSize + nameLength ||
                !ReadAt(file, localOffset, kLocalHeaderSize, localHeader) ||
                ReadU32(localHeader, 0) != kLocalHeaderSignature || ReadU16(localHeader, 26) != nameLength ||
                !ReadAt(file, localOffset + kLocalHeaderSize, nameLength, localName) ||
                !std::equal(localName.begin(), localName.end(), name.begin(), name.end()))
            {
                error = "ZIP local header does not match the central directory for '" + name + "'";
                return false;
            }

            names.push_back(std::move(name));
            cursor += kCentralHeaderSize + nameLength + extraLength + commentLength;
        }
        if (cursor != directory.size())
        {
            error = "ZIP central directory size does not match its entries";
            return false;
        }
        return true;
    }

    bool CreateStagingDirectory(const fs::path& destDir, fs::path& staging, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(destDir, ec);
        if (ec || !fs::is_directory(destDir, ec))
        {
            error = "could not create the extraction destination '" + destDir.string() + "'";
            return false;
        }

        const std::string prefix = ".sparkbuild-staging-" + std::to_string(CurrentProcessId()) + "-";
        for (size_t attempt = 0; attempt < 128; ++attempt)
        {
            const fs::path candidate =
                destDir / (prefix + std::to_string(g_stagingSequence.fetch_add(1, std::memory_order_relaxed)));
            if (fs::create_directory(candidate, ec))
            {
                staging = candidate;
                return true;
            }
            if (ec)
                break;
        }
        error = "could not create a staging directory inside '" + destDir.string() + "'";
        return false;
    }

    bool VerifyStagedMembers(const fs::path& staging, const std::vector<std::string>& names, std::string& error)
    {
        // Every listed member and each of its ancestor directories, relative to staging.
        std::set<std::string> expected; // generic UTF-8 spellings
        std::error_code ec;
        for (const std::string& name : names)
        {
            fs::path member = fs::u8path(name).lexically_normal();
            if (!member.has_filename())
                member = member.parent_path(); // directory entry with a trailing '/'
            if (member.empty() || member == ".")
                continue;
            if (fs::symlink_status(staging / member, ec).type() == fs::file_type::not_found)
            {
                error = "extraction is incomplete: archive member '" + name + "' is missing";
                return false;
            }
            for (fs::path ancestor = member; !ancestor.empty(); ancestor = ancestor.parent_path())
            {
                if (!expected.insert(ancestor.generic_u8string()).second)
                    break; // this ancestor chain is already recorded
            }
        }

        fs::recursive_directory_iterator it(staging, fs::directory_options::none, ec);
        for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec))
        {
            const fs::path relative = it->path().lexically_relative(staging);
            if (expected.count(relative.generic_u8string()) == 0)
            {
                error = "extraction produced an entry the archive does not list: '" + relative.string() + "'";
                return false;
            }
        }
        if (ec)
        {
            error = "could not walk the staged extraction: " + ec.message();
            return false;
        }
        return true;
    }

    bool ValidateStagedTree(const fs::path& staging, std::string& error)
    {
        std::error_code ec;
        const fs::path root = fs::canonical(staging, ec);
        if (ec)
        {
            error = "could not resolve the staging directory";
            return false;
        }

        size_t entryCount = 0;
        fs::recursive_directory_iterator it(root, fs::directory_options::none, ec);
        for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec))
        {
            ++entryCount;
            const fs::path& entry = it->path();
            const fs::file_type type = it->symlink_status(ec).type();
            if (ec)
                break;
            if (type == fs::file_type::directory || type == fs::file_type::regular)
                continue;
            if (type != fs::file_type::symlink)
            {
                error = "archive produced a special file: '" + entry.string() + "'";
                return false;
            }

            // The lexical rule stops a link that leaves the tree and re-enters
            // through the staging directory's own name (which is gone after
            // commit); the physical resolution below catches dangling or
            // looping links and is kept as a second, independent check.
            std::error_code linkError;
            const fs::path linkText = fs::read_symlink(entry, linkError);
            if (linkError || !IsContainedLinkTarget(entry.parent_path().lexically_relative(root), linkText))
            {
                error = "archive symlink target is absolute or climbs out of the extraction root: '" + entry.string() +
                        "' -> '" + linkText.string() + "'";
                return false;
            }
            const fs::path target = fs::canonical(entry, linkError);
            if (linkError)
            {
                error = "archive symlink is dangling or looping: '" + entry.string() + "'";
                return false;
            }
            if (!IsWithin(root, target))
            {
                error = "archive symlink resolves outside the extraction root: '" + entry.string() + "' -> '" +
                        target.string() + "'";
                return false;
            }
        }
        if (ec)
        {
            error = "could not walk the staged extraction: " + ec.message();
            return false;
        }
        if (entryCount == 0)
        {
            error = "archive extracted no files";
            return false;
        }
        return true;
    }

    bool CommitStagedTree(const fs::path& staging, const fs::path& destDir, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(destDir, ec);
        if (ec || !fs::is_directory(destDir, ec))
        {
            error = "could not create the extraction destination '" + destDir.string() + "'";
            return false;
        }

        std::vector<fs::path> topLevel;
        for (fs::directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec))
            topLevel.push_back(it->path().filename());
        if (ec)
        {
            error = "could not list the staged extraction";
            return false;
        }

        // Refuse up front so a conflicting archive changes nothing at all.
        for (const fs::path& name : topLevel)
        {
            const fs::file_type existing = fs::symlink_status(destDir / name, ec).type();
            if (existing != fs::file_type::not_found)
            {
                error = "extraction would replace existing '" + (destDir / name).string() + "'";
                return false;
            }
        }

        std::vector<fs::path> committed;
        for (const fs::path& name : topLevel)
        {
            if (!RenameNoReplace(staging / name, destDir / name))
            {
                error = "could not move '" + name.string() + "' into '" + destDir.string() + "'";
                for (auto undo = committed.rbegin(); undo != committed.rend(); ++undo)
                {
                    if (!RenameNoReplace(destDir / *undo, staging / *undo))
                        error += "; could not roll back '" + (destDir / *undo).string() + "'";
                }
                return false;
            }
            committed.push_back(name);
        }
        return true;
    }
} // namespace SparkBuild::ArchiveExtraction

namespace SparkBuild
{
    ArchiveFormat Downloader::DetectArchiveFormat(const std::string& archivePath)
    {
        std::array<unsigned char, 4> magic{};
        std::ifstream file(archivePath, std::ios::binary);
        file.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
        const std::streamsize bytesRead = file.gcount();
        if (bytesRead == 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 && magic[3] == 0x04)
            return ArchiveFormat::Zip;
        if (bytesRead >= 2 && magic[0] == 0x1F && magic[1] == 0x8B)
            return ArchiveFormat::GzipTar;
        return ArchiveFormat::Unknown;
    }

    ArchiveFormat Downloader::ArchiveFormatFromName(const std::string& nameOrUrl)
    {
        std::string name = nameOrUrl.substr(0, nameOrUrl.find_first_of("?#"));
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        const auto endsWith = [&name](const std::string& suffix) {
            return name.size() >= suffix.size() &&
                   name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        if (endsWith(".zip"))
            return ArchiveFormat::Zip;
        if (endsWith(".tar.gz") || endsWith(".tgz"))
            return ArchiveFormat::GzipTar;
        return ArchiveFormat::Unknown;
    }
} // namespace SparkBuild
