/**
 * @file ServiceTests.cpp
 * @brief Focused protocol/security tests for daemon control-plane services.
 */

#include "CollaborationService.h"
#include "OrchestratorIdentity.h"
#include "OrchestrationService.h"

#include <cstdlib>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    int g_failures = 0;

#if defined(__linux__)
    int g_preExecIdentityDescriptor = -1;

    [[noreturn]] void AbortAfterDurableLaunchIdentity(int64_t processId)
    {
        const char* cursor = reinterpret_cast<const char*>(&processId);
        size_t remaining = sizeof(processId);
        while (remaining > 0)
        {
            const ssize_t written = ::write(g_preExecIdentityDescriptor, cursor, remaining);
            if (written > 0)
            {
                cursor += written;
                remaining -= static_cast<size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR)
                continue;
            _exit(84);
        }
        // Model power loss / SIGKILL semantics: no stack unwinding and no
        // OrchestrationService destructor to close the crash window for us.
        _exit(86);
    }
#endif

    void Check(bool condition, const char* description)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << description << '\n';
            ++g_failures;
        }
    }

    std::vector<uint8_t> CreatePayload(std::string_view id)
    {
        Spark::Daemon::Wire::Writer writer;
        Spark::Daemon::Wire::WriteVersion(writer);
        writer.WriteString(id, Spark::Daemon::kMaximumSessionIdLength);
        return writer.Take();
    }

    bool IsError(const Spark::Daemon::ServiceResponse& response)
    {
        return response.messageType == static_cast<uint16_t>(Spark::Daemon::ControlMessage::ErrorResponse);
    }

    void TestStrictProcessCodec()
    {
        Spark::Daemon::ProcessDefinition definition;
        definition.id = "world-1";
        definition.executable = "/bin/true";
        definition.workingDirectory = "/bin";
        definition.arguments = {"--example"};
        definition.restartPolicy = Spark::Daemon::RestartPolicy::OnFailure;
        definition.gracefulStopMilliseconds = 500;
        Spark::Daemon::MutationKey key{"test-client", 1};
        std::vector<uint8_t> payload;
        Check(Spark::Daemon::EncodeProcessDefinition(key, definition, payload), "process definition encodes");
        Spark::Daemon::MutationKey decodedKey;
        Spark::Daemon::ProcessDefinition decoded;
        Check(Spark::Daemon::DecodeProcessDefinition(payload, decodedKey, decoded), "process definition decodes");
        payload.push_back(0);
        Check(!Spark::Daemon::DecodeProcessDefinition(payload, decodedKey, decoded),
              "process decoder rejects trailing bytes");
        payload[0] = 2;
        Check(!Spark::Daemon::DecodeProcessDefinition(payload, decodedKey, decoded),
              "process decoder rejects unknown schema");
    }

    void TestCollaborationCapabilitiesAndLocks()
    {
        Spark::Daemon::CollaborationConfig config;
        config.maximumSessions = 1;
        config.maximumPeersPerSession = 2;
        config.maximumLocksPerSession = 1;
        config.maximumEditHistory = 1;
        Spark::Daemon::CollaborationService service(config);

        auto created = *service.HandleMessage(
            static_cast<uint16_t>(Spark::Daemon::CollaborationMessage::CreateSessionRequest), CreatePayload("scene"));
        Check(!IsError(created), "session creation succeeds");
        std::string sessionId;
        std::string administrationToken;
        Check(Spark::Daemon::DecodeSessionSecret(created.payload, sessionId, administrationToken),
              "create returns administration capability");
        Check(administrationToken.size() == Spark::Daemon::kCollaborationTokenLength,
              "administration token is 256 bits encoded as hex");

        std::vector<uint8_t> joinPayload;
        Spark::Daemon::EncodeJoinRequest("scene", "alice", joinPayload);
        auto joined = *service.HandleMessage(
            static_cast<uint16_t>(Spark::Daemon::CollaborationMessage::JoinSessionRequest), joinPayload);
        uint32_t peerId = 0;
        std::string token;
        Check(Spark::Daemon::DecodeJoinResponse(joined.payload, peerId, token), "join returns peer capability");

        Spark::Daemon::CollaborationAuth auth{"scene", peerId, token};
        std::vector<uint8_t> lockPayload;
        Spark::Daemon::EncodeAuthString(auth, "node-1", Spark::Daemon::kMaximumNodeIdLength, lockPayload);
        auto acquired = *service.HandleMessage(
            static_cast<uint16_t>(Spark::Daemon::CollaborationMessage::AcquireLockRequest), lockPayload);
        Check(!IsError(acquired), "authenticated peer acquires a lock");

        auth.token[0] = auth.token[0] == '0' ? '1' : '0';
        Spark::Daemon::EncodeAuthString(auth, "node-1", Spark::Daemon::kMaximumNodeIdLength, lockPayload);
        auto rejected = *service.HandleMessage(
            static_cast<uint16_t>(Spark::Daemon::CollaborationMessage::ReleaseLockRequest), lockPayload);
        Check(IsError(rejected), "forged peer token cannot release a lock");

        std::string wrongToken = administrationToken;
        wrongToken[0] = wrongToken[0] == '0' ? '1' : '0';
        std::vector<uint8_t> deletePayload;
        Spark::Daemon::EncodeSessionSecret("scene", wrongToken, deletePayload);
        auto deleteRejected = *service.HandleMessage(
            static_cast<uint16_t>(Spark::Daemon::CollaborationMessage::DeleteSessionRequest), deletePayload);
        Check(IsError(deleteRejected), "forged administration token cannot delete a session");
    }

    void TestCollaborationSnapshotByteBudget()
    {
        Spark::Daemon::CollaborationConfig config;
        config.maximumEditHistory = 100;
        config.maximumSnapshotBytes = 1024;
        Spark::Daemon::CollaborationService service(config);

        auto created = *service.HandleMessage(
            static_cast<uint16_t>(Spark::Daemon::CollaborationMessage::CreateSessionRequest), CreatePayload("bounded"));
        Check(!IsError(created), "bounded collaboration session creates");

        std::vector<uint8_t> payload;
        Spark::Daemon::EncodeJoinRequest("bounded", "alice", payload);
        auto joined = *service.HandleMessage(
            static_cast<uint16_t>(Spark::Daemon::CollaborationMessage::JoinSessionRequest), payload);
        uint32_t peerId = 0;
        std::string token;
        Check(Spark::Daemon::DecodeJoinResponse(joined.payload, peerId, token), "bounded collaboration peer joins");
        Spark::Daemon::CollaborationAuth auth{"bounded", peerId, token};

        Spark::Daemon::EncodeAuthString(auth, "node", Spark::Daemon::kMaximumNodeIdLength, payload);
        auto acquired = *service.HandleMessage(
            static_cast<uint16_t>(Spark::Daemon::CollaborationMessage::AcquireLockRequest), payload);
        Check(!IsError(acquired), "bounded collaboration lock request succeeds");

        const std::string editPayload(300, 'x');
        for (int i = 0; i < 10; ++i)
        {
            Spark::Daemon::EncodeEditRequest(auth, "node", editPayload, payload);
            auto submitted = *service.HandleMessage(
                static_cast<uint16_t>(Spark::Daemon::CollaborationMessage::SubmitEditRequest), payload);
            Check(!IsError(submitted), "new edit evicts old history to stay within the byte budget");
        }

        Spark::Daemon::EncodeCollaborationAuth(auth, payload);
        auto response = *service.HandleMessage(
            static_cast<uint16_t>(Spark::Daemon::CollaborationMessage::SnapshotRequest), payload);
        Check(!IsError(response), "byte-bounded collaboration snapshot remains encodable");
        Check(response.payload.size() <= config.maximumSnapshotBytes,
              "collaboration snapshot stays below its configured byte budget");
        Check(response.payload.size() < Spark::Daemon::kMaxPayloadSize,
              "collaboration snapshot stays below the daemon frame ceiling");

        Spark::Daemon::Wire::Reader reader(response.payload);
        std::string sessionId;
        uint64_t nextSequence = 0;
        uint32_t peerCount = 0;
        Check(Spark::Daemon::Wire::ReadVersion(reader) &&
                  reader.ReadString(sessionId, Spark::Daemon::kMaximumSessionIdLength) && reader.Read(nextSequence) &&
                  reader.Read(peerCount),
              "bounded collaboration snapshot header decodes");
        for (uint32_t i = 0; i < peerCount; ++i)
        {
            uint32_t id = 0;
            std::string name;
            std::string presence;
            Check(reader.Read(id) && reader.ReadString(name, Spark::Daemon::kMaximumPeerNameLength) &&
                      reader.ReadString(presence, Spark::Daemon::kMaximumPresenceLength),
                  "bounded snapshot peer decodes");
        }
        uint32_t lockCount = 0;
        Check(reader.Read(lockCount), "bounded collaboration lock count decodes");
        for (uint32_t i = 0; i < lockCount; ++i)
        {
            std::string nodeId;
            uint32_t owner = 0;
            Check(reader.ReadString(nodeId, Spark::Daemon::kMaximumNodeIdLength) && reader.Read(owner),
                  "bounded snapshot lock decodes");
        }
        uint32_t editCount = 0;
        Check(reader.Read(editCount), "bounded collaboration edit count decodes");
        uint64_t firstSequence = 0;
        for (uint32_t i = 0; i < editCount; ++i)
        {
            uint64_t sequence = 0;
            uint32_t author = 0;
            std::string nodeId;
            std::string edit;
            Check(reader.Read(sequence) && reader.Read(author) &&
                      reader.ReadString(nodeId, Spark::Daemon::kMaximumNodeIdLength) &&
                      reader.ReadString(edit, Spark::Daemon::kMaximumEditPayloadLength),
                  "bounded snapshot edit decodes");
            if (i == 0)
                firstSequence = sequence;
        }
        Check(reader.Finished(), "bounded collaboration snapshot has canonical length");
        Check(editCount > 0 && editCount < 10, "byte budget evicts only the oldest edits");
        Check(firstSequence == 11 - editCount && nextSequence == 11,
              "byte-budget eviction retains the newest contiguous edit suffix");
    }

    void TestSupervisorFailClosedConfiguration()
    {
        Spark::Daemon::OrchestrationConfig config;
        config.allowedExecutableRoots = {};
        Spark::Daemon::OrchestrationService service(config);
        Spark::Daemon::ProcessDefinition definition;
        definition.id = "blocked";
        definition.executable = "/bin/true";
        definition.workingDirectory = "/bin";
        definition.gracefulStopMilliseconds = 500;
        std::vector<uint8_t> payload;
        Spark::Daemon::EncodeProcessDefinition({"test-client", 1}, definition, payload);
        auto response =
            *service.HandleMessage(static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::DefineRequest), payload);
        Check(IsError(response), "supervisor without allow roots rejects definitions");
    }

    void TestSupervisorRevalidatesExecutableAtLaunch(const std::filesystem::path& executable,
                                                     const std::filesystem::path& scratch)
    {
        const auto allowed = scratch / "swap-allowed";
        const auto outside = scratch / "swap-outside";
        std::filesystem::create_directories(allowed);
        std::filesystem::create_directories(outside);
        const auto allowedExecutable = allowed / executable.filename();
        const auto outsideExecutable = outside / executable.filename();
        std::filesystem::copy_file(executable, allowedExecutable, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(executable, outsideExecutable, std::filesystem::copy_options::overwrite_existing);

        Spark::Daemon::OrchestrationConfig config;
        config.allowedExecutableRoots = {allowed};
        config.journalPath = scratch / "swap.state";
        Spark::Daemon::OrchestrationService service(config);
        Spark::Daemon::ProcessDefinition definition;
        definition.id = "swap";
        definition.executable = allowedExecutable.string();
        definition.workingDirectory = allowed.string();
        definition.arguments = {"--supervised-child"};
        definition.gracefulStopMilliseconds = 200;
        std::vector<uint8_t> payload;
        Spark::Daemon::EncodeProcessDefinition({"swap-client", 1}, definition, payload);
        const auto defined =
            *service.HandleMessage(static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::DefineRequest), payload);
        Check(!IsError(defined), "allow-root executable definition is accepted before a path swap");

        std::error_code swapError;
        std::filesystem::remove(allowedExecutable, swapError);
        swapError.clear();
        std::filesystem::create_symlink(outsideExecutable, allowedExecutable, swapError);
        if (!swapError)
        {
            Spark::Daemon::EncodeProcessMutation({"swap-client", 2}, "swap", payload);
            const auto started = *service.HandleMessage(
                static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::StartRequest), payload);
            Check(IsError(started), "launch-time canonicalization rejects executable swapped outside allow roots");
            const auto statuses = service.Snapshot();
            Check(statuses.size() == 1 && statuses.front().processId == 0,
                  "rejected swapped executable never creates a process");
        }
        else
        {
            std::cout << "[ INFO   ] orchestration path-swap test skipped: " << swapError.message() << '\n';
        }
    }

    void TestJournalTornTailAndStalePid(const std::filesystem::path& scratch, const std::filesystem::path& executable)
    {
        const auto journal = scratch / "recovery.state";
        Spark::Daemon::OrchestrationJournalState state;
        Spark::Daemon::JournalProcess process;
        process.definition.id = "stale";
        process.definition.executable = executable.string();
        process.definition.workingDirectory =
            std::filesystem::path(process.definition.executable).parent_path().string();
        process.definition.gracefulStopMilliseconds = 200;
        process.status.id = "stale";
        process.status.state = Spark::Daemon::SupervisedProcessState::Running;
        process.status.processId = 999999;
        process.status.processStartToken = 42;
        process.desiredRunning = false;
        const auto nowMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        process.crashTimestampsUnixMilliseconds = {nowMilliseconds - 1000, nowMilliseconds - 500};
        state.processes.push_back(process);
        state.mutations.push_back(
            {"recover-client", 7, {static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::StopResponse), {1, 0}}});
        Check(Spark::Daemon::CompactOrchestrationJournal(journal, state), "journal compacts atomically");

        Spark::Daemon::OrchestrationIntent interrupted{
            {"torn-client", 9},
            static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::StartRequest),
            "stale",
            999999,
            42};
        Check(Spark::Daemon::AppendOrchestrationIntent(journal, interrupted), "journal intent is durable");
        {
            std::ofstream torn(journal.string() + ".wal", std::ios::binary | std::ios::app);
            const std::array<uint8_t, 7> partial = {0x53, 0x57, 0x41, 0x4c, 0xff, 0xff, 0xff};
            torn.write(reinterpret_cast<const char*>(partial.data()), partial.size());
        }
        auto recovered = Spark::Daemon::RecoverOrchestrationJournal(journal, 16, 16);
        Check(recovered.has_value(), "torn WAL tail preserves prior durable records");
        Check(recovered && recovered->interruptedMutations.size() == 1,
              "uncommitted intent is surfaced for deterministic replay recovery");
        Check(recovered && recovered->processes.front().crashTimestampsUnixMilliseconds.size() == 2,
              "crash-loop timestamps survive journal recovery");

        Spark::Daemon::OrchestrationConfig config;
        config.allowedExecutableRoots = {executable.parent_path()};
        config.journalPath = journal;
        config.maximumCrashesPerMinute = 2;
        Spark::Daemon::OrchestrationService service(config);
        const auto statuses = service.Snapshot();
        Check(statuses.size() == 1 && statuses.front().processId == 0,
              "startup reconciliation rejects a stale PID/start-token pair");
        Check(statuses.size() == 1 && statuses.front().state == Spark::Daemon::SupervisedProcessState::Quarantined,
              "durable crash window restores quarantine across restart");
    }

    void TestJournalWriteBoundsPreservePublishedSnapshot(const std::filesystem::path& scratch)
    {
        const auto journal = scratch / "bounded.state";
        Spark::Daemon::OrchestrationJournalState knownGood;
        knownGood.mutations.push_back(
            {"bounded-client", 1, {static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::StopResponse), {1, 0}}});
        Check(Spark::Daemon::WriteOrchestrationJournal(journal, knownGood), "bounded journal baseline publishes");

        auto oversized = knownGood;
        oversized.mutations.front().sequence = 2;
        oversized.mutations.front().response.payload.resize(8 * 1024 * 1024);
        Check(!Spark::Daemon::WriteOrchestrationJournal(journal, oversized),
              "journal rejects an oversized whole snapshot before publication");
        Check(!std::filesystem::exists(journal.string() + ".tmp"),
              "oversized journal rejection does not create a temporary publication");
        const auto recovered = Spark::Daemon::LoadOrchestrationJournal(journal, 4, 4);
        Check(recovered && recovered->mutations.size() == 1 && recovered->mutations.front().sequence == 1,
              "oversized journal rejection preserves the prior readable snapshot");
    }

    void TestPersistentOrchestratorIdentity(const std::filesystem::path& scratch)
    {
        const auto identityPath = scratch / "operator" / "identity.state";
        std::string error;
        std::string client;
        {
            auto first = Spark::Daemon::OrchestratorIdentityLease::Acquire(identityPath, error);
            Check(first.has_value(), "orchestrator creates a locked persistent mutation identity");
            Check(std::filesystem::is_regular_file(identityPath.string() + ".lock"),
                  "orchestrator uses a separate stable lock file for atomic state replacement");
            if (first)
            {
                client = first->Key().clientInstance;
                Check(!client.empty() && first->Key().sequence == 1,
                      "first orchestrator identity lease starts at sequence one");
            }
        }
        {
            auto second = Spark::Daemon::OrchestratorIdentityLease::Acquire(identityPath, error);
            Check(second.has_value(), "orchestrator reopens its persistent mutation identity");
            Check(second && second->Key().clientInstance == client && second->Key().sequence == 2,
                  "separate orchestrator processes reuse one client and advance monotonically");
        }
    }

    void TestPreExecReleaseFailsClosedAfterAbruptDaemonDeath(const std::filesystem::path& executable,
                                                             const std::filesystem::path& scratch)
    {
#if defined(__linux__)
        int previousSubreaper = 0;
        if (::prctl(PR_GET_CHILD_SUBREAPER, &previousSubreaper) != 0 || ::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
        {
            Check(false, "pre-exec crash test configures child subreaping");
            return;
        }

        int identityPipe[2] = {-1, -1};
        if (::pipe(identityPipe) != 0)
        {
            Check(false, "pre-exec crash test creates identity pipe");
            (void)::prctl(PR_SET_CHILD_SUBREAPER, previousSubreaper);
            return;
        }

        const auto journal = scratch / "pre-exec-crash.state";
        const pid_t daemon = ::fork();
        if (daemon == 0)
        {
            ::close(identityPipe[0]);
            g_preExecIdentityDescriptor = identityPipe[1];

            Spark::Daemon::OrchestrationConfig config;
            config.allowedExecutableRoots = {executable.parent_path()};
            config.journalPath = journal;
            config.beforeExecReleaseForTesting = &AbortAfterDurableLaunchIdentity;
            Spark::Daemon::OrchestrationService service(config);

            Spark::Daemon::ProcessDefinition definition;
            definition.id = "pre-exec-crash";
            definition.executable = executable.string();
            definition.workingDirectory = executable.parent_path().string();
            definition.arguments = {"--supervised-child"};
            definition.gracefulStopMilliseconds = 200;

            std::vector<uint8_t> payload;
            if (!Spark::Daemon::EncodeProcessDefinition({"pre-exec-client", 1}, definition, payload))
                _exit(87);
            const auto defined = *service.HandleMessage(
                static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::DefineRequest), payload);
            if (IsError(defined) ||
                !Spark::Daemon::EncodeProcessMutation({"pre-exec-client", 2}, definition.id, payload))
                _exit(88);
            (void)service.HandleMessage(static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::StartRequest),
                                        payload);
            _exit(89); // The pre-release hook must terminate this daemon.
        }

        ::close(identityPipe[1]);
        if (daemon < 0)
        {
            ::close(identityPipe[0]);
            Check(false, "pre-exec crash test forks daemon process");
            (void)::prctl(PR_SET_CHILD_SUBREAPER, previousSubreaper);
            return;
        }

        int64_t supervisedProcessId = 0;
        char* cursor = reinterpret_cast<char*>(&supervisedProcessId);
        size_t remaining = sizeof(supervisedProcessId);
        while (remaining > 0)
        {
            const ssize_t received = ::read(identityPipe[0], cursor, remaining);
            if (received > 0)
            {
                cursor += received;
                remaining -= static_cast<size_t>(received);
                continue;
            }
            if (received < 0 && errno == EINTR)
                continue;
            break;
        }
        ::close(identityPipe[0]);

        int daemonStatus = 0;
        Check(::waitpid(daemon, &daemonStatus, 0) == daemon, "abrupt test daemon is reaped");
        Check(remaining == 0 && supervisedProcessId > 0,
              "durably published supervised identity reaches the test parent");
        Check(WIFEXITED(daemonStatus) && WEXITSTATUS(daemonStatus) == 86,
              "daemon exits exactly after durable identity and before exec release");

        const auto recovered = Spark::Daemon::RecoverOrchestrationJournal(journal, 16, 16);
        Check(recovered && recovered->processes.size() == 1,
              "pre-exec crash leaves a readable durable process snapshot");
        Check(recovered &&
                  recovered->processes.front().status.state == Spark::Daemon::SupervisedProcessState::Starting &&
                  recovered->processes.front().status.processId == supervisedProcessId &&
                  recovered->processes.front().status.processStartToken != 0,
              "durable snapshot identifies the exact child held before exec");
        Check(recovered && recovered->interruptedMutations.size() == 1,
              "pre-exec identity publication preserves the uncommitted start intent");

        int childStatus = 0;
        pid_t reaped = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do
        {
            reaped = ::waitpid(static_cast<pid_t>(supervisedProcessId), &childStatus, WNOHANG);
            if (reaped == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (reaped == 0 && std::chrono::steady_clock::now() < deadline);
        if (reaped == 0)
        {
            (void)::kill(-static_cast<pid_t>(supervisedProcessId), SIGKILL);
            reaped = ::waitpid(static_cast<pid_t>(supervisedProcessId), &childStatus, 0);
        }
        Check(reaped == supervisedProcessId, "pre-release child is reaped after daemon death");
        Check(reaped == supervisedProcessId && WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 125,
              "pre-release EOF exits the child without executing supervised code");
        (void)::prctl(PR_SET_CHILD_SUBREAPER, previousSubreaper);
#else
        (void)executable;
        (void)scratch;
#endif
    }

    void TestWindowsOrPosixLaunchAndDurableReplay(const std::filesystem::path& executable,
                                                  const std::filesystem::path& scratch)
    {
        Spark::Daemon::OrchestrationConfig config;
        config.allowedExecutableRoots = {executable.parent_path()};
        config.journalPath = scratch / "live.state";
        config.maximumRunningProcesses = 1;
        config.maximumGracefulStopMilliseconds = 1000;
        {
            Spark::Daemon::OrchestrationService service(config);
            Spark::Daemon::ProcessDefinition definition;
            definition.id = "child";
            definition.executable = executable.string();
            definition.workingDirectory = executable.parent_path().string();
            definition.arguments = {"--supervised-child"};
            definition.gracefulStopMilliseconds = 200;
            std::vector<uint8_t> payload;
            Spark::Daemon::EncodeProcessDefinition({"launch-client", 1}, definition, payload);
            auto defined = *service.HandleMessage(
                static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::DefineRequest), payload);
            Check(!IsError(defined), "allowlisted child definition is accepted");
            Spark::Daemon::EncodeProcessMutation({"launch-client", 2}, "child", payload);
            auto started = *service.HandleMessage(
                static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::StartRequest), payload);
            Check(!IsError(started), "allowlisted child launches");
            auto statuses = service.Snapshot();
            Check(statuses.size() == 1 && statuses.front().processId > 0 && statuses.front().processStartToken != 0,
                  "running child owns PID plus process creation token");
            Spark::Daemon::EncodeProcessMutation({"launch-client", 3}, "child", payload);
            auto stopped = *service.HandleMessage(
                static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::StopRequest), payload);
            Check(!IsError(stopped), "child stop enters graceful deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            statuses = service.Snapshot();
            Check(statuses.size() == 1 && statuses.front().state == Spark::Daemon::SupervisedProcessState::Stopped,
                  "operator-requested child stop is not classified as a failure");
            Check(statuses.size() == 1 && statuses.front().crashLoopCount == 0,
                  "operator-requested child stop does not pollute crash-loop accounting");
        }
        {
            Spark::Daemon::OrchestrationService recovered(config);
            std::vector<uint8_t> payload;
            Spark::Daemon::EncodeProcessMutation({"launch-client", 3}, "child", payload);
            auto replay = *recovered.HandleMessage(
                static_cast<uint16_t>(Spark::Daemon::OrchestrationMessage::StopRequest), payload);
            Check(!IsError(replay), "idempotent response survives service restart");
            auto statuses = recovered.Snapshot();
            Check(statuses.size() == 1 && statuses.front().processId == 0,
                  "restart reconciliation does not retain a stale PID");
        }
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--supervised-child")
    {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return EXIT_SUCCESS;
    }
    const auto executable = std::filesystem::canonical(argv[0]);
    const auto scratch =
        std::filesystem::temp_directory_path() /
        ("spark-daemon-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(scratch);
    TestStrictProcessCodec();
    TestCollaborationCapabilitiesAndLocks();
    TestCollaborationSnapshotByteBudget();
    TestSupervisorFailClosedConfiguration();
    TestSupervisorRevalidatesExecutableAtLaunch(executable, scratch);
    TestJournalTornTailAndStalePid(scratch, executable);
    TestJournalWriteBoundsPreservePublishedSnapshot(scratch);
    TestPersistentOrchestratorIdentity(scratch);
    TestPreExecReleaseFailsClosedAfterAbruptDaemonDeath(executable, scratch);
    TestWindowsOrPosixLaunchAndDurableReplay(executable, scratch);
    std::error_code cleanupError;
    std::filesystem::remove_all(scratch, cleanupError);
    if (g_failures != 0)
        return EXIT_FAILURE;
    std::cout << "SparkDaemon service tests passed\n";
    return EXIT_SUCCESS;
}
