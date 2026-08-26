/**
 * @file OrchestrationService.cpp
 * @brief POSIX supervised-process implementation for SparkDaemon.
 */

#include "OrchestrationService.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

namespace Spark::Daemon
{
    namespace
    {
        bool IsValidId(std::string_view id)
        {
            if (id.empty() || id.size() > kMaximumProcessIdLength)
                return false;
            return std::all_of(id.begin(), id.end(), [](unsigned char c)
                               { return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.'; });
        }

        bool IsActive(SupervisedProcessState state)
        {
            return state == SupervisedProcessState::Starting || state == SupervisedProcessState::Running ||
                   state == SupervisedProcessState::Draining || state == SupervisedProcessState::Stopping;
        }

#if !defined(_WIN32)
        void WriteChildError(int descriptor, int errorCode) noexcept
        {
            const char* cursor = reinterpret_cast<const char*>(&errorCode);
            size_t remaining = sizeof(errorCode);
            while (remaining > 0)
            {
                const ssize_t written = ::write(descriptor, cursor, remaining);
                if (written > 0)
                {
                    cursor += written;
                    remaining -= static_cast<size_t>(written);
                    continue;
                }
                if (written < 0 && errno == EINTR)
                    continue;
                break;
            }
        }
#endif

#if defined(_WIN32)
        std::filesystem::path Utf8Path(std::string_view value)
        {
            const auto* bytes = reinterpret_cast<const char8_t*>(value.data());
            return std::filesystem::path{std::u8string{bytes, value.size()}};
        }

        std::wstring Utf8ToWide(std::string_view value)
        {
            if (value.empty())
                return {};
            const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                                     static_cast<int>(value.size()), nullptr, 0);
            if (length <= 0)
                return {};
            std::wstring result(static_cast<size_t>(length), L'\0');
            if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                      result.data(), length) != length)
                return {};
            return result;
        }

        std::wstring QuoteWindowsArgument(std::wstring_view value)
        {
            if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
                return std::wstring(value);
            std::wstring result = L"\"";
            size_t backslashes = 0;
            for (wchar_t character : value)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == L'\"')
                {
                    result.append(backslashes * 2 + 1, L'\\');
                    result.push_back(character);
                    backslashes = 0;
                    continue;
                }
                result.append(backslashes, L'\\');
                backslashes = 0;
                result.push_back(character);
            }
            result.append(backslashes * 2, L'\\');
            result.push_back(L'\"');
            return result;
        }
