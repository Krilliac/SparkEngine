/**
 * @file CrashReporterApp.cpp
 * @brief Out-of-process crash reporter implementation
 */

#include "CrashReporterApp.h"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace SparkCrashReporter
{

    // ============================================================================
    // JSON helpers (bounded, strict, and dependency-free)
    // ============================================================================

    namespace
    {
        constexpr size_t kMaxManifestBytes = 1024 * 1024;
        constexpr size_t kMaxJsonStringBytes = 256 * 1024;
        constexpr size_t kMaxJsonDepth = 16;
        constexpr size_t kMaxCollectionEntries = 4096;
        constexpr size_t kMaxReadyManifests = 32;

        void SecureWipeString(std::string& value) noexcept
        {
            if (value.empty())
                return;
            volatile char* bytes = value.data();
            for (size_t index = 0; index < value.size(); ++index)
                bytes[index] = 0;
            value.clear();
        }

        class ScopedStringWiper
        {
          public:
            explicit ScopedStringWiper(std::string& value) : m_value(value) {}
            ScopedStringWiper(const ScopedStringWiper&) = delete;
            ScopedStringWiper& operator=(const ScopedStringWiper&) = delete;
            ~ScopedStringWiper() { SecureWipeString(m_value); }

          private:
            std::string& m_value;
        };

        void SecureWipeTransportConfiguration(CrashManifest& manifest) noexcept
        {
            SecureWipeString(manifest.uploadURL);
            SecureWipeString(manifest.proxyURL);
            SecureWipeString(manifest.githubRepo);
            SecureWipeString(manifest.githubToken);
            SecureWipeString(manifest.githubLabels);
            SecureWipeString(manifest.smtpUser);
            SecureWipeString(manifest.smtpPass);
            SecureWipeString(manifest.emailTo);
            SecureWipeString(manifest.emailFrom);
            manifest.timeoutSeconds = 5;
        }

        class ScopedManifestCredentialWiper
        {
          public:
            explicit ScopedManifestCredentialWiper(CrashManifest& manifest) : m_manifest(manifest) {}
            ScopedManifestCredentialWiper(const ScopedManifestCredentialWiper&) = delete;
            ScopedManifestCredentialWiper& operator=(const ScopedManifestCredentialWiper&) = delete;
            ~ScopedManifestCredentialWiper() { SecureWipeTransportConfiguration(m_manifest); }

          private:
            CrashManifest& m_manifest;
        };

        class ManifestJsonReader
        {
          public:
            explicit ManifestJsonReader(std::string_view json) : m_json(json) {}

            bool Parse(CrashManifest& manifest)
            {
                SkipWhitespace();
                if (!Consume('{'))
                    return false;

                SkipWhitespace();
                if (Consume('}'))
                    return Finish();

                size_t entryCount = 0;
                while (entryCount++ < kMaxCollectionEntries)
                {
                    std::string key;
                    if (!ParseString(key) || !m_seenKeys.insert(key).second)
                        return false;

                    SkipWhitespace();
                    if (!Consume(':'))
                        return false;
                    SkipWhitespace();

                    if (!ParseManifestMember(key, manifest))
                        return false;

                    SkipWhitespace();
                    if (Consume('}'))
                        return Finish();
                    if (!Consume(','))
                        return false;
                    SkipWhitespace();
                }
                return false;
            }

          private:
            bool Finish()
            {
                SkipWhitespace();
                return m_position == m_json.size();
            }

            void SkipWhitespace()
            {
                while (m_position < m_json.size())
                {
                    const char c = m_json[m_position];
                    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                        break;
                    ++m_position;
                }
            }

            bool Consume(char expected)
            {
                if (m_position >= m_json.size() || m_json[m_position] != expected)
                    return false;
                ++m_position;
                return true;
            }

            static int HexValue(char c)
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            }

            bool ParseHex4(uint32_t& value)
            {
                if (m_json.size() - m_position < 4)
                    return false;
                value = 0;
                for (int index = 0; index < 4; ++index)
                {
                    const int digit = HexValue(m_json[m_position++]);
                    if (digit < 0)
                        return false;
                    value = (value << 4) | static_cast<uint32_t>(digit);
                }
                return true;
            }

            static void AppendUtf8(uint32_t codePoint, std::string& output)
            {
                if (codePoint <= 0x7F)
                {
                    output.push_back(static_cast<char>(codePoint));
                }
                else if (codePoint <= 0x7FF)
                {
                    output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
                else if (codePoint <= 0xFFFF)
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

            bool ParseString(std::string& output)
            {
                if (!Consume('"'))
                    return false;

                output.clear();
                while (m_position < m_json.size())
                {
                    const unsigned char c = static_cast<unsigned char>(m_json[m_position++]);
                    if (c == '"')
                        return output.size() <= kMaxJsonStringBytes;
                    if (c < 0x20)
                        return false;

                    if (c != '\\')
                    {
                        output.push_back(static_cast<char>(c));
                    }
                    else
                    {
                        if (m_position >= m_json.size())
                            return false;
                        const char escape = m_json[m_position++];
                        switch (escape)
                        {
                        case '"':
                        case '\\':
                        case '/':
                            output.push_back(escape);
                            break;
                        case 'b':
                            output.push_back('\b');
                            break;
                        case 'f':
                            output.push_back('\f');
                            break;
                        case 'n':
                            output.push_back('\n');
                            break;
                        case 'r':
                            output.push_back('\r');
                            break;
                        case 't':
                            output.push_back('\t');
                            break;
                        case 'u':
                        {
                            uint32_t codePoint = 0;
                            if (!ParseHex4(codePoint))
                                return false;
                            if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
                            {
                                if (m_json.size() - m_position < 6 || m_json[m_position] != '\\' ||
                                    m_json[m_position + 1] != 'u')
                                    return false;
                                m_position += 2;
                                uint32_t low = 0;
                                if (!ParseHex4(low) || low < 0xDC00 || low > 0xDFFF)
                                    return false;
                                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                            }
                            else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
                            {
                                return false;
                            }
                            AppendUtf8(codePoint, output);
                            break;
                        }
                        default:
                            return false;
                        }
                    }

                    if (output.size() > kMaxJsonStringBytes)
                        return false;
                }
                return false;
            }

            bool ParseBoolean(bool& output)
            {
                if (m_json.substr(m_position, 4) == "true")
                {
                    m_position += 4;
                    output = true;
                    return true;
                }
                if (m_json.substr(m_position, 5) == "false")
                {
                    m_position += 5;
                    output = false;
                    return true;
                }
                return false;
            }

            bool ParseInteger(int& output)
            {
                const size_t start = m_position;
                if (m_position < m_json.size() && m_json[m_position] == '-')
                    ++m_position;
                if (m_position >= m_json.size())
                    return false;
                if (m_json[m_position] == '0')
                {
                    ++m_position;
                    if (m_position < m_json.size() && m_json[m_position] >= '0' && m_json[m_position] <= '9')
                        return false;
                }
                else
                {
                    if (m_json[m_position] < '1' || m_json[m_position] > '9')
                        return false;
                    while (m_position < m_json.size() && m_json[m_position] >= '0' && m_json[m_position] <= '9')
                        ++m_position;
                }

                int64_t parsed = 0;
                const char* begin = m_json.data() + start;
                const char* end = m_json.data() + m_position;
                const auto result = std::from_chars(begin, end, parsed);
                if (result.ec != std::errc{} || result.ptr != end || parsed < std::numeric_limits<int>::min() ||
                    parsed > std::numeric_limits<int>::max())
                    return false;
                output = static_cast<int>(parsed);
                return true;
            }

            bool SkipNumber()
            {
                if (m_position < m_json.size() && m_json[m_position] == '-')
                    ++m_position;
                if (m_position >= m_json.size())
                    return false;
                if (m_json[m_position] == '0')
                {
                    ++m_position;
                }
                else
                {
                    if (m_json[m_position] < '1' || m_json[m_position] > '9')
                        return false;
                    while (m_position < m_json.size() && m_json[m_position] >= '0' && m_json[m_position] <= '9')
                        ++m_position;
                }
                if (m_position < m_json.size() && m_json[m_position] == '.')
                {
                    ++m_position;
                    const size_t fractionStart = m_position;
                    while (m_position < m_json.size() && m_json[m_position] >= '0' && m_json[m_position] <= '9')
                        ++m_position;
                    if (fractionStart == m_position)
                        return false;
                }
                if (m_position < m_json.size() && (m_json[m_position] == 'e' || m_json[m_position] == 'E'))
                {
                    ++m_position;
                    if (m_position < m_json.size() && (m_json[m_position] == '+' || m_json[m_position] == '-'))
                        ++m_position;
                    const size_t exponentStart = m_position;
                    while (m_position < m_json.size() && m_json[m_position] >= '0' && m_json[m_position] <= '9')
                        ++m_position;
                    if (exponentStart == m_position)
                        return false;
                }
                return true;
            }

            bool SkipValue(size_t depth)
            {
                if (depth > kMaxJsonDepth || m_position >= m_json.size())
                    return false;

                if (m_json[m_position] == '"')
                {
                    std::string ignored;
                    return ParseString(ignored);
                }
                if (m_json[m_position] == '{')
                {
                    ++m_position;
                    SkipWhitespace();
                    if (Consume('}'))
                        return true;
                    size_t entries = 0;
                    while (entries++ < kMaxCollectionEntries)
                    {
                        std::string ignoredKey;
                        if (!ParseString(ignoredKey))
                            return false;
                        SkipWhitespace();
                        if (!Consume(':'))
                            return false;
                        SkipWhitespace();
                        if (!SkipValue(depth + 1))
                            return false;
                        SkipWhitespace();
                        if (Consume('}'))
                            return true;
                        if (!Consume(','))
                            return false;
                        SkipWhitespace();
                    }
                    return false;
                }
                if (m_json[m_position] == '[')
                {
                    ++m_position;
                    SkipWhitespace();
                    if (Consume(']'))
                        return true;
                    size_t entries = 0;
                    while (entries++ < kMaxCollectionEntries)
                    {
                        if (!SkipValue(depth + 1))
                            return false;
                        SkipWhitespace();
                        if (Consume(']'))
                            return true;
                        if (!Consume(','))
                            return false;
                        SkipWhitespace();
                    }
                    return false;
                }
                if (m_json.substr(m_position, 4) == "true" || m_json.substr(m_position, 4) == "null")
                {
                    m_position += 4;
                    return true;
                }
                if (m_json.substr(m_position, 5) == "false")
                {
                    m_position += 5;
                    return true;
                }
                return SkipNumber();
            }

            bool ParseManifestMember(const std::string& key, CrashManifest& manifest)
            {
                if (key == "enginePID")
                    return ParseString(manifest.enginePID);
                if (key == "timestamp")
                    return ParseString(manifest.timestamp);
                if (key == "dumpFile")
                    return ParseString(manifest.dumpFile);
                if (key == "logFile")
                    return ParseString(manifest.logFile);
                if (key == "screenshotFile")
                    return ParseString(manifest.screenshotFile);
                if (key == "zipFile")
                    return ParseString(manifest.zipFile);
                if (key == "crashTitle")
                    return ParseString(manifest.crashTitle);
                if (key == "uploadURL")
                    return ParseDiscardedString();
                if (key == "proxyURL")
                    return ParseDiscardedString();
                if (key == "githubRepo")
                    return ParseDiscardedString();
                if (key == "githubToken")
                    return ParseDiscardedString();
                if (key == "githubLabels")
                    return ParseDiscardedString();
                if (key == "smtpUser")
                    return ParseDiscardedString();
                if (key == "smtpPass")
                    return ParseDiscardedString();
                if (key == "emailTo")
                    return ParseDiscardedString();
                if (key == "emailFrom")
                    return ParseDiscardedString();
                if (key == "requireConsent")
                    return ParseBoolean(manifest.requireConsent);
                if (key == "allowScreenshotRefusal")
                    return ParseBoolean(manifest.allowScreenshotRefusal);
                if (key == "promptUserDescription")
                    return ParseBoolean(manifest.promptUserDescription);
                if (key == "fullMemoryDump")
                    return ParseBoolean(manifest.fullMemoryDump);
                if (key == "timeoutSeconds")
                {
                    int ignored = 0;
                    return ParseInteger(ignored);
                }
                return SkipValue(0);
            }

            bool ParseDiscardedString()
            {
                std::string legacyValue;
                ScopedStringWiper wipeLegacyValue(legacyValue);
                legacyValue.reserve(m_json.size() - m_position);
                return ParseString(legacyValue);
            }

            std::string_view m_json;
            size_t m_position = 0;
            std::unordered_set<std::string> m_seenKeys;
        };

        std::filesystem::path PathFromUtf8(std::string_view path)
        {
#ifdef _WIN32
            return std::filesystem::u8path(path.begin(), path.end());
#else
            return std::filesystem::path(path);
#endif
        }

        std::string PathToUtf8(const std::filesystem::path& path)
        {
#ifdef _WIN32
            const std::u8string utf8 = path.u8string();
            return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#else
            return path.string();
#endif
        }

        bool SameIdentity(const ArtifactIdentity& lhs, const ArtifactIdentity& rhs)
        {
            return lhs.valid && rhs.valid && lhs.device == rhs.device && lhs.file == rhs.file;
        }

        class ScopedNativeHandle
        {
          public:
#ifdef _WIN32
            using Value = HANDLE;
            static Value Invalid() noexcept { return INVALID_HANDLE_VALUE; }
#else
            using Value = int;
            static constexpr Value Invalid() noexcept { return -1; }
#endif

            ScopedNativeHandle() = default;
            explicit ScopedNativeHandle(Value value) : m_value(value) {}
            ScopedNativeHandle(const ScopedNativeHandle&) = delete;
            ScopedNativeHandle& operator=(const ScopedNativeHandle&) = delete;
            ScopedNativeHandle(ScopedNativeHandle&& other) noexcept : m_value(other.Release()) {}
            ScopedNativeHandle& operator=(ScopedNativeHandle&& other) noexcept
            {
                if (this != &other)
                {
                    Reset();
                    m_value = other.Release();
                }
                return *this;
            }
            ~ScopedNativeHandle() { Reset(); }

            explicit operator bool() const { return m_value != Invalid(); }
            Value Get() const { return m_value; }

          private:
            Value Release()
            {
                const Value value = m_value;
                m_value = Invalid();
                return value;
            }
            void Reset()
            {
                if (m_value == Invalid())
                    return;
#ifdef _WIN32
                CloseHandle(m_value);
#else
                close(m_value);
#endif
                m_value = Invalid();
            }

            Value m_value = Invalid();
        };

        struct PinnedDirectory
        {
            ScopedNativeHandle handle;
            std::filesystem::path path;
            ArtifactIdentity identity;
        };

        bool GetHandleIdentity(ScopedNativeHandle::Value handle, bool requireRegularFile, ArtifactIdentity& identity,
                               std::uint64_t* size = nullptr)
        {
#ifdef _WIN32
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                return false;
            }
            const bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (requireRegularFile && (directory || info.nNumberOfLinks != 1))
                return false;
            if (!requireRegularFile && !directory)
                return false;
            identity.device = info.dwVolumeSerialNumber;
            identity.file = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
            identity.valid = true;
            if (size)
                *size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
            struct stat info
            {
            };
            if (fstat(handle, &info) != 0)
                return false;
            if (requireRegularFile && (!S_ISREG(info.st_mode) || info.st_nlink != 1))
                return false;
            if (!requireRegularFile && !S_ISDIR(info.st_mode))
                return false;
            identity.device = static_cast<std::uint64_t>(info.st_dev);
            identity.file = static_cast<std::uint64_t>(info.st_ino);
            identity.valid = true;
            if (size)
                *size = static_cast<std::uint64_t>(info.st_size);
#endif
            return true;
        }

        bool OpenPinnedDirectory(const std::filesystem::path& root, PinnedDirectory& output)
        {
            std::error_code error;
            const std::filesystem::path absoluteRoot = std::filesystem::absolute(root, error).lexically_normal();
            if (error || absoluteRoot.empty())
                return false;

#ifdef _WIN32
            ScopedNativeHandle handle(CreateFileW(absoluteRoot.c_str(), FILE_READ_ATTRIBUTES,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
#else
            int flags = O_RDONLY;
#ifdef O_DIRECTORY
            flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            ScopedNativeHandle handle(open(absoluteRoot.c_str(), flags));
#endif
            ArtifactIdentity identity;
            if (!handle || !GetHandleIdentity(handle.Get(), false, identity))
                return false;

            output.handle = std::move(handle);
            output.path = absoluteRoot;
            output.identity = identity;
            return true;
        }

        bool ArtifactNameInRoot(const PinnedDirectory& root, std::string_view artifactPath, std::filesystem::path& name)
        {
            namespace fs = std::filesystem;
            if (artifactPath.empty())
                return false;

            const fs::path candidate = PathFromUtf8(artifactPath).lexically_normal();
            if (candidate.is_absolute())
            {
                if (candidate.parent_path() != root.path)
                    return false;
                name = candidate.filename();
            }
            else
            {
                if (candidate.has_parent_path())
                    return false;
                name = candidate;
            }
            return !name.empty() && name != "." && name != "..";
        }

        bool OpenArtifact(const PinnedDirectory& root, const std::filesystem::path& name, ScopedNativeHandle& output,
                          ArtifactIdentity& identity, std::uint64_t* size = nullptr, bool requestDelete = false)
        {
#ifdef _WIN32
            const DWORD access = GENERIC_READ | (requestDelete ? DELETE : 0);
            const DWORD sharing = FILE_SHARE_READ | (requestDelete ? FILE_SHARE_DELETE : 0);
            ScopedNativeHandle handle(CreateFileW((root.path / name).c_str(), access, sharing, nullptr, OPEN_EXISTING,
                                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
#else
            (void)requestDelete;
            int flags = O_RDONLY;
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            ScopedNativeHandle handle(openat(root.handle.Get(), name.c_str(), flags));
#endif
            if (!handle || !GetHandleIdentity(handle.Get(), true, identity, size))
                return false;
            output = std::move(handle);
            return true;
        }

        bool RemoveOpenedArtifact(const PinnedDirectory& root, const std::filesystem::path& name,
                                  ScopedNativeHandle::Value handle, const ArtifactIdentity& openedIdentity)
        {
#ifdef _WIN32
            (void)root;
            (void)name;
            (void)openedIdentity;
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
#else
            (void)handle;
            struct stat namedInfo
            {
            };
            if (fstatat(root.handle.Get(), name.c_str(), &namedInfo, AT_SYMLINK_NOFOLLOW) != 0)
                return false;
            ArtifactIdentity namedIdentity;
            namedIdentity.device = static_cast<std::uint64_t>(namedInfo.st_dev);
            namedIdentity.file = static_cast<std::uint64_t>(namedInfo.st_ino);
            namedIdentity.valid = S_ISREG(namedInfo.st_mode) && namedInfo.st_nlink == 1;
            return SameIdentity(openedIdentity, namedIdentity) && unlinkat(root.handle.Get(), name.c_str(), 0) == 0;
#endif
        }

        bool ReadOpenedFile(ScopedNativeHandle::Value handle, std::uint64_t size, size_t maximumBytes,
                            std::string& output)
        {
            if (size > maximumBytes || size > static_cast<std::uint64_t>(std::numeric_limits<size_t>::max()))
                return false;
            output.assign(static_cast<size_t>(size), '\0');
            size_t offset = 0;
#ifdef _WIN32
            LARGE_INTEGER beginning{};
            if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN))
                return false;
            while (offset < output.size())
            {
                DWORD count = 0;
                const DWORD requested = static_cast<DWORD>(
                    (std::min)(output.size() - offset, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
                if (!ReadFile(handle, output.data() + offset, requested, &count, nullptr) || count == 0)
                    return false;
                offset += count;
            }
#else
            if (lseek(handle, 0, SEEK_SET) < 0)
                return false;
            while (offset < output.size())
            {
                const ssize_t count = read(handle, output.data() + offset, output.size() - offset);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    return false;
                offset += static_cast<size_t>(count);
            }
#endif
            return true;
        }

        bool WriteNewFileInDirectory(const PinnedDirectory& root, const std::filesystem::path& name,
                                     std::string_view content)
        {
            if (name.empty() || name.has_parent_path())
                return false;
#ifdef _WIN32
            ScopedNativeHandle handle(CreateFileW((root.path / name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
#else
            int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            ScopedNativeHandle handle(openat(root.handle.Get(), name.c_str(), flags, S_IRUSR | S_IWUSR));
#endif
            if (!handle)
                return false;

            size_t offset = 0;
            while (offset < content.size())
            {
#ifdef _WIN32
                DWORD count = 0;
                const DWORD requested = static_cast<DWORD>(
                    (std::min)(content.size() - offset, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
                if (!WriteFile(handle.Get(), content.data() + offset, requested, &count, nullptr) || count == 0)
                    return false;
#else
                const ssize_t count = write(handle.Get(), content.data() + offset, content.size() - offset);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    return false;
#endif
                offset += static_cast<size_t>(count);
            }
            return true;
        }

        bool PinArtifactPath(const PinnedDirectory& root, std::string& artifactPath, ArtifactIdentity& identity,
                             bool required)
        {
            if (artifactPath.empty())
                return !required;

            std::filesystem::path name;
            if (!ArtifactNameInRoot(root, artifactPath, name))
                return false;

            ScopedNativeHandle handle;
            if (!OpenArtifact(root, name, handle, identity))
                return false;
            artifactPath = PathToUtf8(root.path / name);
            return true;
        }

        bool NormalizeManifestArtifacts(CrashManifest& manifest, PinnedDirectory& root)
        {
            if (!PinArtifactPath(root, manifest.logFile, manifest.logIdentity, true) ||
                !PinArtifactPath(root, manifest.dumpFile, manifest.dumpIdentity, false) ||
                !PinArtifactPath(root, manifest.screenshotFile, manifest.screenshotIdentity, false) ||
                !PinArtifactPath(root, manifest.zipFile, manifest.zipIdentity, false))
            {
                return false;
            }

            manifest.artifactRoot = PathToUtf8(root.path);
            manifest.artifactRootIdentity = root.identity;
            return true;
        }

        bool ReadArtifact(const PinnedDirectory& root, const std::string& artifactPath,
                          const ArtifactIdentity& expectedIdentity, size_t maximumBytes, std::string& output)
        {
            std::filesystem::path name;
            if (!ArtifactNameInRoot(root, artifactPath, name))
                return false;

            ScopedNativeHandle handle;
            ArtifactIdentity actualIdentity;
            std::uint64_t size = 0;
            if (!OpenArtifact(root, name, handle, actualIdentity, &size) ||
                !SameIdentity(expectedIdentity, actualIdentity))
                return false;
            return ReadOpenedFile(handle.Get(), size, maximumBytes, output);
        }

        bool ValidateArtifact(const PinnedDirectory& root, const std::string& artifactPath,
                              const ArtifactIdentity& expectedIdentity, bool required)
        {
            if (artifactPath.empty())
                return !required;
            std::filesystem::path name;
            if (!ArtifactNameInRoot(root, artifactPath, name))
                return false;
            ScopedNativeHandle handle;
            ArtifactIdentity actualIdentity;
            return OpenArtifact(root, name, handle, actualIdentity) && SameIdentity(expectedIdentity, actualIdentity);
        }

        bool LoadManifestFromPinnedDirectory(PinnedDirectory& root, const std::filesystem::path& manifestName,
                                             CrashManifest& output, bool consume = false)
        {
            if (manifestName.empty() || manifestName.has_parent_path())
                return false;

            ScopedNativeHandle manifestHandle;
            ArtifactIdentity manifestIdentity;
            std::uint64_t manifestSize = 0;
            if (!OpenArtifact(root, manifestName, manifestHandle, manifestIdentity, &manifestSize, consume) ||
                manifestSize == 0 || manifestSize > kMaxManifestBytes)
            {
                return false;
            }

            std::string json;
            ScopedStringWiper wipeJson(json);
            if (!ReadOpenedFile(manifestHandle.Get(), manifestSize, kMaxManifestBytes, json))
                return false;

            CrashManifest parsed;
            ScopedManifestCredentialWiper wipeParsedCredentials(parsed);
            ManifestJsonReader reader(json);
            if (!reader.Parse(parsed) || parsed.logFile.empty() || !NormalizeManifestArtifacts(parsed, root))
                return false;

            if (consume && !RemoveOpenedArtifact(root, manifestName, manifestHandle.Get(), manifestIdentity))
                return false;

            SecureWipeTransportConfiguration(output);
            output = std::move(parsed);
            return true;
        }

        bool IsReadyManifestName(std::string_view name)
        {
            constexpr std::string_view prefix = "crash_manifest_";
            constexpr std::string_view suffix = ".json";
            if (name.size() != prefix.size() + 16 + suffix.size() || !name.starts_with(prefix) ||
                !name.ends_with(suffix))
            {
                return false;
            }
            for (const char character : name.substr(prefix.size(), 16))
            {
                if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
                    return false;
            }
            return true;
        }

        std::vector<std::filesystem::path> ListReadyManifests(const PinnedDirectory& root)
        {
            std::vector<std::filesystem::path> manifests;
#ifdef _WIN32
            WIN32_FIND_DATAW entry{};
            HANDLE search = FindFirstFileW((root.path / L"crash_manifest_*.json").c_str(), &entry);
            if (search != INVALID_HANDLE_VALUE)
            {
                do
                {
                    const std::filesystem::path name(entry.cFileName);
                    if (IsReadyManifestName(PathToUtf8(name)))
                    {
                        manifests.push_back(name);
                        if (manifests.size() >= kMaxReadyManifests)
                            break;
                    }
                } while (FindNextFileW(search, &entry));
                FindClose(search);
            }
#else
            int flags = O_RDONLY;
#ifdef O_DIRECTORY
            flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            const int directoryHandle = openat(root.handle.Get(), ".", flags);
            if (directoryHandle < 0)
                return manifests;
            DIR* directory = fdopendir(directoryHandle);
            if (!directory)
            {
                close(directoryHandle);
                return manifests;
            }
            while (const dirent* entry = readdir(directory))
            {
                if (IsReadyManifestName(entry->d_name))
                {
                    manifests.emplace_back(entry->d_name);
                    if (manifests.size() >= kMaxReadyManifests)
                        break;
                }
            }
            closedir(directory);
#endif
            std::sort(manifests.begin(), manifests.end(),
                      [](const auto& lhs, const auto& rhs) { return PathToUtf8(lhs) < PathToUtf8(rhs); });
            return manifests;
        }

        bool ClaimManifest(const PinnedDirectory& root, const std::filesystem::path& readyName,
                           std::filesystem::path& claimedName)
        {
            if (!IsReadyManifestName(PathToUtf8(readyName)))
                return false;
            claimedName = readyName;
            claimedName += ".claimed.";
#ifdef _WIN32
            claimedName += std::to_string(GetCurrentProcessId());
#else
            claimedName += std::to_string(getpid());
#endif
#ifdef _WIN32
            return MoveFileExW((root.path / readyName).c_str(), (root.path / claimedName).c_str(),
                               MOVEFILE_WRITE_THROUGH) != FALSE;
#else
            return renameat(root.handle.Get(), readyName.c_str(), root.handle.Get(), claimedName.c_str()) == 0;
#endif
        }

        void RemoveClaimedManifest(const PinnedDirectory& root, const std::filesystem::path& claimedName)
        {
            ScopedNativeHandle handle;
            ArtifactIdentity identity;
            if (OpenArtifact(root, claimedName, handle, identity, nullptr, true))
                RemoveOpenedArtifact(root, claimedName, handle.Get(), identity);
        }

        bool ClaimNextManifest(PinnedDirectory& root, CrashManifest& output)
        {
            for (const std::filesystem::path& readyName : ListReadyManifests(root))
            {
                std::filesystem::path claimedName;
                if (!ClaimManifest(root, readyName, claimedName))
                    continue;
                if (LoadManifestFromPinnedDirectory(root, claimedName, output, true))
                    return true;
                RemoveClaimedManifest(root, claimedName);
            }
            return false;
        }
    } // namespace

    static std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 16);
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char c : s)
        {
            if (c == '"')
                out += "\\\"";
            else if (c == '\\')
                out += "\\\\";
            else if (c == '\b')
                out += "\\b";
            else if (c == '\f')
                out += "\\f";
            else if (c == '\n')
                out += "\\n";
            else if (c == '\r')
                out += "\\r";
            else if (c == '\t')
                out += "\\t";
            else if (c < 0x20)
            {
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0F]);
            }
            else
                out += static_cast<char>(c);
        }
        return out;
    }

    // ============================================================================
    // Manifest I/O
    // ============================================================================

    bool LoadManifest(const std::string& path, CrashManifest& out)
    {
        const std::filesystem::path manifestPath = PathFromUtf8(path);
        PinnedDirectory root;
        if (!OpenPinnedDirectory(
                manifestPath.parent_path().empty() ? std::filesystem::path(".") : manifestPath.parent_path(), root))
            return false;

        std::filesystem::path manifestName;
        if (!ArtifactNameInRoot(root, path, manifestName))
            return false;
        return LoadManifestFromPinnedDirectory(root, manifestName, out);
    }

    bool WriteManifest(const std::string& path, const CrashManifest& m)
    {
        const std::filesystem::path manifestPath = PathFromUtf8(path);
        PinnedDirectory root;
        if (!OpenPinnedDirectory(
                manifestPath.parent_path().empty() ? std::filesystem::path(".") : manifestPath.parent_path(), root))
            return false;

        std::filesystem::path manifestName;
        if (!ArtifactNameInRoot(root, path, manifestName))
            return false;

        std::ostringstream json;
        json << "{\n";
        json << "  \"enginePID\": \"" << JsonEscape(m.enginePID) << "\",\n";
        json << "  \"timestamp\": \"" << JsonEscape(m.timestamp) << "\",\n";
        json << "  \"dumpFile\": \"" << JsonEscape(m.dumpFile) << "\",\n";
        json << "  \"logFile\": \"" << JsonEscape(m.logFile) << "\",\n";
        json << "  \"screenshotFile\": \"" << JsonEscape(m.screenshotFile) << "\",\n";
        json << "  \"zipFile\": \"" << JsonEscape(m.zipFile) << "\",\n";
        json << "  \"crashTitle\": \"" << JsonEscape(m.crashTitle) << "\",\n";
        json << "  \"requireConsent\": " << (m.requireConsent ? "true" : "false") << ",\n";
        json << "  \"allowScreenshotRefusal\": " << (m.allowScreenshotRefusal ? "true" : "false") << ",\n";
        json << "  \"promptUserDescription\": " << (m.promptUserDescription ? "true" : "false") << ",\n";
        json << "  \"fullMemoryDump\": " << (m.fullMemoryDump ? "true" : "false") << "\n";
        json << "}\n";
        return WriteNewFileInDirectory(root, manifestName, json.str());
    }

    // ============================================================================
    // Console-based crash reporter dialog (cross-platform)
    // ============================================================================

    std::string BuildConsentMessage(const CrashManifest& manifest)
    {
        std::string message = "SparkEngine has crashed. Would you like to review the crash report saved locally?\n\n"
                              "This reporter does not send files automatically. The local report can include ";
        if (!manifest.dumpFile.empty())
        {
            if (manifest.fullMemoryDump)
            {
                message += "a full-memory process dump (which can contain application or user data held in memory), ";
            }
            else
            {
                message += "a minimal process dump or core-location hint (which can still contain limited memory "
                           "and file paths), ";
            }
        }
        message += "stack traces, system and process information, and file paths. These diagnostics may contain "
                   "personal or sensitive data.";

        if (!manifest.screenshotFile.empty() && manifest.allowScreenshotRefusal)
        {
            message += "\n\nA screenshot was captured locally. You can choose whether to consider it part of the local "
                       "report in the next dialog.";
        }
        else if (!manifest.screenshotFile.empty())
        {
            message += "\n\nThe report also includes a screenshot of the last rendered frame.";
        }
        return message;
    }

    int RunCrashReporter(const CrashManifest& untrustedManifest)
    {
        CrashManifest manifest = untrustedManifest;
        SecureWipeTransportConfiguration(manifest);
        PinnedDirectory root;
        if (manifest.artifactRoot.empty() || !manifest.artifactRootIdentity.valid ||
            !OpenPinnedDirectory(PathFromUtf8(manifest.artifactRoot), root) ||
            !SameIdentity(manifest.artifactRootIdentity, root.identity) ||
            !ValidateArtifact(root, manifest.logFile, manifest.logIdentity, true) ||
            !ValidateArtifact(root, manifest.dumpFile, manifest.dumpIdentity, false) ||
            !ValidateArtifact(root, manifest.screenshotFile, manifest.screenshotIdentity, false) ||
            !ValidateArtifact(root, manifest.zipFile, manifest.zipIdentity, false))
        {
            std::cerr << "Crash report rejected: the private artifact root or a file identity changed.\n";
            return 2;
        }

        constexpr size_t kMaxCrashLogBytes = 8 * 1024 * 1024;
        std::string crashLog;
        if (!ReadArtifact(root, manifest.logFile, manifest.logIdentity, kMaxCrashLogBytes, crashLog))
        {
            std::cerr << "Crash report rejected: the crash log could not be read safely within the size limit.\n";
            return 2;
        }

        std::cerr << "\n";
        std::cerr << "================================================================\n";
        std::cerr << "       SPARK ENGINE CRASH REPORTER\n";
        std::cerr << "================================================================\n";
        std::cerr << "\n";
        std::cerr << "The engine has crashed: " << manifest.crashTitle << "\n";
        std::cerr << "Timestamp: " << manifest.timestamp << "\n";
        std::cerr << "Crash log: " << manifest.logFile << "\n";
        std::cerr << "Crash log bytes read safely: " << crashLog.size() << "\n";
        if (!manifest.dumpFile.empty())
            std::cerr << "Dump file: " << manifest.dumpFile << "\n";
        if (!manifest.screenshotFile.empty())
            std::cerr << "Screenshot: " << manifest.screenshotFile << "\n";
        if (!manifest.zipFile.empty())
            std::cerr << "Prebuilt archive: ignored by this read-only reporter\n";
        std::cerr << "\n";

        // Consent
        bool shouldReview = true;
        if (manifest.requireConsent)
        {
#ifdef _WIN32
            const std::string consentMessage = BuildConsentMessage(manifest);
            int result = MessageBoxA(nullptr, consentMessage.c_str(), "Crash Report", MB_YESNO | MB_ICONERROR);
            shouldReview = (result == IDYES);
#else
            std::cerr << BuildConsentMessage(manifest) << "\n[Y/n]: ";
            std::string input;
            std::getline(std::cin, input);
            shouldReview = input.empty() || input[0] == 'Y' || input[0] == 'y';
#endif
        }

        if (!shouldReview)
        {
            std::cerr << "Crash report review declined. No files were sent or modified; artifacts remain local.\n";
            return 0;
        }

        // Screenshot consent
        bool includeScreenshot = true;
        if (manifest.allowScreenshotRefusal && !manifest.screenshotFile.empty())
        {
#ifdef _WIN32
            int ssResult = MessageBoxA(nullptr,
                                       "Include a screenshot of the last rendered frame "
                                       "with the crash report?",
                                       "Screenshot Consent", MB_YESNO | MB_ICONERROR);
            includeScreenshot = (ssResult == IDYES);
#else
            std::cerr << "Include screenshot with report? [Y/n]: ";
            std::string input;
            std::getline(std::cin, input);
            includeScreenshot = input.empty() || input[0] == 'Y' || input[0] == 'y';
#endif
        }

        if (!includeScreenshot && !manifest.screenshotFile.empty())
        {
            std::cerr << "Screenshot excluded from this review. Raw local artifacts were not modified.\n";
            manifest.screenshotFile.clear();
        }
        // Prebuilt archives can contain a screenshot or other stale bytes from
        // before the consent choice. This executable never reuses them.
        manifest.zipFile.clear();

        if (manifest.promptUserDescription)
        {
            std::cerr << "This read-only reporter does not append descriptions to crash files.\n";
        }

        std::cerr << "\nCrash report remains saved locally. No files were modified, archived, or uploaded.\n";
        return 0;
    }

    // ============================================================================
    // Watchdog mode — monitor engine process
    // ============================================================================

    static bool IsProcessAlive(const std::string& pidStr)
    {
        if (pidStr.empty())
            return false;

        std::uint64_t parsedPid = 0;
        const auto [end, error] = std::from_chars(pidStr.data(), pidStr.data() + pidStr.size(), parsedPid);
        if (error != std::errc{} || end != pidStr.data() + pidStr.size() || parsedPid == 0)
            return false;

#ifdef _WIN32
        if (parsedPid > (std::numeric_limits<DWORD>::max)())
            return false;
        const DWORD pid = static_cast<DWORD>(parsedPid);
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess)
            return false;
        DWORD exitCode = 0;
        GetExitCodeProcess(hProcess, &exitCode);
        CloseHandle(hProcess);
        return (exitCode == STILL_ACTIVE);
#else
        if (parsedPid > static_cast<std::uint64_t>((std::numeric_limits<pid_t>::max)()))
            return false;
        const pid_t pid = static_cast<pid_t>(parsedPid);
        return kill(pid, 0) == 0 || errno == EPERM;
#endif
    }

    int WatchAndReport(const std::string& manifestDir, const std::string& enginePID)
    {
        std::cerr << "[CrashReporter] Watching engine process (PID " << enginePID << ")...\n";

        PinnedDirectory manifestRoot;
        if (!OpenPinnedDirectory(PathFromUtf8(manifestDir), manifestRoot))
        {
            std::cerr << "[CrashReporter] Refusing untrusted or replaced manifest directory.\n";
            return 2;
        }
        int result = 0;
        bool engineExitObserved = false;

        // Atomically claim and drain every ready manifest before considering
        // process exit. A final grace interval catches publications racing exit.
        while (true)
        {
            CrashManifest manifest;
            if (ClaimNextManifest(manifestRoot, manifest))
            {
                std::cerr << "[CrashReporter] Crash manifest detected!\n";
                const int reportResult = RunCrashReporter(manifest);
                if (reportResult != 0)
                    result = reportResult;
                continue;
            }

            if (!IsProcessAlive(enginePID))
            {
                if (!engineExitObserved)
                {
                    engineExitObserved = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                std::cerr << "[CrashReporter] Engine exited normally.\n";
                return result;
            }

            engineExitObserved = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

} // namespace SparkCrashReporter
