/**
 * @file OrchestratorIdentity.cpp
 * @brief Secure cross-process persistence for SparkOrchestrator mutation keys.
 */

#include "OrchestratorIdentity.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spark::Daemon
{
    namespace
    {
        constexpr std::string_view kMagic = "SPORCHCLI1\n";
        constexpr size_t kMaximumStateBytes = 256;

        std::string GenerateClientInstance()
        {
            std::random_device random;
            std::array<uint32_t, 4> words{};
            for (auto& word : words)
                word = random();
            char buffer[40]{};
            std::snprintf(buffer, sizeof(buffer), "cli-%08x%08x%08x%08x", words[0], words[1], words[2], words[3]);
            return buffer;
        }

        bool DecodeState(std::string_view bytes, MutationKey& key)
        {
            if (!bytes.starts_with(kMagic))
                return false;
            bytes.remove_prefix(kMagic.size());
            const auto newline = bytes.find('\n');
            if (newline == std::string_view::npos || newline == 0 || newline > kMaximumClientInstanceLength)
                return false;
            key.clientInstance.assign(bytes.substr(0, newline));
            bytes.remove_prefix(newline + 1);
            if (bytes.empty() || bytes.back() != '\n')
                return false;
            bytes.remove_suffix(1);
            uint64_t lastSequence = 0;
            const auto [end, error] = std::from_chars(bytes.data(), bytes.data() + bytes.size(), lastSequence);
            if (error != std::errc{} || end != bytes.data() + bytes.size() ||
                lastSequence == std::numeric_limits<uint64_t>::max())
                return false;
            key.sequence = lastSequence + 1;
            return true;
        }

        std::string EncodeState(const MutationKey& key)
        {
            return std::string(kMagic) + key.clientInstance + "\n" + std::to_string(key.sequence) + "\n";
        }

        std::filesystem::path LockPath(const std::filesystem::path& path)
        {
            auto lock = path;
            lock += ".lock";
            return lock;
        }

        std::filesystem::path TemporaryPath(const std::filesystem::path& path)
        {
            auto temporary = path;
            temporary += ".tmp." + GenerateClientInstance();
            return temporary;
        }

#if defined(_WIN32)
        bool ReadStateFile(const std::filesystem::path& path, std::string& bytes, std::string& error)
        {
            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                const DWORD openError = ::GetLastError();
                if (openError == ERROR_FILE_NOT_FOUND || openError == ERROR_PATH_NOT_FOUND)
                {
                    bytes.clear();
                    return true;
                }
                error = "could not open mutation identity state";
                return false;
            }
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            LARGE_INTEGER size{};
            if (!::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
                (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                ::GetFileType(file) != FILE_TYPE_DISK || !::GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
                size.QuadPart > kMaximumStateBytes)
            {
                ::CloseHandle(file);
                error = "mutation identity state is invalid or too large";
                return false;
            }
            bytes.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            const bool success =
                bytes.empty() || (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                                  static_cast<size_t>(read) == bytes.size());
            ::CloseHandle(file);
            if (!success)
                error = "could not read mutation identity state";
            return success;
        }

        bool AtomicWriteState(const std::filesystem::path& path, std::string_view bytes, std::string& error)
        {
            const auto temporary = TemporaryPath(path);
            HANDLE file =
                ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = "could not create temporary mutation identity state";
                return false;
            }
            DWORD written = 0;
            const bool persisted =
                bytes.size() <= MAXDWORD &&
                ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                static_cast<size_t>(written) == bytes.size() && ::FlushFileBuffers(file);
            ::CloseHandle(file);
            if (!persisted ||
                !::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                (void)::DeleteFileW(temporary.c_str());
                error = "could not atomically persist mutation identity sequence";
                return false;
            }
            return true;
        }
#else
        bool ValidatePrivateFile(int file, size_t maximumSize, std::string& error, std::string_view label)
        {
            struct stat metadata{};
            if (::fstat(file, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
                (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0 || metadata.st_size < 0 ||
                static_cast<uint64_t>(metadata.st_size) > maximumSize)
            {
                error = std::string(label) + " is not a private regular file";
                return false;
            }
            return true;
        }

        bool ReadStateFile(const std::filesystem::path& path, std::string& bytes, std::string& error)
        {
            const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (file < 0)
            {
                if (errno == ENOENT)
                {
                    bytes.clear();
                    return true;
                }
                error = "could not securely open mutation identity state";
                return false;
            }
            struct stat metadata{};
            if (::fstat(file, &metadata) != 0 ||
                !ValidatePrivateFile(file, kMaximumStateBytes, error, "mutation identity state"))
            {
                ::close(file);
                return false;
            }
            bytes.resize(static_cast<size_t>(metadata.st_size));
            size_t position = 0;
            while (position < bytes.size())
            {
                const ssize_t count = ::read(file, bytes.data() + position, bytes.size() - position);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                {
                    ::close(file);
                    error = "could not read mutation identity state";
                    return false;
                }
                position += static_cast<size_t>(count);
            }
            ::close(file);
            return true;
        }

        bool AtomicWriteState(const std::filesystem::path& path, std::string_view bytes, std::string& error)
        {
            const auto temporary = TemporaryPath(path);
            const int file =
                ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
            if (file < 0)
            {
                error = "could not create temporary mutation identity state";
                return false;
            }
            size_t position = 0;
            bool persisted = true;
            while (position < bytes.size())
            {
                const ssize_t count = ::write(file, bytes.data() + position, bytes.size() - position);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                {
                    persisted = false;
                    break;
                }
                position += static_cast<size_t>(count);
            }
            persisted = persisted && ::fsync(file) == 0;
            if (::close(file) != 0)
                persisted = false;
            if (!persisted || ::rename(temporary.c_str(), path.c_str()) != 0)
            {
                (void)::unlink(temporary.c_str());
                error = "could not atomically persist mutation identity sequence";
                return false;
            }
            const auto directoryPath = path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
            const int directory = ::open(directoryPath.c_str(), O_RDONLY | O_CLOEXEC);
            const bool openedDirectory = directory >= 0;
            const bool flushedDirectory = openedDirectory && ::fsync(directory) == 0;
            const int flushError = flushedDirectory ? 0 : errno;
            if (!openedDirectory || (!flushedDirectory && flushError != EINVAL && flushError != ENOTSUP))
            {
                if (directory >= 0)
                    ::close(directory);
                error = "could not flush mutation identity directory";
                return false;
            }
            ::close(directory);
            return true;
        }
#endif
    } // namespace

    OrchestratorIdentityLease::~OrchestratorIdentityLease()
    {
        Release();
    }

    OrchestratorIdentityLease::OrchestratorIdentityLease(OrchestratorIdentityLease&& other) noexcept
        : m_key(std::move(other.m_key)), m_file(other.m_file)
    {
        other.m_file = -1;
    }

    OrchestratorIdentityLease& OrchestratorIdentityLease::operator=(OrchestratorIdentityLease&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_key = std::move(other.m_key);
            m_file = other.m_file;
            other.m_file = -1;
        }
        return *this;
    }

    void OrchestratorIdentityLease::Release() noexcept
    {
        if (m_file == -1)
            return;
#if defined(_WIN32)
        OVERLAPPED lock{};
        (void)::UnlockFileEx(reinterpret_cast<HANDLE>(m_file), 0, MAXDWORD, MAXDWORD, &lock);
        ::CloseHandle(reinterpret_cast<HANDLE>(m_file));
#else
        (void)::flock(static_cast<int>(m_file), LOCK_UN);
        (void)::close(static_cast<int>(m_file));
#endif
        m_file = -1;
    }

    std::optional<OrchestratorIdentityLease> OrchestratorIdentityLease::Acquire(const std::filesystem::path& path,
                                                                                std::string& error)
    {
        std::error_code filesystemError;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), filesystemError);
        if (filesystemError)
        {
            error = "could not create mutation identity directory";
            return std::nullopt;
        }

        OrchestratorIdentityLease lease;
        const auto lockPath = LockPath(path);