#endif
    } // namespace

    OrchestrationService::OrchestrationService(OrchestrationConfig config) : m_config(std::move(config))
    {
        m_config.maximumDefinitions = std::clamp<size_t>(m_config.maximumDefinitions, 1, 1024);
        m_config.maximumRunningProcesses = std::clamp<size_t>(m_config.maximumRunningProcesses, 1, 256);
        m_config.maximumClientInstances = std::clamp<size_t>(m_config.maximumClientInstances, 1, 65'536);
        m_config.maximumGracefulStopMilliseconds =
            std::clamp<uint32_t>(m_config.maximumGracefulStopMilliseconds, 100, 300'000);
        m_config.restartBackoffMilliseconds = std::clamp<uint32_t>(m_config.restartBackoffMilliseconds, 100, 60'000);
        m_config.maximumCrashesPerMinute = std::clamp<uint32_t>(m_config.maximumCrashesPerMinute, 1, 100);

        for (const auto& root : m_config.allowedExecutableRoots)
        {
            std::error_code error;
            auto canonical = std::filesystem::canonical(root, error);
            if (!error && std::filesystem::is_directory(canonical, error))
                m_allowedRoots.push_back(std::move(canonical));
        }
        LoadJournalLocked();
        m_monitorThread = std::thread(&OrchestrationService::MonitorMain, this);
    }

    OrchestrationService::~OrchestrationService()
    {
        StopAll();
    }

    std::optional<ServiceResponse> OrchestrationService::HandleMessage(uint16_t messageType,
                                                                       const std::vector<uint8_t>& payload)
    {
        switch (static_cast<OrchestrationMessage>(messageType))
        {
        case OrchestrationMessage::DefineRequest:
            return Define(payload);
        case OrchestrationMessage::UndefineRequest:
            return Undefine(payload);
        case OrchestrationMessage::StartRequest:
            return Start(payload);
        case OrchestrationMessage::StopRequest:
            return Stop(payload, false);
        case OrchestrationMessage::DrainRequest:
            return Stop(payload, true);
        case OrchestrationMessage::RestartRequest:
            return Restart(payload);
        case OrchestrationMessage::StatusRequest:
            return Status(payload);
        case OrchestrationMessage::ListRequest:
            return List(payload);
        default:
            return MakeError("unsupported orchestration message");
        }
    }

    ServiceResponse OrchestrationService::Define(const std::vector<uint8_t>& payload)
    {
        MutationKey key;
        ProcessDefinition definition;
        if (!DecodeProcessDefinition(payload, key, definition))
            return MakeError("malformed or unsupported process definition");
        std::string error;
        if (!NormalizeDefinition(definition, error))
            return MakeError(std::move(error));

        std::lock_guard lock(m_mutex);
        if (auto replay =
                BeginMutationLocked(key, static_cast<uint16_t>(OrchestrationMessage::DefineRequest), definition.id))
            return *replay;
        auto existing = m_records.find(definition.id);
        if (existing != m_records.end() && IsActive(existing->second.status.state))
            return RememberMutationLocked(key, MakeError("cannot replace a running process definition"));
        if (existing == m_records.end() && m_records.size() >= m_config.maximumDefinitions)
            return RememberMutationLocked(key, MakeError("process definition limit reached"));

        Record record;
        record.status.id = definition.id;
        record.definition = std::move(definition);
        m_records.insert_or_assign(record.status.id, std::move(record));
        return RememberMutationLocked(key, MakeAck(OrchestrationMessage::DefineResponse));
    }

    ServiceResponse OrchestrationService::Undefine(const std::vector<uint8_t>& payload)
    {
        MutationKey key;
        std::string id;
        if (!DecodeProcessMutation(payload, key, id))
            return MakeError("malformed undefine request");
        std::lock_guard lock(m_mutex);
        if (auto replay = BeginMutationLocked(key, static_cast<uint16_t>(OrchestrationMessage::UndefineRequest), id))
            return *replay;
        auto it = m_records.find(id);
        if (it == m_records.end())
            return RememberMutationLocked(key, MakeError("unknown process definition"));
        if (IsActive(it->second.status.state) || it->second.status.state == SupervisedProcessState::Backoff)
            return RememberMutationLocked(key, MakeError("cannot undefine an active process"));
        m_records.erase(it);
        return RememberMutationLocked(key, MakeAck(OrchestrationMessage::UndefineResponse));
    }

    ServiceResponse OrchestrationService::Start(const std::vector<uint8_t>& payload)
    {
        MutationKey key;
        std::string id;
        if (!DecodeProcessMutation(payload, key, id))
            return MakeError("malformed start request");
        std::lock_guard lock(m_mutex);
        if (auto replay = BeginMutationLocked(key, static_cast<uint16_t>(OrchestrationMessage::StartRequest), id))
            return *replay;
        auto it = m_records.find(id);
        if (it == m_records.end())
            return RememberMutationLocked(key, MakeError("unknown process definition"));
        if (IsActive(it->second.status.state))
            return RememberMutationLocked(key, MakeError("process is already active"));
        if (RunningCountLocked() >= m_config.maximumRunningProcesses)
            return RememberMutationLocked(key, MakeError("running process limit reached"));
        it->second.desiredRunning = true;
        std::string error;
        if (!LaunchLocked(it->second, error))
        {
            it->second.desiredRunning = false;
            return RememberMutationLocked(key, MakeError(std::move(error)));
        }
        return RememberMutationLocked(key, MakeAck(OrchestrationMessage::StartResponse));
    }

    ServiceResponse OrchestrationService::Stop(const std::vector<uint8_t>& payload, bool draining)
    {
        MutationKey key;
        std::string id;
        if (!DecodeProcessMutation(payload, key, id))
            return MakeError(draining ? "malformed drain request" : "malformed stop request");
        std::lock_guard lock(m_mutex);
        if (auto replay = BeginMutationLocked(key,
                                              static_cast<uint16_t>(draining ? OrchestrationMessage::DrainRequest
                                                                             : OrchestrationMessage::StopRequest),
                                              id))
            return *replay;
        auto it = m_records.find(id);
        if (it == m_records.end())
            return RememberMutationLocked(key, MakeError("unknown process definition"));
        it->second.desiredRunning = false;
        it->second.restartAfterStop = false;
        if (IsActive(it->second.status.state))
            RequestStopLocked(it->second, draining);
        else
            it->second.status.state = SupervisedProcessState::Stopped;
        m_wake.notify_all();
        return RememberMutationLocked(
            key, MakeAck(draining ? OrchestrationMessage::DrainResponse : OrchestrationMessage::StopResponse));
    }

    ServiceResponse OrchestrationService::Restart(const std::vector<uint8_t>& payload)
    {
        MutationKey key;
        std::string id;
        if (!DecodeProcessMutation(payload, key, id))
            return MakeError("malformed restart request");
        std::lock_guard lock(m_mutex);
        if (auto replay = BeginMutationLocked(key, static_cast<uint16_t>(OrchestrationMessage::RestartRequest), id))
            return *replay;
        auto it = m_records.find(id);
        if (it == m_records.end())
            return RememberMutationLocked(key, MakeError("unknown process definition"));
        it->second.desiredRunning = true;
        if (IsActive(it->second.status.state))
        {
            it->second.restartAfterStop = true;
            RequestStopLocked(it->second, false);
        }
        else
        {
            if (RunningCountLocked() >= m_config.maximumRunningProcesses)
                return RememberMutationLocked(key, MakeError("running process limit reached"));
            std::string error;
            if (!LaunchLocked(it->second, error))
                return RememberMutationLocked(key, MakeError(std::move(error)));
        }
        m_wake.notify_all();
        return RememberMutationLocked(key, MakeAck(OrchestrationMessage::RestartResponse));
    }

    ServiceResponse OrchestrationService::Status(const std::vector<uint8_t>& payload) const
    {
        std::string id;
        if (!DecodeProcessId(payload, id))
            return MakeError("malformed status request");
        std::lock_guard lock(m_mutex);
        auto it = m_records.find(id);
        if (it == m_records.end())
            return MakeError("unknown process definition");
        ServiceResponse response;
        response.messageType = static_cast<uint16_t>(OrchestrationMessage::StatusResponse);
        if (!EncodeProcessStatuses({it->second.status}, response.payload))
            return MakeError("could not encode process status");
        return response;
    }

    ServiceResponse OrchestrationService::List(const std::vector<uint8_t>& payload) const
    {
        Wire::Reader reader(payload);
        if (!Wire::ReadVersion(reader) || !reader.Finished())
            return MakeError("malformed list request");
        auto statuses = Snapshot();
        ServiceResponse response;
        response.messageType = static_cast<uint16_t>(OrchestrationMessage::ListResponse);
        if (!EncodeProcessStatuses(statuses, response.payload))
            return MakeError("could not encode process list");
        return response;
    }

    std::vector<ProcessStatus> OrchestrationService::Snapshot() const
    {
        std::lock_guard lock(m_mutex);
        std::vector<ProcessStatus> result;
        result.reserve(m_records.size());
        for (const auto& [id, record] : m_records)
            result.push_back(record.status);
        std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) { return lhs.id < rhs.id; });
        return result;
    }

    ServiceResponse OrchestrationService::MakeError(std::string message) const
    {
        ServiceResponse response;
        response.messageType = static_cast<uint16_t>(ControlMessage::ErrorResponse);
        response.payload.assign(message.begin(), message.end());
        return response;
    }

    ServiceResponse OrchestrationService::MakeAck(OrchestrationMessage responseType) const
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        return ServiceResponse{static_cast<uint16_t>(responseType), writer.Take()};
    }

    std::optional<ServiceResponse> OrchestrationService::ReplayOrRejectMutationLocked(const MutationKey& key) const
    {
        auto it = m_mutations.find(key.clientInstance);
        if (it == m_mutations.end())
        {
            if (m_mutations.size() >= m_config.maximumClientInstances)
                return MakeError("orchestration client-instance limit reached");
            return std::nullopt;
        }
        if (key.sequence == it->second.lastSequence)
            return it->second.lastResponse;
        if (key.sequence < it->second.lastSequence)
            return MakeError("stale orchestration mutation sequence");
        return std::nullopt;
    }

    std::optional<ServiceResponse> OrchestrationService::BeginMutationLocked(const MutationKey& key,
                                                                             uint16_t messageType,
                                                                             std::string_view processId)
    {
        if (auto replay = ReplayOrRejectMutationLocked(key))
            return replay;
        if (!m_config.journalPath.empty())
        {
            OrchestrationIntent intent;
            intent.key = key;
            intent.messageType = messageType;
            intent.processId = std::string(processId);
            auto record = m_records.find(intent.processId);
            if (record != m_records.end())
            {
                intent.processIdBefore = record->second.status.processId;
                intent.processStartTokenBefore = record->second.status.processStartToken;
            }
            if (!AppendOrchestrationIntent(m_config.journalPath, intent))
                return MakeError("could not durably record orchestration intent");
        }
        return std::nullopt;
    }

    ServiceResponse OrchestrationService::RememberMutationLocked(const MutationKey& key, ServiceResponse response)
    {
        auto& state = m_mutations[key.clientInstance];
        state.lastSequence = key.sequence;
        state.lastResponse = response;
        if (!m_config.journalPath.empty() &&
            !AppendOrchestrationCommit(m_config.journalPath, key, MakeJournalStateLocked()))
        {
            state.lastResponse = MakeError("orchestration operation completed but durable commit failed");
            return state.lastResponse;
        }
        return response;
    }

    OrchestrationJournalState OrchestrationService::MakeJournalStateLocked() const
    {
        OrchestrationJournalState state;
        state.processes.reserve(m_records.size());
        for (const auto& [id, record] : m_records)
        {
            JournalProcess process;
            process.definition = record.definition;
            process.status = record.status;
            process.desiredRunning = record.desiredRunning;
            process.crashTimestampsUnixMilliseconds.reserve(record.recentCrashes.size());
            for (const auto& timestamp : record.recentCrashes)
                process.crashTimestampsUnixMilliseconds.push_back(
                    std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count());
            state.processes.push_back(std::move(process));
        }
        state.mutations.reserve(m_mutations.size());
        for (const auto& [client, mutation] : m_mutations)
            state.mutations.push_back({client, mutation.lastSequence, mutation.lastResponse});
        return state;
    }

    void OrchestrationService::LoadJournalLocked()
    {
        if (m_config.journalPath.empty())
            return;
        auto recovered = RecoverOrchestrationJournal(m_config.journalPath, m_config.maximumDefinitions,
                                                     m_config.maximumClientInstances);
        if (!recovered)
            return; // Fail closed: malformed journal is never partially applied.
        for (auto& persisted : recovered->processes)
        {
            std::string error;
            if (!NormalizeDefinition(persisted.definition, error))
                continue;
            Record record;
            record.definition = std::move(persisted.definition);
            record.status = std::move(persisted.status);
            record.desiredRunning = persisted.desiredRunning;
            const auto wallNow = std::chrono::system_clock::now();
            for (int64_t timestamp : persisted.crashTimestampsUnixMilliseconds)
            {
                const auto crashTime = std::chrono::system_clock::time_point{std::chrono::milliseconds(timestamp)};
                if (crashTime <= wallNow && wallNow - crashTime <= std::chrono::minutes(1))
                    record.recentCrashes.push_back(crashTime);
            }
            record.status.crashLoopCount = static_cast<uint32_t>(record.recentCrashes.size());
            if (record.recentCrashes.size() >= m_config.maximumCrashesPerMinute)
            {
                record.desiredRunning = false;
                record.status.state = SupervisedProcessState::Quarantined;
            }
            bool reconciled = record.status.processId > 0 && StillOwnsProcessLocked(record);
#if defined(_WIN32)
            if (reconciled)
            {
                HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
                                               FALSE, static_cast<DWORD>(record.status.processId));
                HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!process || !job ||
                    !::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                    !::AssignProcessToJobObject(job, process))
                {
                    if (process)
                        ::CloseHandle(process);
                    if (job)
                        ::CloseHandle(job);
                    reconciled = false;
                }
                else
                {
                    record.nativeProcessHandle = reinterpret_cast<std::intptr_t>(process);
                    record.nativeJobHandle = reinterpret_cast<std::intptr_t>(job);
                }
            }
