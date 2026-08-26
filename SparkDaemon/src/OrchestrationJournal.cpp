/**
 * @file OrchestrationJournal.cpp
 * @brief Atomic, bounded orchestration journal implementation.
 */

#include "OrchestrationJournal.h"

#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spark::Daemon
{
    namespace
    {
        constexpr std::string_view kMagic = "SPORCH2";
        constexpr size_t kMaximumJournalBlob = 8 * 1024 * 1024;
        constexpr uint32_t kWalMagic = 0x4C415753;

        enum class WalKind : uint8_t
        {
            Intent = 1,
            Commit = 2,
        };

        uint64_t Checksum(const std::vector<uint8_t>& bytes) noexcept
        {
            uint64_t hash = 0xcbf29ce484222325ull;
            for (uint8_t byte : bytes)
            {
                hash ^= byte;
                hash *= 0x100000001b3ull;
            }
            return hash;
        }

        std::filesystem::path WalPath(const std::filesystem::path& path)
        {
            auto wal = path;
            wal += ".wal";
            return wal;
        }

        bool DurableWrite(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, bool append)
        {
#if defined(_WIN32)
            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                        append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            if (append)
                ::SetFilePointer(file, 0, nullptr, FILE_END);
            DWORD written = 0;
            const bool success = bytes.size() <= MAXDWORD &&
                                 ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                                 written == bytes.size() && ::FlushFileBuffers(file);
            ::CloseHandle(file);
            return success;
#else
            const int file =
                ::open(path.c_str(), O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC), S_IRUSR | S_IWUSR);
            if (file < 0)
                return false;
            size_t position = 0;
            while (position < bytes.size())
            {
                const ssize_t written = ::write(file, bytes.data() + position, bytes.size() - position);
                if (written <= 0)
                {
                    ::close(file);
                    return false;
                }
                position += static_cast<size_t>(written);
            }
            const bool success = ::fsync(file) == 0;
            ::close(file);
            return success;
#endif
        }

        bool ReadFileBounded(const std::filesystem::path& path, std::vector<uint8_t>& bytes)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                bytes.clear();
                return true;
            }
            input.seekg(0, std::ios::end);
            const auto length = input.tellg();
            if (length < 0 || static_cast<uint64_t>(length) > kMaximumJournalBlob)
                return false;
            input.seekg(0, std::ios::beg);
            bytes.resize(static_cast<size_t>(length));
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return input.good() || input.eof();
        }
    } // namespace

    std::optional<OrchestrationJournalState> LoadOrchestrationJournal(const std::filesystem::path& path,
                                                                      size_t maximumProcesses, size_t maximumClients)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return OrchestrationJournalState{};
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        if (length < 0 || static_cast<uint64_t>(length) > kMaximumJournalBlob)
            return std::nullopt;
        input.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input && !bytes.empty())
            return std::nullopt;

        Wire::Reader reader(bytes);
        std::string magic;
        uint32_t processCount = 0;
        if (!reader.ReadString(magic, kMagic.size()) || magic != kMagic || !Wire::ReadVersion(reader) ||
            !reader.Read(processCount) || processCount > maximumProcesses)
            return std::nullopt;

        OrchestrationJournalState state;
        state.processes.reserve(processCount);
        for (uint32_t index = 0; index < processCount; ++index)
        {
            std::vector<uint8_t> definitionBytes;
            std::vector<uint8_t> statusBytes;
            uint8_t desired = 0;
            uint32_t crashCount = 0;
            if (!reader.ReadBytes(definitionBytes, kMaximumJournalBlob) ||
                !reader.ReadBytes(statusBytes, kMaximumJournalBlob) || !reader.Read(desired) || desired > 1 ||
                !reader.Read(crashCount) || crashCount > 100)
                return std::nullopt;
            MutationKey ignored;
            JournalProcess process;
            std::vector<ProcessStatus> statuses;
            if (!DecodeProcessDefinition(definitionBytes, ignored, process.definition) ||
                !DecodeProcessStatuses(statusBytes, statuses, 1) || statuses.size() != 1)
                return std::nullopt;
            process.status = std::move(statuses.front());
            process.desiredRunning = desired != 0;
            process.crashTimestampsUnixMilliseconds.resize(crashCount);
            for (auto& timestamp : process.crashTimestampsUnixMilliseconds)
                if (!reader.Read(timestamp))
                    return std::nullopt;
            state.processes.push_back(std::move(process));
        }

        uint32_t mutationCount = 0;
        if (!reader.Read(mutationCount) || mutationCount > maximumClients)
            return std::nullopt;
        state.mutations.reserve(mutationCount);
        for (uint32_t index = 0; index < mutationCount; ++index)
        {
            JournalMutation mutation;
            if (!reader.ReadString(mutation.clientInstance, kMaximumClientInstanceLength) ||
                !reader.Read(mutation.sequence) || !reader.Read(mutation.response.messageType) ||
                !reader.ReadBytes(mutation.response.payload, kMaximumJournalBlob) || mutation.sequence == 0)
                return std::nullopt;
            state.mutations.push_back(std::move(mutation));
        }
        if (!reader.Finished())
            return std::nullopt;
        return state;
    }

    bool WriteOrchestrationJournal(const std::filesystem::path& path, const OrchestrationJournalState& state)
    {
        if (state.processes.size() > std::numeric_limits<uint32_t>::max() ||
            state.mutations.size() > std::numeric_limits<uint32_t>::max())
            return false;
        Wire::Writer writer;
        if (!writer.WriteString(kMagic, kMagic.size()))
            return false;
        Wire::WriteVersion(writer);
        writer.Write<uint32_t>(static_cast<uint32_t>(state.processes.size()));
        for (const auto& process : state.processes)
        {
            if (process.crashTimestampsUnixMilliseconds.size() > 100)
                return false;
            std::vector<uint8_t> definitionBytes;
            std::vector<uint8_t> statusBytes;
            if (!EncodeProcessDefinition({"journal", 1}, process.definition, definitionBytes) ||
                !EncodeProcessStatuses({process.status}, statusBytes) ||
                !writer.WriteBytes(definitionBytes, kMaximumJournalBlob) ||
                !writer.WriteBytes(statusBytes, kMaximumJournalBlob))
                return false;
            writer.Write<uint8_t>(process.desiredRunning ? 1 : 0);
            writer.Write<uint32_t>(static_cast<uint32_t>(process.crashTimestampsUnixMilliseconds.size()));
            for (int64_t timestamp : process.crashTimestampsUnixMilliseconds)
                writer.Write(timestamp);
        }
        writer.Write<uint32_t>(static_cast<uint32_t>(state.mutations.size()));
        for (const auto& mutation : state.mutations)
        {
            if (!writer.WriteString(mutation.clientInstance, kMaximumClientInstanceLength))
                return false;
            writer.Write(mutation.sequence);
            writer.Write(mutation.response.messageType);
            if (!writer.WriteBytes(mutation.response.payload, kMaximumJournalBlob))
                return false;
        }

        // LoadOrchestrationJournal rejects a whole snapshot above this bound.
        // Enforce the same bound before creating a temporary file so an
        // unreadable snapshot can never replace the last known-good state.
        auto bytes = writer.Take();
        if (bytes.size() > kMaximumJournalBlob)
            return false;

        std::error_code error;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return false;
        auto temporary = path;
        temporary += ".tmp";
        if (!DurableWrite(temporary, bytes, false))
            return false;
#if defined(_WIN32)
        if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return false;
#else
        std::filesystem::rename(temporary, path, error);
        if (error)
            return false;
        const auto directoryPath = path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
        const int directory = ::open(directoryPath.c_str(), O_RDONLY);
        if (directory >= 0)
        {
            (void)::fsync(directory);
            ::close(directory);
        }
#endif
        return true;
    }

    namespace
    {
        bool AppendWalRecord(const std::filesystem::path& path, const std::vector<uint8_t>& payload)
        {
            Wire::Writer frame;
            frame.Write(kWalMagic);
            frame.Write<uint32_t>(static_cast<uint32_t>(payload.size()));
            frame.Write(Checksum(payload));
            if (!frame.WriteBytes(payload, kMaximumJournalBlob))
                return false;
            return DurableWrite(WalPath(path), frame.Take(), true);
        }

        std::string MutationIdentity(const MutationKey& key)
        {
            return key.clientInstance + "\n" + std::to_string(key.sequence);
        }
    } // namespace

    bool AppendOrchestrationIntent(const std::filesystem::path& path, const OrchestrationIntent& intent)
    {
        Wire::Writer writer;
        writer.Write(WalKind::Intent);
        if (!WriteMutationKey(writer, intent.key))
            return false;
        writer.Write(intent.messageType);
        if (!writer.WriteString(intent.processId, kMaximumProcessIdLength))
            return false;
        writer.Write(intent.processIdBefore);
        writer.Write(intent.processStartTokenBefore);
        return AppendWalRecord(path, writer.Take());
    }

    bool AppendOrchestrationCommit(const std::filesystem::path& path, const MutationKey& key,
                                   const OrchestrationJournalState& state)
    {
        // The atomic snapshot is the authoritative post-operation state. The
        // following small WAL record closes the preceding intent. If a crash
        // occurs between these writes, recovery recognizes the mutation in the
        // snapshot and treats the intent as committed.
        if (!WriteOrchestrationJournal(path, state))
            return false;
        Wire::Writer writer;
        writer.Write(WalKind::Commit);
        if (!WriteMutationKey(writer, key))
            return false;
        if (!AppendWalRecord(path, writer.Take()))
            return false;
        std::error_code error;
        const auto walSize = std::filesystem::file_size(WalPath(path), error);
        if (!error && walSize > 1024 * 1024)
            return DurableWrite(WalPath(path), {}, false);
        return true;
    }

    std::optional<OrchestrationJournalState> RecoverOrchestrationJournal(const std::filesystem::path& path,
                                                                         size_t maximumProcesses, size_t maximumClients)
    {
        auto recovered = LoadOrchestrationJournal(path, maximumProcesses, maximumClients);
        if (!recovered)
            return std::nullopt;
        std::vector<uint8_t> wal;
        if (!ReadFileBounded(WalPath(path), wal))
            return std::nullopt;

        std::unordered_map<std::string, MutationKey> pending;
        size_t position = 0;
        while (position + 20 <= wal.size())
        {
            std::vector<uint8_t> headerBytes(wal.begin() + static_cast<std::ptrdiff_t>(position),
                                             wal.begin() + static_cast<std::ptrdiff_t>(position + 16));
            Wire::Reader header(headerBytes);
            uint32_t magic = 0;
            uint32_t payloadSize = 0;
            uint64_t checksum = 0;
            if (!header.Read(magic) || !header.Read(payloadSize) || !header.Read(checksum) || magic != kWalMagic ||
                payloadSize > kMaximumJournalBlob || position + 20ull + payloadSize > wal.size())
                break; // Torn/invalid tail: preserve every prior complete record.
            const uint32_t duplicateSize =
                static_cast<uint32_t>(wal[position + 16]) | (static_cast<uint32_t>(wal[position + 17]) << 8) |
                (static_cast<uint32_t>(wal[position + 18]) << 16) | (static_cast<uint32_t>(wal[position + 19]) << 24);
            if (duplicateSize != payloadSize)
                break;
            std::vector<uint8_t> payload(wal.begin() + static_cast<std::ptrdiff_t>(position + 20),
                                         wal.begin() + static_cast<std::ptrdiff_t>(position + 20 + payloadSize));
            if (Checksum(payload) != checksum)
                break;
            position += 20 + payloadSize;

            Wire::Reader reader(payload);
            WalKind kind{};
            MutationKey key;
            if (!reader.Read(kind) || !ReadMutationKey(reader, key))
                break;
            const auto identity = MutationIdentity(key);
            if (kind == WalKind::Intent)
            {
                uint16_t messageType = 0;
                std::string processId;
                int64_t oldProcessId = 0;
                uint64_t oldStartToken = 0;
                if (!reader.Read(messageType) || !reader.ReadString(processId, kMaximumProcessIdLength) ||
                    !reader.Read(oldProcessId) || !reader.Read(oldStartToken) || !reader.Finished())
                    break;
                pending[identity] = key;
            }
            else if (kind == WalKind::Commit && reader.Finished())
            {
                pending.erase(identity);
            }
            else
            {
                break;
            }
        }

        // Snapshot publication precedes Commit. A snapshot containing the key
        // proves the operation committed even if its trailing commit tore.
        for (const auto& mutation : recovered->mutations)
            pending.erase(MutationIdentity({mutation.clientInstance, mutation.sequence}));
        for (const auto& [identity, key] : pending)
            recovered->interruptedMutations.push_back(key);
        return recovered;
    }

    bool CompactOrchestrationJournal(const std::filesystem::path& path, const OrchestrationJournalState& state)
    {
        return WriteOrchestrationJournal(path, state) && DurableWrite(WalPath(path), {}, false);
    }
} // namespace Spark::Daemon
