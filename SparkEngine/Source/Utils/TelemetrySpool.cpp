/**
 * @file TelemetrySpool.cpp
 * @brief Public TelemetrySpool methods: configure, store, restore, clear.
 */

#include "Utils/TelemetrySpool.h"
#include "Utils/TelemetrySpoolInternal.h"

#include "Utils/Telemetry.h"

#include <system_error>

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

        const auto openDirectory = [&handles](const std::filesystem::path& path, bool isRoot)
        {
            HANDLE handle =
                CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
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
