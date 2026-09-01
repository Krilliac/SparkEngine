/**
 * @file TelemetrySpool.cpp
 * @brief Versioned, bounded, atomic telemetry spool implementation.
 */

#include "Utils/TelemetrySpool.h"

#include "Utils/Telemetry.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spark::TelemetryDetail
{
    namespace
    {
        constexpr std::array<uint8_t, 8> kMagic{'S', 'P', 'A', 'R', 'K', 'T', 'L', 'M'};
        constexpr uint32_t kFormatVersion = 1;
        constexpr size_t kHeaderBytes = kMagic.size() + sizeof(uint32_t) + sizeof(uint32_t);
        constexpr size_t kMinimumEventBytes = sizeof(uint64_t) * 2 + sizeof(uint32_t) * 3;
        constexpr uint32_t kAbsoluteMaxEvents = 100000;
        constexpr uint32_t kMaxPropertiesPerEvent = 256;
        constexpr uint32_t kMaxStringBytes = 1024 * 1024;

        bool CheckedAdd(size_t& total, size_t amount)
        {
            if (amount > (std::numeric_limits<size_t>::max)() - total)
                return false;
            total += amount;
            return true;
        }

        bool TryResize(std::vector<uint8_t>& bytes, size_t size)
        {
            try
            {
                bytes.resize(size);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool EventSerializedSize(const TelemetryEvent& event, size_t& size)
        {
            if (event.sequence == 0 || event.sequence == (std::numeric_limits<uint64_t>::max)() ||
                event.name.size() > kMaxStringBytes || event.sessionId.size() > kMaxStringBytes ||
                event.properties.size() > kMaxPropertiesPerEvent)
            {
                return false;
            }

            size = sizeof(uint64_t) * 2 + sizeof(uint32_t) * 3;
            if (!CheckedAdd(size, event.name.size()) || !CheckedAdd(size, event.sessionId.size()))
                return false;

            for (const auto& [key, value] : event.properties)
            {
                if (key.size() > kMaxStringBytes || value.size() > kMaxStringBytes ||
                    !CheckedAdd(size, sizeof(uint32_t) * 2) || !CheckedAdd(size, key.size()) ||
                    !CheckedAdd(size, value.size()))
                {
                    return false;
                }
            }
            return true;
        }

        void AppendU32(std::vector<uint8_t>& bytes, uint32_t value)
        {
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
        }

        void AppendU64(std::vector<uint8_t>& bytes, uint64_t value)
        {
            for (unsigned shift = 0; shift < 64; shift += 8)
                bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
        }

        void AppendString(std::vector<uint8_t>& bytes, std::string_view value)
        {
            AppendU32(bytes, static_cast<uint32_t>(value.size()));
            bytes.insert(bytes.end(), value.begin(), value.end());
        }

        bool Serialize(const std::vector<TelemetryEvent>& events, uint64_t maximumBytes, std::vector<uint8_t>& bytes)
        {
            if (events.empty() || events.size() > (std::numeric_limits<uint32_t>::max)())
                return false;

            size_t expectedSize = kHeaderBytes;
            uint64_t previousSequence = 0;
            for (const auto& event : events)
            {
                size_t eventSize = 0;
                if (!EventSerializedSize(event, eventSize) || event.sequence <= previousSequence ||
                    !CheckedAdd(expectedSize, eventSize))
                {
                    return false;
                }
                previousSequence = event.sequence;
            }
            if (expectedSize > maximumBytes)
                return false;

            bytes.clear();
            bytes.reserve(expectedSize);
            bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
            AppendU32(bytes, kFormatVersion);
            AppendU32(bytes, static_cast<uint32_t>(events.size()));

            for (const auto& event : events)
            {
                AppendU64(bytes, event.sequence);
                AppendU64(bytes, event.timestamp);
                AppendString(bytes, event.name);
                AppendString(bytes, event.sessionId);

                std::vector<std::pair<std::string_view, std::string_view>> properties;
                properties.reserve(event.properties.size());
                for (const auto& [key, value] : event.properties)
                    properties.emplace_back(key, value);
                std::sort(properties.begin(), properties.end());

                AppendU32(bytes, static_cast<uint32_t>(properties.size()));
                for (const auto& [key, value] : properties)
                {
                    AppendString(bytes, key);
                    AppendString(bytes, value);
                }
            }
            return bytes.size() == expectedSize;
        }

        class BinaryReader final
        {
          public:
            explicit BinaryReader(const std::vector<uint8_t>& bytes) : m_bytes(bytes) {}

            bool ReadBytes(void* output, size_t count)
            {
                if (count > m_bytes.size() - m_offset)
                    return false;
                if (count > 0)
                    std::memcpy(output, m_bytes.data() + m_offset, count);
                m_offset += count;
                return true;
            }

            bool ReadU32(uint32_t& value)
            {
                std::array<uint8_t, 4> encoded{};
                if (!ReadBytes(encoded.data(), encoded.size()))
                    return false;
                value = 0;
                for (unsigned index = 0; index < encoded.size(); ++index)
                    value |= static_cast<uint32_t>(encoded[index]) << (index * 8);
                return true;
            }

            bool ReadU64(uint64_t& value)
            {
                std::array<uint8_t, 8> encoded{};
                if (!ReadBytes(encoded.data(), encoded.size()))
                    return false;
                value = 0;
                for (unsigned index = 0; index < encoded.size(); ++index)
                    value |= static_cast<uint64_t>(encoded[index]) << (index * 8);
                return true;
            }

            bool ReadString(std::string& value)
            {
                uint32_t length = 0;
                if (!ReadU32(length) || length > kMaxStringBytes || length > m_bytes.size() - m_offset)
                    return false;
                value.assign(reinterpret_cast<const char*>(m_bytes.data() + m_offset), length);
                m_offset += length;
                return true;
            }

            [[nodiscard]] bool AtEnd() const { return m_offset == m_bytes.size(); }

          private:
            const std::vector<uint8_t>& m_bytes;
            size_t m_offset = 0;
        };

        bool Parse(const std::vector<uint8_t>& bytes, uint32_t maximumEvents, std::vector<TelemetryEvent>& events)
        {
            BinaryReader reader(bytes);
            std::array<uint8_t, kMagic.size()> magic{};
            uint32_t version = 0;
            uint32_t eventCount = 0;
            if (!reader.ReadBytes(magic.data(), magic.size()) || magic != kMagic || !reader.ReadU32(version) ||
                version != kFormatVersion || !reader.ReadU32(eventCount) || eventCount == 0 ||
                eventCount > maximumEvents || eventCount > kAbsoluteMaxEvents || bytes.size() < kHeaderBytes ||
                eventCount > (bytes.size() - kHeaderBytes) / kMinimumEventBytes)
            {
                return false;
            }

            std::vector<TelemetryEvent> parsed;
            try
            {
                parsed.reserve(eventCount);
            }
            catch (...)
            {
                return false;
            }
            uint64_t previousSequence = 0;
            for (uint32_t eventIndex = 0; eventIndex < eventCount; ++eventIndex)
            {
                TelemetryEvent event;
                uint32_t propertyCount = 0;
                if (!reader.ReadU64(event.sequence) || event.sequence == 0 ||
                    event.sequence == (std::numeric_limits<uint64_t>::max)() || event.sequence <= previousSequence ||
                    !reader.ReadU64(event.timestamp) || !reader.ReadString(event.name) ||
                    !reader.ReadString(event.sessionId) || !reader.ReadU32(propertyCount) ||
                    propertyCount > kMaxPropertiesPerEvent)
                {
                    return false;
                }

                for (uint32_t propertyIndex = 0; propertyIndex < propertyCount; ++propertyIndex)
                {
                    std::string key;
                    std::string value;
                    if (!reader.ReadString(key) || !reader.ReadString(value) ||
                        !event.properties.emplace(std::move(key), std::move(value)).second)
                    {
                        return false;
                    }
                }

                previousSequence = event.sequence;
                parsed.push_back(std::move(event));
            }

            if (!reader.AtEnd())
                return false;
            events = std::move(parsed);
            return true;
        }

        bool HasReparseAttribute(const std::filesystem::path& path)
        {
#ifdef _WIN32
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
            (void)path;
            return false;
#endif
        }

        bool IsSafeDirectoryPath(const std::filesystem::path& path)
        {
            if (path.empty() || !path.is_absolute())
                return false;

            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                HasReparseAttribute(path))
            {
                return false;
            }

            std::filesystem::path current = path.root_path();
            for (const auto& component : path.relative_path())
            {
                current /= component;
                const auto componentStatus = std::filesystem::symlink_status(current, error);
                if (error || !std::filesystem::is_directory(componentStatus) ||
                    std::filesystem::is_symlink(componentStatus) || HasReparseAttribute(current))
                {
                    return false;
                }
            }
            return true;
        }

        bool IsSafeRegularFile(const std::filesystem::path& path)
        {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status) ||
                HasReparseAttribute(path))
            {
                return false;
            }
            const auto links = std::filesystem::hard_link_count(path, error);
            return !error && links == 1;
        }

        bool PathExists(const std::filesystem::path& path, bool& exists)
        {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (!error)
            {
                exists = std::filesystem::exists(status);
                return true;
            }
            if (error == std::errc::no_such_file_or_directory)
            {
                exists = false;
                return true;
            }
            exists = false;
            return false;
        }

        TelemetrySpoolResult RemoveOwnedFile(const std::filesystem::path& path)
        {
            bool exists = false;
            if (!PathExists(path, exists))
                return TelemetrySpoolResult::IoFailure;
            if (!exists)
                return TelemetrySpoolResult::Success;
            if (!IsSafeRegularFile(path))
                return TelemetrySpoolResult::Rejected;

            std::error_code error;
            if (!std::filesystem::remove(path, error) || error)
                return TelemetrySpoolResult::IoFailure;
            return TelemetrySpoolResult::Success;
        }

        TelemetrySpoolResult ReadOwnedFile(const std::filesystem::path& path, uint64_t maximumBytes,
                                           std::vector<uint8_t>& bytes)
        {
            if (!IsSafeRegularFile(path))
                return TelemetrySpoolResult::Rejected;

#ifdef _WIN32
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return TelemetrySpoolResult::IoFailure;

            BY_HANDLE_FILE_INFORMATION information{};
            LARGE_INTEGER size{};
            const bool safe =
                GetFileInformationByHandle(file, &information) != FALSE &&
                (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
                information.nNumberOfLinks == 1 && GetFileSizeEx(file, &size) != FALSE &&
                size.QuadPart >= static_cast<LONGLONG>(kHeaderBytes) &&
                static_cast<uint64_t>(size.QuadPart) <= maximumBytes;
            if (!safe)
            {
                CloseHandle(file);
                return TelemetrySpoolResult::Rejected;
            }

            if (!TryResize(bytes, static_cast<size_t>(size.QuadPart)))
            {
                CloseHandle(file);
                return TelemetrySpoolResult::IoFailure;
            }
            size_t offset = 0;
            while (offset < bytes.size())
            {
                DWORD read = 0;
                const DWORD request = static_cast<DWORD>(
                    (std::min)(bytes.size() - offset, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
                if (!ReadFile(file, bytes.data() + offset, request, &read, nullptr) || read == 0)
                {
                    CloseHandle(file);
                    return TelemetrySpoolResult::IoFailure;
                }
                offset += read;
            }
            uint8_t extra = 0;
            DWORD extraRead = 0;
            const bool readSucceeded = ReadFile(file, &extra, 1, &extraRead, nullptr) != FALSE;
            CloseHandle(file);
            if (!readSucceeded)
                return TelemetrySpoolResult::IoFailure;
            return extraRead == 0 ? TelemetrySpoolResult::Success : TelemetrySpoolResult::Rejected;
#else
            const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (file < 0)
                return errno == ELOOP ? TelemetrySpoolResult::Rejected : TelemetrySpoolResult::IoFailure;

            struct stat information{};
            if (fstat(file, &information) != 0 || !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
                information.st_size < static_cast<off_t>(kHeaderBytes) ||
                static_cast<uint64_t>(information.st_size) > maximumBytes)
            {
                close(file);
                return TelemetrySpoolResult::Rejected;
            }

            if (!TryResize(bytes, static_cast<size_t>(information.st_size)))
            {
                close(file);
                return TelemetrySpoolResult::IoFailure;
            }
            size_t offset = 0;
            while (offset < bytes.size())
            {
                const ssize_t readCount = read(file, bytes.data() + offset, bytes.size() - offset);
                if (readCount < 0 && errno == EINTR)
                    continue;
                if (readCount <= 0)
                {
                    close(file);
                    return TelemetrySpoolResult::IoFailure;
                }
                offset += static_cast<size_t>(readCount);
            }
            uint8_t extra = 0;
            ssize_t extraRead = -1;
            do
            {
                extraRead = read(file, &extra, 1);
            } while (extraRead < 0 && errno == EINTR);
            close(file);
            if (extraRead < 0)
                return TelemetrySpoolResult::IoFailure;
            return extraRead == 0 ? TelemetrySpoolResult::Success : TelemetrySpoolResult::Rejected;
#endif
        }

        TelemetrySpoolResult WriteOwnedFileAtomically(const std::filesystem::path& directory,
                                                      const std::filesystem::path& artifact,
                                                      const std::filesystem::path& staging,
                                                      const std::vector<uint8_t>& bytes)
        {
            const auto staleResult = RemoveOwnedFile(staging);
            if (staleResult != TelemetrySpoolResult::Success)
                return staleResult;

            bool artifactExists = false;
            if (!PathExists(artifact, artifactExists))
                return TelemetrySpoolResult::IoFailure;
            if (artifactExists && !IsSafeRegularFile(artifact))
                return TelemetrySpoolResult::Rejected;

#ifdef _WIN32
            HANDLE file =
                CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return TelemetrySpoolResult::IoFailure;

            size_t offset = 0;
            bool written = true;
            while (offset < bytes.size())
            {
                DWORD count = 0;
                const DWORD request = static_cast<DWORD>(
                    (std::min)(bytes.size() - offset, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
                if (!WriteFile(file, bytes.data() + offset, request, &count, nullptr) || count == 0)
                {
                    written = false;
                    break;
                }
                offset += count;
            }
            written = written && FlushFileBuffers(file) != FALSE;
            CloseHandle(file);
            if (!written)
            {
                DeleteFileW(staging.c_str());
                return TelemetrySpoolResult::IoFailure;
            }

            if (!MoveFileExW(staging.c_str(), artifact.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                DeleteFileW(staging.c_str());
                return TelemetrySpoolResult::IoFailure;
            }
#else
            const int file = open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (file < 0)
                return errno == ELOOP ? TelemetrySpoolResult::Rejected : TelemetrySpoolResult::IoFailure;

            size_t offset = 0;
            bool written = true;
            while (offset < bytes.size())
            {
                const ssize_t count = write(file, bytes.data() + offset, bytes.size() - offset);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                {
                    written = false;
                    break;
                }
                offset += static_cast<size_t>(count);
            }
            written = written && fsync(file) == 0;
            close(file);
            if (!written)
            {
                unlink(staging.c_str());
                return TelemetrySpoolResult::IoFailure;
            }
            if (rename(staging.c_str(), artifact.c_str()) != 0)
            {
                unlink(staging.c_str());
                return TelemetrySpoolResult::IoFailure;
            }

            const int directoryHandle = open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
            if (directoryHandle < 0 || fsync(directoryHandle) != 0)
            {
                if (directoryHandle >= 0)
                    close(directoryHandle);
                return TelemetrySpoolResult::IoFailure;
            }
            close(directoryHandle);
#endif

            return IsSafeRegularFile(artifact) ? TelemetrySpoolResult::Success : TelemetrySpoolResult::Rejected;
        }
    } // namespace

    TelemetrySpoolResult TelemetrySpool::InspectDeferredCleanupDirectory(std::string_view directory)
    {
        std::filesystem::path candidate{std::string(directory)};
        if (candidate.empty() || !candidate.is_absolute())
            return TelemetrySpoolResult::Rejected;
        candidate = candidate.lexically_normal();

        const auto root = candidate.root_path();
        if (root.empty())
            return TelemetrySpoolResult::Rejected;

#ifdef _WIN32
        struct HandleSet final
        {
            std::vector<HANDLE> values;
            ~HandleSet()
            {
                for (HANDLE value : values)
                    CloseHandle(value);
            }
        } handles;

        const auto openDirectory = [&handles](const std::filesystem::path& path, bool isRoot) {
            HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                const DWORD error = GetLastError();
                if (!isRoot && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
                    return TelemetrySpoolResult::NotFound;
                return TelemetrySpoolResult::IoFailure;
            }

            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)))
            {
                CloseHandle(handle);
                return TelemetrySpoolResult::IoFailure;
            }
            if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                CloseHandle(handle);
                return TelemetrySpoolResult::Rejected;
            }
            handles.values.push_back(handle);
            return TelemetrySpoolResult::Success;
        };

        std::filesystem::path current = root;
        auto components = candidate.relative_path();
        auto component = components.begin();

        // A UNC server name is not itself an openable directory. Pin the
        // \\server\share anchor as the root so an offline/missing share remains
        // an I/O failure rather than terminal absence.
        const auto rootName = candidate.root_name().native();
        if (rootName.size() >= 2 && rootName[0] == L'\\' && rootName[1] == L'\\')
        {
            if (component == components.end())
                return TelemetrySpoolResult::Rejected;
            current /= *component;
            ++component;
        }

        auto result = openDirectory(current, true);
        if (result != TelemetrySpoolResult::Success)
            return result;
        for (; component != components.end(); ++component)
        {
            current /= *component;
            result = openDirectory(current, false);
            if (result != TelemetrySpoolResult::Success)
                return result;
        }
        return TelemetrySpoolResult::Success;
#else
        struct DescriptorSet final
        {
            std::vector<int> values;
            ~DescriptorSet()
            {
                for (int value : values)
                    ::close(value);
            }
        } descriptors;

        int current = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (current < 0)
            return TelemetrySpoolResult::IoFailure;
        descriptors.values.push_back(current);

        for (const auto& component : candidate.relative_path())
        {
            const int next = ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0)
            {
                if (errno == ENOENT)
                    return TelemetrySpoolResult::NotFound;
                if (errno == ELOOP || errno == ENOTDIR)
                    return TelemetrySpoolResult::Rejected;
                return TelemetrySpoolResult::IoFailure;
            }
            descriptors.values.push_back(next);
            current = next;
        }
        return TelemetrySpoolResult::Success;
#endif
    }

    TelemetrySpoolResult TelemetrySpool::Configure(std::string_view directory, uint64_t maxBytes, uint32_t maxEvents)
    {
        m_directory.clear();
        m_artifactPath.clear();
        m_stagingPath.clear();
        m_maxBytes = 0;
        m_maxEvents = 0;

        if (directory.empty())
            return TelemetrySpoolResult::Disabled;
        if (maxBytes < kHeaderBytes || maxEvents == 0 || maxEvents > kAbsoluteMaxEvents)
            return TelemetrySpoolResult::Rejected;

        std::error_code error;
        std::filesystem::path configured =
            std::filesystem::absolute(std::filesystem::path(std::string(directory)), error);
        if (error || configured.empty())
            return TelemetrySpoolResult::Rejected;
        configured = configured.lexically_normal();
        bool directoryExists = false;
        if (!PathExists(configured, directoryExists))
            return TelemetrySpoolResult::IoFailure;
        if (!directoryExists)
        {
            const auto parent = configured.parent_path();
            if (!IsSafeDirectoryPath(parent))
                return TelemetrySpoolResult::Rejected;
            if (!std::filesystem::create_directory(configured, error) || error)
                return TelemetrySpoolResult::IoFailure;
        }
        if (!IsSafeDirectoryPath(configured))
            return TelemetrySpoolResult::Rejected;

        m_directory = configured;
        m_artifactPath = m_directory / std::string(kArtifactName);
        m_stagingPath = m_directory / std::string(kStagingName);
        m_maxBytes = maxBytes;
        m_maxEvents = maxEvents;
        if (!ValidateDirectory())
        {
            m_directory.clear();
            m_artifactPath.clear();
            m_stagingPath.clear();
            return TelemetrySpoolResult::Rejected;
        }

        bool stagingExists = false;
        if (!PathExists(m_stagingPath, stagingExists))
        {
            m_directory.clear();
            m_artifactPath.clear();
            m_stagingPath.clear();
            return TelemetrySpoolResult::IoFailure;
        }
        if (stagingExists)
        {
            const auto stagingResult = RemoveOwnedFile(m_stagingPath);
            if (stagingResult != TelemetrySpoolResult::Success)
            {
                m_directory.clear();
                m_artifactPath.clear();
                m_stagingPath.clear();
                return stagingResult;
            }
        }
        return TelemetrySpoolResult::Success;
    }

    uint64_t TelemetrySpool::Constrain(std::vector<TelemetryEvent>& events,
                                       std::vector<uint64_t>* droppedSequences) const
    {
        if (!IsConfigured())
            return 0;

        std::vector<TelemetryEvent> accepted;
        accepted.reserve((std::min)(events.size(), static_cast<size_t>(m_maxEvents)));
        size_t bytes = kHeaderBytes;
        uint64_t dropped = 0;
        uint64_t previousSequence = 0;
        for (auto& event : events)
        {
            size_t eventBytes = 0;
            if (accepted.size() >= m_maxEvents || !EventSerializedSize(event, eventBytes) ||
                event.sequence <= previousSequence || eventBytes > m_maxBytes - bytes)
            {
                if (droppedSequences != nullptr)
                    droppedSequences->push_back(event.sequence);
                ++dropped;
                continue;
            }
            bytes += eventBytes;
            previousSequence = event.sequence;
            accepted.push_back(std::move(event));
        }
        events = std::move(accepted);
        return dropped;
    }

    TelemetrySpoolResult TelemetrySpool::Restore(std::vector<TelemetryEvent>& events) const
    {
        events.clear();
        if (!IsConfigured())
            return TelemetrySpoolResult::Disabled;
        if (!ValidateDirectory())
            return TelemetrySpoolResult::Rejected;

        bool exists = false;
        if (!PathExists(m_artifactPath, exists))
            return TelemetrySpoolResult::IoFailure;
        if (!exists)
            return TelemetrySpoolResult::NotFound;

        std::vector<uint8_t> bytes;
        const auto readResult = ReadOwnedFile(m_artifactPath, m_maxBytes, bytes);
        if (readResult != TelemetrySpoolResult::Success)
            return readResult;

        std::vector<TelemetryEvent> parsed;
        try
        {
            if (!Parse(bytes, m_maxEvents, parsed))
                return TelemetrySpoolResult::Rejected;
        }
        catch (...)
        {
            return TelemetrySpoolResult::IoFailure;
        }
        events = std::move(parsed);
        return TelemetrySpoolResult::Success;
    }

    TelemetrySpoolResult TelemetrySpool::Store(const std::vector<TelemetryEvent>& events)
    {
        if (!IsConfigured())
            return TelemetrySpoolResult::Disabled;
        if (!ValidateDirectory())
            return TelemetrySpoolResult::Rejected;
        if (events.empty())
            return Clear();
        if (events.size() > m_maxEvents)
            return TelemetrySpoolResult::Rejected;

        std::vector<uint8_t> bytes;
        try
        {
            if (!Serialize(events, m_maxBytes, bytes))
                return TelemetrySpoolResult::Rejected;
        }
        catch (...)
        {
            return TelemetrySpoolResult::IoFailure;
        }
        return WriteOwnedFileAtomically(m_directory, m_artifactPath, m_stagingPath, bytes);
    }

    TelemetrySpoolResult TelemetrySpool::Clear()
    {
        if (!IsConfigured())
            return TelemetrySpoolResult::Disabled;
        bool directoryExists = false;
        if (!PathExists(m_directory, directoryExists))
            return TelemetrySpoolResult::IoFailure;
        if (!directoryExists)
            return TelemetrySpoolResult::Success;
        if (!ValidateDirectory())
            return TelemetrySpoolResult::Rejected;

        const auto stagingResult = RemoveOwnedFile(m_stagingPath);
        if (stagingResult != TelemetrySpoolResult::Success)
            return stagingResult;
        const auto artifactResult = RemoveOwnedFile(m_artifactPath);
        if (artifactResult != TelemetrySpoolResult::Success)
            return artifactResult;

#ifndef _WIN32
        const int directoryHandle = open(m_directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (directoryHandle < 0)
            return TelemetrySpoolResult::IoFailure;
        const bool synced = fsync(directoryHandle) == 0;
        close(directoryHandle);
        if (!synced)
            return TelemetrySpoolResult::IoFailure;
#endif
        return TelemetrySpoolResult::Success;
    }

    bool TelemetrySpool::ValidateDirectory() const
    {
        return IsSafeDirectoryPath(m_directory);
    }
} // namespace Spark::TelemetryDetail