#endif
            if (reconciled)
            {
                if (record.status.state == SupervisedProcessState::Stopping ||
                    record.status.state == SupervisedProcessState::Draining)
                {
                    const auto deadline = std::chrono::system_clock::time_point{
                        std::chrono::milliseconds(record.status.drainDeadlineUnixMilliseconds)};
                    const auto remaining =
                        std::max(std::chrono::milliseconds(0), std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                   deadline - std::chrono::system_clock::now()));
                    record.stopDeadline = std::chrono::steady_clock::now() + remaining;
                }
                else
                {
                    record.status.state = SupervisedProcessState::Running;
                    record.status.health = ProcessHealth::Healthy;
                }
            }
            else
            {
                record.status.processId = 0;
                record.status.processStartToken = 0;
                record.status.health = ProcessHealth::Unknown;
                if (record.status.state != SupervisedProcessState::Quarantined)
                    record.status.state =
                        record.desiredRunning ? SupervisedProcessState::Backoff : SupervisedProcessState::Stopped;
                record.restartAt = std::chrono::steady_clock::now();
            }
            m_records.emplace(record.definition.id, std::move(record));
        }
        for (auto& mutation : recovered->mutations)
            m_mutations.emplace(std::move(mutation.clientInstance),
                                ClientMutationState{mutation.sequence, std::move(mutation.response)});
        for (const auto& interrupted : recovered->interruptedMutations)
        {
            auto& mutation = m_mutations[interrupted.clientInstance];
            if (interrupted.sequence >= mutation.lastSequence)
            {
                mutation.lastSequence = interrupted.sequence;
                mutation.lastResponse = MakeError("prior orchestration mutation was interrupted and reconciled");
            }
        }
        (void)CompactOrchestrationJournal(m_config.journalPath, MakeJournalStateLocked());
    }

    bool OrchestrationService::PersistSystemStateLocked()
    {
        return m_config.journalPath.empty() ||
               CompactOrchestrationJournal(m_config.journalPath, MakeJournalStateLocked());
    }

    bool OrchestrationService::NormalizeDefinition(ProcessDefinition& definition, std::string& error) const
    {
        if (!IsValidId(definition.id))
        {
            error = "process id must use 1-64 alphanumeric, dot, dash, or underscore characters";
            return false;
        }
        if (m_allowedRoots.empty())
        {
            error = "orchestration has no configured executable allow roots";
            return false;
        }
        if (definition.gracefulStopMilliseconds == 0 ||
            definition.gracefulStopMilliseconds > m_config.maximumGracefulStopMilliseconds)
        {
            error = "graceful stop timeout is outside the configured bound";
            return false;
        }
        std::error_code pathError;
#if defined(_WIN32)
        auto executable = std::filesystem::canonical(Utf8Path(definition.executable), pathError);
#else
        auto executable = std::filesystem::canonical(definition.executable, pathError);
#endif
        if (pathError || !std::filesystem::is_regular_file(executable, pathError) || !IsUnderAllowedRoot(executable))
        {
            error = "executable is missing, not a regular file, or outside an allow root";
            return false;
        }
#if !defined(_WIN32)
        auto permissions = std::filesystem::status(executable, pathError).permissions();
        constexpr auto executeBits = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec;
        if (pathError || (permissions & executeBits) == std::filesystem::perms::none)
        {
            error = "executable does not have an execute permission bit";
            return false;
        }
#endif
        auto working = definition.workingDirectory.empty() ? executable.parent_path()
                                                           : std::filesystem::canonical(
#if defined(_WIN32)
                                                                 Utf8Path(definition.workingDirectory), pathError);
#else
                                                                 definition.workingDirectory, pathError);
