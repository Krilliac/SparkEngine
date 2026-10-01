/**
 * @file FuzzOrchestrationJournalProduction.cpp
 * @brief libc++-compiled production adapter for the SparkDaemon orchestration journal harness.
 *
 * OrchestrationService::LoadJournalLocked restores supervised processes from the
 * snapshot at OrchestrationConfig::journalPath and the write-ahead log beside it
 * through RecoverOrchestrationJournal. Input layout:
 *   byte 0      bit 0: write the snapshot file, bit 1: write the <snapshot>.wal file
 *   bytes 1..4  little-endian snapshot length (clamped to the bytes present)
 *   rest        snapshot bytes, then WAL bytes
 * A violated contract aborts so libFuzzer records a crash:
 *  - an accepted journal holds at most the configured process and client counts,
 *    at most 100 crash timestamps per process, unique process ids and unique,
 *    non-empty client instances with non-zero sequences (the writer serializes the
 *    service's keyed maps, so it can produce nothing else),
 *  - every crash timestamp and drain deadline the service converts to a
 *    std::chrono::system_clock time point is a non-negative Unix millisecond value
 *    that conversion can represent,
 *  - every interrupted mutation is a well-formed key reported once and absent from
 *    the snapshot's committed mutations,
 *  - an accepted snapshot is a fixed point of Write -> Load -> Write.
 */

#include "FuzzOrchestrationJournalProduction.h"

#include "OrchestrationJournal.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace Daemon = Spark::Daemon;

    constexpr std::size_t kMaxInputBytes = 256u * 1024u;
    constexpr std::size_t kHeaderBytes = 5u;
    // OrchestrationConfig defaults: maximumDefinitions and maximumClientInstances.
    constexpr std::size_t kMaximumProcesses = 64u;
    constexpr std::size_t kMaximumClients = 1024u;
    constexpr std::size_t kMaximumCrashTimestamps = 100u;
    std::atomic<std::uint64_t> s_directoryCounter{0};

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzOrchestrationJournal: %s\n", what);
        std::abort();
    }

    /// A fresh directory for one input; removed with its contents on scope exit.
    class ScopedDirectory
    {
      public:
        ScopedDirectory()
        {
            const auto suffix = s_directoryCounter.fetch_add(1, std::memory_order_relaxed);
            m_path = fs::temp_directory_path() /
                     ("spark-fuzz-orchestration-journal-" + std::to_string(getpid()) + "-" + std::to_string(suffix));
            std::error_code error;
            fs::remove_all(m_path, error);
            m_created = fs::create_directory(m_path, error) && !error;
        }
        ~ScopedDirectory()
        {
            std::error_code ignored;
            fs::remove_all(m_path, ignored);
        }
        ScopedDirectory(const ScopedDirectory&) = delete;
        ScopedDirectory& operator=(const ScopedDirectory&) = delete;

        bool Created() const { return m_created; }
        const fs::path& Path() const { return m_path; }

      private:
        fs::path m_path;
        bool m_created = false;
    };

    bool WriteBytes(const fs::path& path, const std::uint8_t* data, std::size_t size)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (size != 0)
        {
            file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
        file.close();
        return static_cast<bool>(file);
    }

    std::vector<std::uint8_t> ReadBytes(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    /// Whether std::chrono::system_clock::time_point{std::chrono::milliseconds(value)}, the conversion
    /// OrchestrationService applies to persisted Unix times, is defined and not before the epoch.
    bool IsRepresentableUnixMilliseconds(std::int64_t value)
    {
        using Milliseconds = std::chrono::milliseconds;
        constexpr auto kMaximum =
            std::chrono::duration_cast<Milliseconds>(std::chrono::system_clock::duration::max()).count();
        return value >= 0 && value <= kMaximum;
    }

    std::string Identity(const std::string& clientInstance, std::uint64_t sequence)
    {
        return clientInstance + "\n" + std::to_string(sequence);
    }

    void CheckAccepted(const Daemon::OrchestrationJournalState& state)
    {
        if (state.processes.size() > kMaximumProcesses)
        {
            InvariantFailure("accepted more processes than the configured maximum");
        }
        if (state.mutations.size() > kMaximumClients)
        {
            InvariantFailure("accepted more client mutations than the configured maximum");
        }
        std::set<std::string> processIds;
        for (const Daemon::JournalProcess& process : state.processes)
        {
            if (process.crashTimestampsUnixMilliseconds.size() > kMaximumCrashTimestamps)
            {
                InvariantFailure("accepted more than 100 crash timestamps for one process");
            }
            for (const std::int64_t timestamp : process.crashTimestampsUnixMilliseconds)
            {
                if (!IsRepresentableUnixMilliseconds(timestamp))
                {
                    InvariantFailure("accepted a crash timestamp system_clock cannot represent");
                }
            }
            if (!IsRepresentableUnixMilliseconds(process.status.drainDeadlineUnixMilliseconds))
            {
                InvariantFailure("accepted a drain deadline system_clock cannot represent");
            }
            if (!processIds.insert(process.definition.id).second)
            {
                InvariantFailure("accepted two snapshot records for one process id");
            }
        }
        std::set<std::string> clients;
        std::set<std::string> committed;
        for (const Daemon::JournalMutation& mutation : state.mutations)
        {
            if (mutation.clientInstance.empty() ||
                mutation.clientInstance.size() > Daemon::kMaximumClientInstanceLength || mutation.sequence == 0)
            {
                InvariantFailure("accepted a committed mutation with a malformed client key");
            }
            if (!clients.insert(mutation.clientInstance).second)
            {
                InvariantFailure("accepted two committed mutations for one client instance");
            }
            committed.insert(Identity(mutation.clientInstance, mutation.sequence));
        }
        std::set<std::string> interrupted;
        for (const Daemon::MutationKey& key : state.interruptedMutations)
        {
            if (key.clientInstance.empty() || key.clientInstance.size() > Daemon::kMaximumClientInstanceLength ||
                key.sequence == 0)
            {
                InvariantFailure("reported a malformed interrupted mutation key");
            }
            const std::string identity = Identity(key.clientInstance, key.sequence);
            if (!interrupted.insert(identity).second)
            {
                InvariantFailure("reported one interrupted mutation twice");
            }
            if (committed.contains(identity))
            {
                InvariantFailure("reported a mutation the snapshot committed as interrupted");
            }
        }
    }

    /// Write -> Load -> Write must reproduce the first write byte for byte.
    void CheckFixedPoint(const fs::path& directory, Daemon::OrchestrationJournalState state)
    {
        state.interruptedMutations.clear();
        const fs::path first = directory / "first.journal";
        const fs::path second = directory / "second.journal";
        if (!Daemon::WriteOrchestrationJournal(first, state))
        {
            InvariantFailure("an accepted snapshot cannot be written back");
        }
        const auto reloaded = Daemon::LoadOrchestrationJournal(first, kMaximumProcesses, kMaximumClients);
        if (!reloaded)
        {
            InvariantFailure("the writer produced a snapshot the loader rejects");
        }
        if (!Daemon::WriteOrchestrationJournal(second, *reloaded))
        {
            InvariantFailure("a reloaded snapshot cannot be written back");
        }
        if (ReadBytes(first) != ReadBytes(second))
        {
            InvariantFailure("Write -> Load -> Write changed the snapshot");
        }
    }
} // namespace

