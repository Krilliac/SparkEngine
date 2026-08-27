/**
 * @file OrchestrationService.h
 * @brief Bounded, allowlisted child-process supervisor hosted by SparkDaemon.
 */

#pragma once

#include "OrchestrationProtocol.h"
#include "OrchestrationJournal.h"
#include "ServiceBase.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Spark::Daemon
{
    struct OrchestrationConfig
    {
        std::vector<std::filesystem::path> allowedExecutableRoots;
        size_t maximumDefinitions = 64;
        size_t maximumRunningProcesses = 16;
        size_t maximumClientInstances = 1024;
        uint32_t maximumGracefulStopMilliseconds = 60'000;
        uint32_t restartBackoffMilliseconds = 500;
        uint32_t maximumCrashesPerMinute = 5;
        std::filesystem::path journalPath;
#if !defined(_WIN32)
        // Test seam invoked after a launch identity is durably published but
        // before the waiting child is released into exec. Production leaves
        // this null; tests use it to model an abrupt daemon death at the exact
        // crash boundary without adding a command-line or environment hook.
        void (*beforeExecReleaseForTesting)(int64_t processId) = nullptr;
#endif
    };

    /**
     * @brief Owns and supervises explicitly defined child processes.
     *
     * Ownership: this service exclusively owns every launched child process.
     * Threading: HandleMessage and Snapshot are any-thread/thread-safe. A single
     * monitor thread performs waitpid, deadline escalation, and restart policy.
     * Security: launches never invoke a shell; executable and working-directory
     * paths must resolve beneath an administrator-provided canonical allow root.
     * Lifecycle: non-hot-reloadable; destruction gracefully stops every child.
     */
    class OrchestrationService final : public ServiceBase
    {
      public:
        explicit OrchestrationService(OrchestrationConfig config);
        ~OrchestrationService() override;

        OrchestrationService(const OrchestrationService&) = delete;
        OrchestrationService& operator=(const OrchestrationService&) = delete;

        [[nodiscard]] ServiceId GetServiceId() const noexcept override { return ServiceId::Orchestration; }
        [[nodiscard]] const char* GetName() const noexcept override { return "orchestration"; }
        std::optional<ServiceResponse> HandleMessage(uint16_t messageType,
                                                     const std::vector<uint8_t>& payload) override;

        [[nodiscard]] std::vector<ProcessStatus> Snapshot() const;

      private:
        struct Record
        {
            ProcessDefinition definition;
            ProcessStatus status;
            bool desiredRunning = false;
            bool restartAfterStop = false;
            std::chrono::steady_clock::time_point stopDeadline{};
            std::chrono::steady_clock::time_point restartAt{};
            std::deque<std::chrono::system_clock::time_point> recentCrashes;
            std::intptr_t nativeProcessHandle = 0;
            std::intptr_t nativeJobHandle = 0;
        };

        struct ClientMutationState
        {
            uint64_t lastSequence = 0;
            ServiceResponse lastResponse;
        };

        ServiceResponse Define(const std::vector<uint8_t>& payload);
        ServiceResponse Undefine(const std::vector<uint8_t>& payload);
        ServiceResponse Start(const std::vector<uint8_t>& payload);
        ServiceResponse Stop(const std::vector<uint8_t>& payload, bool draining);
        ServiceResponse Restart(const std::vector<uint8_t>& payload);
        ServiceResponse Status(const std::vector<uint8_t>& payload) const;
        ServiceResponse List(const std::vector<uint8_t>& payload) const;
        ServiceResponse MakeError(std::string message) const;
        ServiceResponse MakeAck(OrchestrationMessage response) const;

        bool NormalizeDefinition(ProcessDefinition& definition, std::string& error) const;
        std::optional<ServiceResponse> ReplayOrRejectMutationLocked(const MutationKey& key) const;
        std::optional<ServiceResponse> BeginMutationLocked(const MutationKey& key, uint16_t messageType,
                                                           std::string_view processId);
        ServiceResponse RememberMutationLocked(const MutationKey& key, ServiceResponse response);
        [[nodiscard]] OrchestrationJournalState MakeJournalStateLocked() const;
        void LoadJournalLocked();
        bool PersistSystemStateLocked();
        bool IsUnderAllowedRoot(const std::filesystem::path& path) const;
        bool LaunchLocked(Record& record, std::string& error);
        bool StillOwnsProcessLocked(const Record& record) const;
        static uint64_t ReadProcessStartToken(int64_t processId);
        void RequestStopLocked(Record& record, bool draining);
        void MonitorMain();
        void ReapAndSuperviseLocked(std::chrono::steady_clock::time_point now);
        void StopAll();
        [[nodiscard]] size_t RunningCountLocked() const;

        OrchestrationConfig m_config;
        std::vector<std::filesystem::path> m_allowedRoots;
        mutable std::mutex m_mutex;
        std::condition_variable m_wake;
        std::unordered_map<std::string, Record> m_records;
        mutable std::unordered_map<std::string, ClientMutationState> m_mutations;
        bool m_stopping = false;
        std::thread m_monitorThread;
    };
} // namespace Spark::Daemon