#endif
        if (pathError || !std::filesystem::is_directory(working, pathError) || !IsUnderAllowedRoot(working))
        {
            error = "working directory is missing or outside an allow root";
            return false;
        }
#if defined(_WIN32)
        const auto executableUtf8 = executable.u8string();
        const auto workingUtf8 = working.u8string();
        definition.executable.assign(executableUtf8.begin(), executableUtf8.end());
        definition.workingDirectory.assign(workingUtf8.begin(), workingUtf8.end());
#else
        definition.executable = executable.string();
        definition.workingDirectory = working.string();
#endif
        return true;
    }

    bool OrchestrationService::IsUnderAllowedRoot(const std::filesystem::path& path) const
    {
        for (const auto& root : m_allowedRoots)
        {
            std::error_code error;
            auto relative = std::filesystem::relative(path, root, error);
            if (error || relative.empty())
                continue;
            auto first = relative.begin();
            if (!relative.is_absolute() && first != relative.end() && *first != "..")
                return true;
        }
        return false;
    }

    bool OrchestrationService::LaunchLocked(Record& record, std::string& error)
    {
#if defined(_WIN32)
        const auto executable = Utf8Path(record.definition.executable).wstring();
        const auto workingDirectory = Utf8Path(record.definition.workingDirectory).wstring();
        std::wstring commandLine = QuoteWindowsArgument(executable);
        for (const auto& argument : record.definition.arguments)
        {
            const auto wideArgument = Utf8ToWide(argument);
            if (wideArgument.empty() && !argument.empty())
            {
                error = "process argument is not valid UTF-8";
                return false;
            }
            commandLine.push_back(L' ');
            commandLine += QuoteWindowsArgument(wideArgument);
        }
        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');

        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        if (!job)
        {
            error = "CreateJobObjectW failed";
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        {
            ::CloseHandle(job);
            error = "SetInformationJobObject failed";
            return false;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        record.status.state = SupervisedProcessState::Starting;
        const BOOL launched = ::CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                                               CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                               nullptr, workingDirectory.c_str(), &startup, &process);
        if (!launched)
        {
            ::CloseHandle(job);
            record.status.state = SupervisedProcessState::Failed;
            error = "CreateProcessW failed with error " + std::to_string(::GetLastError());
            return false;
        }
        if (!::AssignProcessToJobObject(job, process.hProcess))
        {
            const DWORD assignError = ::GetLastError();
            ::TerminateProcess(process.hProcess, 126);
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
            ::CloseHandle(job);
            record.status.state = SupervisedProcessState::Failed;
            error = "AssignProcessToJobObject failed with error " + std::to_string(assignError);
            return false;
        }
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!::GetProcessTimes(process.hProcess, &creation, &exit, &kernel, &user))
        {
            ::TerminateJobObject(job, 126);
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
            ::CloseHandle(job);
            record.status.state = SupervisedProcessState::Failed;
            error = "GetProcessTimes failed";
            return false;
        }
        if (::ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        {
            const DWORD resumeError = ::GetLastError();
            ::TerminateJobObject(job, 126);
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
            ::CloseHandle(job);
            record.status.state = SupervisedProcessState::Failed;
            error = "ResumeThread failed with error " + std::to_string(resumeError);
            return false;
        }
        ::CloseHandle(process.hThread);
        ULARGE_INTEGER creationValue{};
        creationValue.LowPart = creation.dwLowDateTime;
        creationValue.HighPart = creation.dwHighDateTime;
        record.nativeProcessHandle = reinterpret_cast<std::intptr_t>(process.hProcess);
        record.nativeJobHandle = reinterpret_cast<std::intptr_t>(job);
        record.status.processId = static_cast<int64_t>(process.dwProcessId);
        record.status.processStartToken = creationValue.QuadPart;
        record.status.state = SupervisedProcessState::Running;
        record.status.health = ProcessHealth::Healthy;
        record.status.exitCode = 0;
        record.status.drainDeadlineUnixMilliseconds = 0;
        record.stopDeadline = {};
        return true;
#else
        int errorPipe[2] = {-1, -1};
        if (::pipe(errorPipe) != 0)
        {
            error = std::string("pipe failed: ") + std::strerror(errno);
            return false;
        }
        ::fcntl(errorPipe[1], F_SETFD, FD_CLOEXEC);

        std::vector<char*> argv;
        argv.reserve(record.definition.arguments.size() + 2);
        argv.push_back(record.definition.executable.data());
        for (auto& argument : record.definition.arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);

        record.status.state = SupervisedProcessState::Starting;
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            ::close(errorPipe[0]);
            ::setpgid(0, 0);
#if defined(__linux__)
            // Close the fork->journal-commit crash window: a daemon crash
            // cannot strand a child whose PID was never durably recorded.
            (void)::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() == 1)
                _exit(125);
#endif
            if (::chdir(record.definition.workingDirectory.c_str()) != 0)
            {
                int childError = errno;
                WriteChildError(errorPipe[1], childError);
                _exit(126);
            }
            ::execv(record.definition.executable.c_str(), argv.data());
            int childError = errno;
            WriteChildError(errorPipe[1], childError);
            _exit(127);
        }

        ::close(errorPipe[1]);
        if (pid < 0)
        {
            ::close(errorPipe[0]);
            record.status.state = SupervisedProcessState::Failed;
            error = std::string("fork failed: ") + std::strerror(errno);
            return false;
        }

        int childError = 0;
        const ssize_t received = ::read(errorPipe[0], &childError, sizeof(childError));
        ::close(errorPipe[0]);
        if (received > 0)
        {
            (void)::waitpid(pid, nullptr, 0);
            record.status.state = SupervisedProcessState::Failed;
            record.status.health = ProcessHealth::Unhealthy;
            error = std::string("exec failed: ") + std::strerror(childError);
            return false;
        }

        record.status.processId = static_cast<int64_t>(pid);
        record.status.processStartToken = ReadProcessStartToken(record.status.processId);
        if (record.status.processStartToken == 0)
        {
            (void)::kill(-pid, SIGKILL);
            (void)::waitpid(pid, nullptr, 0);
            record.status.processId = 0;
            record.status.state = SupervisedProcessState::Failed;
            error = "could not establish child process start token";
            return false;
        }