extern "C" int SparkFuzzRecoverOrchestrationJournal(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0) || size < kHeaderBytes)
    {
        return 0;
    }
    const std::uint8_t flags = data[0];
    const std::size_t declared = static_cast<std::size_t>(data[1]) | (static_cast<std::size_t>(data[2]) << 8) |
                                 (static_cast<std::size_t>(data[3]) << 16) | (static_cast<std::size_t>(data[4]) << 24);
    const std::uint8_t* body = data + kHeaderBytes;
    const std::size_t bodySize = size - kHeaderBytes;
    const std::size_t snapshotSize = declared < bodySize ? declared : bodySize;

    ScopedDirectory directory;
    if (!directory.Created())
    {
        return 0;
    }
    const fs::path journal = directory.Path() / "orchestration.journal";
    fs::path wal = journal;
    wal += ".wal";
    if ((flags & 1u) != 0 && !WriteBytes(journal, body, snapshotSize))
    {
        return 0;
    }
    if ((flags & 2u) != 0 && !WriteBytes(wal, body + snapshotSize, bodySize - snapshotSize))
    {
        return 0;
    }

    const auto recovered = Daemon::RecoverOrchestrationJournal(journal, kMaximumProcesses, kMaximumClients);
    if (!recovered)
    {
        return 0;
    }
    CheckAccepted(*recovered);
    CheckFixedPoint(directory.Path(), *recovered);
    return 0;
}