#if defined(_WIN32)
        HANDLE file =
            ::CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = "could not open mutation identity lock";
            return std::nullopt;
        }
        lease.m_file = reinterpret_cast<std::intptr_t>(file);
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        LARGE_INTEGER lockSize{};
        if (!::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || ::GetFileType(file) != FILE_TYPE_DISK ||
            !::GetFileSizeEx(file, &lockSize) || lockSize.QuadPart < 0 || lockSize.QuadPart > kMaximumStateBytes)
        {
            error = "mutation identity lock must not be a reparse point";
            return std::nullopt;
        }
        OVERLAPPED lock{};
        if (!::LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &lock))
        {
            error = "could not lock mutation identity state";
            return std::nullopt;
        }
#else
        const int file = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (file < 0)
        {
            error = "could not securely open mutation identity lock";
            return std::nullopt;
        }
        lease.m_file = file;
        if (!ValidatePrivateFile(file, kMaximumStateBytes, error, "mutation identity lock") ||
            ::flock(file, LOCK_EX) != 0)
        {
            if (error.empty())
                error = "could not lock mutation identity state";
            return std::nullopt;
        }
#endif

        std::string bytes;
        if (!ReadStateFile(path, bytes, error))
            return std::nullopt;

        if (bytes.empty())
        {
            lease.m_key = {GenerateClientInstance(), 1};
        }
        else if (!DecodeState(bytes, lease.m_key))
        {
            error = "mutation identity file is malformed or exhausted";
            return std::nullopt;
        }

        const std::string encoded = EncodeState(lease.m_key);
        if (!AtomicWriteState(path, encoded, error))
            return std::nullopt;
        return lease;
    }

    std::filesystem::path DefaultOrchestratorIdentityPath()
    {
        if (const char* overridePath = std::getenv("SPARK_ORCHESTRATOR_IDENTITY"); overridePath && *overridePath)
            return std::filesystem::u8path(overridePath);
#if defined(_WIN32)
        if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData)
            return std::filesystem::u8path(localAppData) / "SparkEngine" / "orchestrator.identity";
#else
        if (const char* stateHome = std::getenv("XDG_STATE_HOME"); stateHome && *stateHome)
            return std::filesystem::u8path(stateHome) / "sparkengine" / "orchestrator.identity";
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::filesystem::u8path(home) / ".local" / "state" / "sparkengine" / "orchestrator.identity";
#endif
        return std::filesystem::temp_directory_path() / "sparkengine-orchestrator.identity";
    }
} // namespace Spark::Daemon