#if !defined(__linux__)
        // A live parent-child relationship is the only portable identity
        // primitive available on generic POSIX. This in-memory marker is
        // intentionally not reconstructed from the journal after restart.
        record.nativeProcessHandle = static_cast<std::intptr_t>(pid);
#endif
        record.status.state = SupervisedProcessState::Running;
        record.status.health = ProcessHealth::Healthy;
        record.status.exitCode = 0;
        record.status.drainDeadlineUnixMilliseconds = 0;
        record.stopDeadline = {};
        return true;
#endif
    }

    void OrchestrationService::RequestStopLocked(Record& record, bool draining)
    {
#if defined(_WIN32)
        if (record.status.processId > 0 && StillOwnsProcessLocked(record))
            (void)::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(record.status.processId));
#else
        if (record.status.processId > 0 && StillOwnsProcessLocked(record))
            (void)::kill(-static_cast<pid_t>(record.status.processId), SIGTERM);
#endif
        record.status.state = draining ? SupervisedProcessState::Draining : SupervisedProcessState::Stopping;
        record.status.health = ProcessHealth::Unknown;
        record.stopDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(record.definition.gracefulStopMilliseconds);
        record.status.drainDeadlineUnixMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                (std::chrono::system_clock::now() +
                 std::chrono::milliseconds(record.definition.gracefulStopMilliseconds))
                    .time_since_epoch())
                .count();
    }

    void OrchestrationService::MonitorMain()
    {
        std::unique_lock lock(m_mutex);
        while (!m_stopping)
        {
            ReapAndSuperviseLocked(std::chrono::steady_clock::now());
            m_wake.wait_for(lock, std::chrono::milliseconds(100), [this] { return m_stopping; });
        }
    }

    void OrchestrationService::ReapAndSuperviseLocked(std::chrono::steady_clock::time_point now)
    {
        bool stateChanged = false;
        const auto wallNow = std::chrono::system_clock::now();
#if !defined(_WIN32)
        for (auto& [id, record] : m_records)
        {
            if (record.status.processId > 0)
            {
                int waitStatus = 0;
                const pid_t result = ::waitpid(static_cast<pid_t>(record.status.processId), &waitStatus, WNOHANG);
                if (result > 0)
                {
                    stateChanged = true;
                    const bool requestedStop = record.status.state == SupervisedProcessState::Stopping ||
                                               record.status.state == SupervisedProcessState::Draining;
                    const bool succeeded = WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0;
                    const bool expectedExit = succeeded || requestedStop;
                    record.status.exitCode =
                        WIFEXITED(waitStatus) ? WEXITSTATUS(waitStatus) : 128 + WTERMSIG(waitStatus);
                    record.status.processId = 0;
                    record.status.processStartToken = 0;
                    record.nativeProcessHandle = 0;
                    record.status.health = expectedExit ? ProcessHealth::Unknown : ProcessHealth::Unhealthy;
                    record.status.drainDeadlineUnixMilliseconds = 0;
                    if (!expectedExit)
                    {
                        record.recentCrashes.push_back(wallNow);
                        while (!record.recentCrashes.empty() &&
                               wallNow - record.recentCrashes.front() > std::chrono::minutes(1))
                            record.recentCrashes.pop_front();
                    }
                    record.status.crashLoopCount = static_cast<uint32_t>(record.recentCrashes.size());
                    if (!expectedExit && record.recentCrashes.size() >= m_config.maximumCrashesPerMinute)
                    {
                        record.desiredRunning = false;
                        record.restartAfterStop = false;
                        record.status.state = SupervisedProcessState::Quarantined;
                        continue;
                    }
                    const bool policyRestart =
                        !requestedStop && record.desiredRunning &&
                        (record.definition.restartPolicy == RestartPolicy::Always ||
                         (record.definition.restartPolicy == RestartPolicy::OnFailure && !succeeded));
                    if (!m_stopping && (record.restartAfterStop || policyRestart))
                    {
                        record.restartAfterStop = false;
                        record.status.state = SupervisedProcessState::Backoff;
                        record.restartAt = now + std::chrono::milliseconds(m_config.restartBackoffMilliseconds);
                    }
                    else
                    {
                        record.desiredRunning = false;
                        record.status.state =
                            expectedExit ? SupervisedProcessState::Stopped : SupervisedProcessState::Failed;
                    }
                }
                else if ((record.status.state == SupervisedProcessState::Stopping ||
                          record.status.state == SupervisedProcessState::Draining) &&
                         now >= record.stopDeadline)
                {
                    if (StillOwnsProcessLocked(record))
                        (void)::kill(-static_cast<pid_t>(record.status.processId), SIGKILL);
                    record.stopDeadline = now + std::chrono::hours(24);
                }
            }

            if (!m_stopping && record.status.state == SupervisedProcessState::Backoff && now >= record.restartAt)
            {
                if (RunningCountLocked() >= m_config.maximumRunningProcesses)
                {
                    record.restartAt = now + std::chrono::milliseconds(m_config.restartBackoffMilliseconds);
                    continue;
                }
                ++record.status.restartCount;
                std::string ignored;
                if (!LaunchLocked(record, ignored))
                {
                    record.status.state = SupervisedProcessState::Backoff;
                    record.restartAt = now + std::chrono::milliseconds(m_config.restartBackoffMilliseconds);
                }
                stateChanged = true;
            }
        }
#else
        for (auto& [id, record] : m_records)
        {
            if (record.nativeProcessHandle != 0)
            {
                HANDLE process = reinterpret_cast<HANDLE>(record.nativeProcessHandle);
                const DWORD wait = ::WaitForSingleObject(process, 0);
                if (wait == WAIT_OBJECT_0)
                {
                    stateChanged = true;
                    const bool requestedStop = record.status.state == SupervisedProcessState::Stopping ||
                                               record.status.state == SupervisedProcessState::Draining;
                    DWORD exitCode = 1;
                    (void)::GetExitCodeProcess(process, &exitCode);
                    const bool succeeded = exitCode == 0;
                    const bool expectedExit = succeeded || requestedStop;
                    record.status.exitCode = static_cast<int32_t>(exitCode);
                    ::CloseHandle(process);
                    record.nativeProcessHandle = 0;
                    if (record.nativeJobHandle != 0)
                    {
                        ::CloseHandle(reinterpret_cast<HANDLE>(record.nativeJobHandle));
                        record.nativeJobHandle = 0;
                    }
                    record.status.processId = 0;
                    record.status.processStartToken = 0;
                    record.status.health = expectedExit ? ProcessHealth::Unknown : ProcessHealth::Unhealthy;
                    record.status.drainDeadlineUnixMilliseconds = 0;
                    if (!expectedExit)
                    {
                        record.recentCrashes.push_back(wallNow);
                        while (!record.recentCrashes.empty() &&
                               wallNow - record.recentCrashes.front() > std::chrono::minutes(1))
                            record.recentCrashes.pop_front();
                    }
                    record.status.crashLoopCount = static_cast<uint32_t>(record.recentCrashes.size());
                    if (!expectedExit && record.recentCrashes.size() >= m_config.maximumCrashesPerMinute)
                    {
                        record.desiredRunning = false;
                        record.restartAfterStop = false;
                        record.status.state = SupervisedProcessState::Quarantined;
                        continue;
                    }
                    const bool policyRestart =
                        !requestedStop && record.desiredRunning &&
                        (record.definition.restartPolicy == RestartPolicy::Always ||
                         (record.definition.restartPolicy == RestartPolicy::OnFailure && !succeeded));
                    if (!m_stopping && (record.restartAfterStop || policyRestart))
                    {
                        record.restartAfterStop = false;
                        record.status.state = SupervisedProcessState::Backoff;
                        record.restartAt = now + std::chrono::milliseconds(m_config.restartBackoffMilliseconds);
                    }
                    else
                    {
                        record.desiredRunning = false;
                        record.status.state =
                            expectedExit ? SupervisedProcessState::Stopped : SupervisedProcessState::Failed;
                    }
                }
                else if ((record.status.state == SupervisedProcessState::Stopping ||
                          record.status.state == SupervisedProcessState::Draining) &&
                         now >= record.stopDeadline)
                {
                    if (StillOwnsProcessLocked(record) && record.nativeJobHandle != 0)
                        (void)::TerminateJobObject(reinterpret_cast<HANDLE>(record.nativeJobHandle), 137);
                    record.stopDeadline = now + std::chrono::hours(24);
                }
            }

            if (!m_stopping && record.status.state == SupervisedProcessState::Backoff && now >= record.restartAt)
            {
                if (RunningCountLocked() >= m_config.maximumRunningProcesses)
                {
                    record.restartAt = now + std::chrono::milliseconds(m_config.restartBackoffMilliseconds);
                    continue;
                }
                ++record.status.restartCount;
                std::string ignored;
                if (!LaunchLocked(record, ignored))
                {
                    record.status.state = SupervisedProcessState::Backoff;
                    record.restartAt = now + std::chrono::milliseconds(m_config.restartBackoffMilliseconds);
                }
                stateChanged = true;
            }
        }
#endif
        if (stateChanged)
            (void)PersistSystemStateLocked();
    }

    void OrchestrationService::StopAll()
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping)
                return;
            m_stopping = true;
            for (auto& [id, record] : m_records)
            {
                record.desiredRunning = false;
                record.restartAfterStop = false;
                if (IsActive(record.status.state))
                    RequestStopLocked(record, false);
            }
        }
        m_wake.notify_all();
        if (m_monitorThread.joinable())
            m_monitorThread.join();

        std::unique_lock lock(m_mutex);
        const auto finalDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(m_config.maximumGracefulStopMilliseconds);
        while (RunningCountLocked() > 0 && std::chrono::steady_clock::now() < finalDeadline)
        {
            ReapAndSuperviseLocked(std::chrono::steady_clock::now());
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            lock.lock();
        }
