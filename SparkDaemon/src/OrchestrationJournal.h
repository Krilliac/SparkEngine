/**
 * @file OrchestrationJournal.h
 * @brief Atomic persistent state for SparkDaemon process supervision.
 */

#pragma once

#include "OrchestrationProtocol.h"
#include "ServiceBase.h"

#include <filesystem>
#include <optional>

namespace Spark::Daemon
{
    struct JournalProcess
    {
        ProcessDefinition definition;
        ProcessStatus status;
        bool desiredRunning = false;
        std::vector<int64_t> crashTimestampsUnixMilliseconds;
    };

    struct JournalMutation
    {
        std::string clientInstance;
        uint64_t sequence = 0;
        ServiceResponse response;
    };

    struct OrchestrationJournalState
    {
        std::vector<JournalProcess> processes;
        std::vector<JournalMutation> mutations;
        std::vector<MutationKey> interruptedMutations;
    };

    struct OrchestrationIntent
    {
        MutationKey key;
        uint16_t messageType = 0;
        std::string processId;
        int64_t processIdBefore = 0;
        uint64_t processStartTokenBefore = 0;
    };

    [[nodiscard]] std::optional<OrchestrationJournalState> LoadOrchestrationJournal(const std::filesystem::path& path,
                                                                                    size_t maximumProcesses,
                                                                                    size_t maximumClients);

    [[nodiscard]] bool WriteOrchestrationJournal(const std::filesystem::path& path,
                                                 const OrchestrationJournalState& state);

    /// Durable WAL intent. Flushes to stable storage before returning success.
    [[nodiscard]] bool AppendOrchestrationIntent(const std::filesystem::path& path, const OrchestrationIntent& intent);

    /// Durable WAL commit containing the complete post-operation state.
    [[nodiscard]] bool AppendOrchestrationCommit(const std::filesystem::path& path, const MutationKey& key,
                                                 const OrchestrationJournalState& state);

    /// Load atomic snapshot then replay complete WAL records; torn tail is ignored.
    [[nodiscard]] std::optional<OrchestrationJournalState> RecoverOrchestrationJournal(
        const std::filesystem::path& path, size_t maximumProcesses, size_t maximumClients);

    /// Atomic snapshot + durable WAL truncation.
    [[nodiscard]] bool CompactOrchestrationJournal(const std::filesystem::path& path,
                                                   const OrchestrationJournalState& state);
} // namespace Spark::Daemon
