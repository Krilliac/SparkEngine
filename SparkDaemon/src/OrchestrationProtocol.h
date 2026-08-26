/**
 * @file OrchestrationProtocol.h
 * @brief Versioned DTOs for SparkDaemon's supervised-process control plane.
 */

#pragma once

#include "BoundedWireCodec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Daemon
{
    inline constexpr size_t kMaximumProcessIdLength = 64;
    inline constexpr size_t kMaximumProcessPathLength = 1024;
    inline constexpr size_t kMaximumProcessArgumentLength = 4096;
    inline constexpr size_t kMaximumProcessArguments = 128;
    inline constexpr size_t kMaximumClientInstanceLength = 64;

    struct MutationKey
    {
        std::string clientInstance;
        uint64_t sequence = 0;
    };

    enum class OrchestrationMessage : uint16_t
    {
        DefineRequest = 0x0001,
        DefineResponse = 0x0002,
        UndefineRequest = 0x0003,
        UndefineResponse = 0x0004,
        StartRequest = 0x0005,
        StartResponse = 0x0006,
        StopRequest = 0x0007,
        StopResponse = 0x0008,
        DrainRequest = 0x0009,
        DrainResponse = 0x000A,
        RestartRequest = 0x000B,
        RestartResponse = 0x000C,
        StatusRequest = 0x000D,
        StatusResponse = 0x000E,
        ListRequest = 0x000F,
        ListResponse = 0x0010,
    };

    enum class RestartPolicy : uint8_t
    {
        Never,
        OnFailure,
        Always,
    };

    enum class SupervisedProcessState : uint8_t
    {
        Stopped,
        Starting,
        Running,
        Draining,
        Stopping,
        Backoff,
        Failed,
        Quarantined,
    };

    enum class ProcessHealth : uint8_t
    {
        Unknown,
        Healthy,
        Unhealthy,
    };

    struct ProcessDefinition
    {
        std::string id;
        std::string executable;
        std::string workingDirectory;
        std::vector<std::string> arguments;
        RestartPolicy restartPolicy = RestartPolicy::Never;
        uint32_t gracefulStopMilliseconds = 5000;
    };

    struct ProcessStatus
    {
        std::string id;
        SupervisedProcessState state = SupervisedProcessState::Stopped;
        int64_t processId = 0;
        uint32_t restartCount = 0;
        int32_t exitCode = 0;
        ProcessHealth health = ProcessHealth::Unknown;
        uint64_t processStartToken = 0; ///< OS birth token or launch nonce; PID alone never establishes ownership.
        uint32_t crashLoopCount = 0;
        int64_t drainDeadlineUnixMilliseconds = 0;
    };

    inline bool WriteMutationKey(Wire::Writer& writer, const MutationKey& key)
    {
        if (key.sequence == 0 || !writer.WriteString(key.clientInstance, kMaximumClientInstanceLength))
            return false;
        writer.Write(key.sequence);
        return true;
    }

    inline bool ReadMutationKey(Wire::Reader& reader, MutationKey& key)
    {
        return reader.ReadString(key.clientInstance, kMaximumClientInstanceLength) && !key.clientInstance.empty() &&
               reader.Read(key.sequence) && key.sequence != 0;
    }

    inline bool EncodeProcessId(std::string_view id, std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        if (!writer.WriteString(id, kMaximumProcessIdLength))
            return false;
        out = writer.Take();
        return true;
    }

    inline bool EncodeProcessMutation(const MutationKey& key, std::string_view id, std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        if (!WriteMutationKey(writer, key) || !writer.WriteString(id, kMaximumProcessIdLength))
            return false;
        out = writer.Take();
        return true;
    }

    inline bool DecodeProcessMutation(const std::vector<uint8_t>& bytes, MutationKey& key, std::string& id)
    {
        Wire::Reader reader(bytes);
        return Wire::ReadVersion(reader) && ReadMutationKey(reader, key) &&
               reader.ReadString(id, kMaximumProcessIdLength) && reader.Finished();
    }

    inline bool DecodeProcessId(const std::vector<uint8_t>& bytes, std::string& out)
    {
        Wire::Reader reader(bytes);
        return Wire::ReadVersion(reader) && reader.ReadString(out, kMaximumProcessIdLength) && reader.Finished();
    }

    inline bool EncodeProcessDefinition(const MutationKey& key, const ProcessDefinition& value,
                                        std::vector<uint8_t>& out)
    {
        if (value.arguments.size() > kMaximumProcessArguments)
            return false;
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        if (!WriteMutationKey(writer, key) || !writer.WriteString(value.id, kMaximumProcessIdLength) ||
            !writer.WriteString(value.executable, kMaximumProcessPathLength) ||
            !writer.WriteString(value.workingDirectory, kMaximumProcessPathLength))
            return false;
        writer.Write<uint32_t>(static_cast<uint32_t>(value.arguments.size()));
        for (const auto& argument : value.arguments)
            if (!writer.WriteString(argument, kMaximumProcessArgumentLength))
                return false;
        writer.Write(value.restartPolicy);
        writer.Write(value.gracefulStopMilliseconds);
        out = writer.Take();
        return true;
    }

    inline bool DecodeProcessDefinition(const std::vector<uint8_t>& bytes, MutationKey& key, ProcessDefinition& out)
    {
        Wire::Reader reader(bytes);
        uint32_t argumentCount = 0;
        if (!Wire::ReadVersion(reader) || !ReadMutationKey(reader, key) ||
            !reader.ReadString(out.id, kMaximumProcessIdLength) ||
            !reader.ReadString(out.executable, kMaximumProcessPathLength) ||
            !reader.ReadString(out.workingDirectory, kMaximumProcessPathLength) || !reader.Read(argumentCount) ||
            argumentCount > kMaximumProcessArguments)
            return false;
        out.arguments.clear();
        out.arguments.resize(argumentCount);
        for (auto& argument : out.arguments)
            if (!reader.ReadString(argument, kMaximumProcessArgumentLength))
                return false;
        uint8_t policy = 0;
        if (!reader.Read(policy) || policy > static_cast<uint8_t>(RestartPolicy::Always) ||
            !reader.Read(out.gracefulStopMilliseconds) || !reader.Finished())
            return false;
        out.restartPolicy = static_cast<RestartPolicy>(policy);
        return true;
    }

    inline bool WriteProcessStatus(Wire::Writer& writer, const ProcessStatus& value)
    {
        if (!writer.WriteString(value.id, kMaximumProcessIdLength))
            return false;
        writer.Write(value.state);
        writer.Write(value.processId);
        writer.Write(value.restartCount);
        writer.Write(value.exitCode);
        writer.Write(value.health);
        writer.Write(value.processStartToken);
        writer.Write(value.crashLoopCount);
        writer.Write(value.drainDeadlineUnixMilliseconds);
        return true;
    }

    inline bool EncodeProcessStatuses(const std::vector<ProcessStatus>& values, std::vector<uint8_t>& out)
    {
        Wire::Writer writer;
        Wire::WriteVersion(writer);
        writer.Write<uint32_t>(static_cast<uint32_t>(values.size()));
        for (const auto& value : values)
            if (!WriteProcessStatus(writer, value))
                return false;
        out = writer.Take();
        return true;
    }

    inline bool DecodeProcessStatuses(const std::vector<uint8_t>& bytes, std::vector<ProcessStatus>& out,
                                      size_t maximum)
    {
        Wire::Reader reader(bytes);
        uint32_t count = 0;
        if (!Wire::ReadVersion(reader) || !reader.Read(count) || count > maximum)
            return false;
        out.clear();
        out.resize(count);
        for (auto& value : out)
        {
            uint8_t state = 0;
            uint8_t health = 0;
            if (!reader.ReadString(value.id, kMaximumProcessIdLength) || !reader.Read(state) ||
                state > static_cast<uint8_t>(SupervisedProcessState::Quarantined) || !reader.Read(value.processId) ||
                !reader.Read(value.restartCount) || !reader.Read(value.exitCode) || !reader.Read(health) ||
                health > static_cast<uint8_t>(ProcessHealth::Unhealthy) || !reader.Read(value.processStartToken) ||
                !reader.Read(value.crashLoopCount) || !reader.Read(value.drainDeadlineUnixMilliseconds))
                return false;
            value.state = static_cast<SupervisedProcessState>(state);
            value.health = static_cast<ProcessHealth>(health);
        }
        return reader.Finished();
    }
} // namespace Spark::Daemon