#if !defined(_WIN32)
        const auto killDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (auto& [id, record] : m_records)
        {
            if (record.status.processId <= 0)
                continue;
            if (StillOwnsProcessLocked(record))
            {
                const pid_t pid = static_cast<pid_t>(record.status.processId);
                if (::kill(-pid, SIGKILL) != 0 && errno == ESRCH)
                    (void)::kill(pid, SIGKILL);
            }
        }
        bool childrenRemain = true;
        while (childrenRemain && std::chrono::steady_clock::now() < killDeadline)
        {
            childrenRemain = false;
            for (auto& [id, record] : m_records)
            {
                if (record.status.processId <= 0)
                    continue;
                int waitStatus = 0;
                const pid_t result = ::waitpid(static_cast<pid_t>(record.status.processId), &waitStatus, WNOHANG);
                if (result == 0)
                {
                    childrenRemain = true;
                    continue;
                }
                const bool reaped = result > 0;
                if (!reaped && errno == EINTR)
                {
                    childrenRemain = true;
                    continue;
                }
                record.status.processId = 0;
                record.status.processStartToken = 0;
                record.nativeProcessHandle = 0;
                record.status.state = reaped ? SupervisedProcessState::Stopped : SupervisedProcessState::Failed;
                record.status.health = reaped ? ProcessHealth::Unknown : ProcessHealth::Unhealthy;
            }
            if (childrenRemain)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (auto& [id, record] : m_records)
        {
            if (record.status.processId <= 0)
                continue;
            // Never block daemon teardown indefinitely. A child that cannot be
            // reaped by the deadline is no longer represented as safely owned.
            record.status.processId = 0;
            record.status.processStartToken = 0;
            record.nativeProcessHandle = 0;
            record.status.state = SupervisedProcessState::Failed;
            record.status.health = ProcessHealth::Unhealthy;
        }
#else
        for (auto& [id, record] : m_records)
        {
            if (record.nativeJobHandle != 0)
            {
                if (StillOwnsProcessLocked(record))
                    (void)::TerminateJobObject(reinterpret_cast<HANDLE>(record.nativeJobHandle), 137);
                ::CloseHandle(reinterpret_cast<HANDLE>(record.nativeJobHandle));
                record.nativeJobHandle = 0;
            }
            if (record.nativeProcessHandle != 0)
            {
                (void)::WaitForSingleObject(reinterpret_cast<HANDLE>(record.nativeProcessHandle), 5000);
                ::CloseHandle(reinterpret_cast<HANDLE>(record.nativeProcessHandle));
                record.nativeProcessHandle = 0;
            }
            record.status.processId = 0;
            record.status.processStartToken = 0;
            record.status.state = SupervisedProcessState::Stopped;
        }
#endif
        (void)PersistSystemStateLocked();
    }

    size_t OrchestrationService::RunningCountLocked() const
    {
        return static_cast<size_t>(std::count_if(m_records.begin(), m_records.end(),
                                                 [](const auto& pair) { return IsActive(pair.second.status.state); }));
    }

    bool OrchestrationService::StillOwnsProcessLocked(const Record& record) const
    {
#if defined(__linux__)
        return record.status.processId > 0 && record.status.processStartToken != 0 &&
               ReadProcessStartToken(record.status.processId) == record.status.processStartToken;
#elif !defined(_WIN32)
        // Generic POSIX has no portable process birth-time query. Only trust a
        // child launched by this service instance whose unreaped parent-child
        // relationship still pins the PID. Journal recovery deliberately has
        // no native marker and therefore cannot signal a possibly reused PID.
        return record.status.processId > 0 && record.status.processStartToken != 0 &&
               record.nativeProcessHandle == static_cast<std::intptr_t>(record.status.processId);
#else
        if (record.status.processId <= 0 || record.status.processStartToken == 0)
            return false;
        HANDLE process = record.nativeProcessHandle != 0
                             ? reinterpret_cast<HANDLE>(record.nativeProcessHandle)
                             : ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                             static_cast<DWORD>(record.status.processId));
        if (!process)
            return false;
        FILETIME creation{}, exit{}, kernel{}, user{};
        const bool queried = ::GetProcessTimes(process, &creation, &exit, &kernel, &user) != 0;
        if (record.nativeProcessHandle == 0)
            ::CloseHandle(process);
        ULARGE_INTEGER creationValue{};
        creationValue.LowPart = creation.dwLowDateTime;
        creationValue.HighPart = creation.dwHighDateTime;
        return queried && creationValue.QuadPart == record.status.processStartToken;
#endif
    }

    uint64_t OrchestrationService::ReadProcessStartToken(int64_t processId)
    {
#if defined(__linux__)
        std::ifstream statFile("/proc/" + std::to_string(processId) + "/stat");
        std::string line;
        if (!std::getline(statFile, line))
            return 0;
        const auto closeParen = line.rfind(')');
        if (closeParen == std::string::npos || closeParen + 2 >= line.size())
            return 0;
        std::istringstream fields(line.substr(closeParen + 2));
        std::string field;
        for (size_t index = 0; index <= 19; ++index)
        {
            if (!(fields >> field))
                return 0;
        }
        try
        {
            return std::stoull(field);
        }
        catch (...)
        {
            return 0;
        }
#elif !defined(_WIN32)
        // On non-Linux POSIX, the daemon remains the child's parent and never
        // releases the PID before waitpid. Use a per-launch birth marker while
        // that parent-child relationship is intact.
        return static_cast<uint64_t>(processId) ^
               static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#else
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
        if (!process)
            return 0;
        FILETIME creation{}, exit{}, kernel{}, user{};
        const bool queried = ::GetProcessTimes(process, &creation, &exit, &kernel, &user) != 0;
        ::CloseHandle(process);
        if (!queried)
            return 0;
        ULARGE_INTEGER creationValue{};
        creationValue.LowPart = creation.dwLowDateTime;
        creationValue.HighPart = creation.dwHighDateTime;
        return creationValue.QuadPart;
#endif
    }
} // namespace Spark::Daemon
