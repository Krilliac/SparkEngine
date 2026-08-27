/**
 * @file OrchestratorMain.cpp
 * @brief Persistent-identity CLI client for SparkDaemon process supervision.
 */

#include "OrchestrationProtocol.h"
#include "OrchestratorIdentity.h"
#include "Utils/DaemonClient.h"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    const char* StateName(Spark::Daemon::SupervisedProcessState state)
    {
        using State = Spark::Daemon::SupervisedProcessState;
        switch (state)
        {
        case State::Stopped:
            return "stopped";
        case State::Starting:
            return "starting";
        case State::Running:
            return "running";
        case State::Draining:
            return "draining";
        case State::Stopping:
            return "stopping";
        case State::Backoff:
            return "backoff";
        case State::Failed:
            return "failed";
        case State::Quarantined:
            return "quarantined";
        }
        return "unknown";
    }

    void PrintUsage()
    {
        std::puts("SparkOrchestrator [--socket <path>] [--client <id>] [--sequence <N>] <command>\n"
                  "  Default mutation identity persists under the user state directory.\n"
                  "  Set SPARK_ORCHESTRATOR_IDENTITY to override its path.\n"
                  "  list\n"
                  "  daemon-shutdown\n"
                  "  status <id>\n"
                  "  start|stop|drain|restart|undefine <id>\n"
                  "  define <id> <executable> <working-directory> [arguments ...]");
    }
} // namespace

int main(int argc, char** argv)
{
    std::string socketPath = "./.spark-daemon.sock";
    Spark::Daemon::MutationKey mutation;
    bool explicitClient = false;
    bool explicitSequence = false;
    int position = 1;
    while (position < argc)
    {
        const std::string_view option = argv[position];
        if (option == "--socket" && position + 1 < argc)
        {
            socketPath = argv[position + 1];
            position += 2;
        }
        else if (option == "--client" && position + 1 < argc)
        {
            mutation.clientInstance = argv[position + 1];
            explicitClient = true;
            position += 2;
        }
        else if (option == "--sequence" && position + 1 < argc)
        {
            try
            {
                mutation.sequence = std::stoull(argv[position + 1]);
                explicitSequence = true;
            }
            catch (...)
            {
                std::fprintf(stderr, "SparkOrchestrator: invalid sequence\n");
                return 2;
            }
            position += 2;
        }
        else if (option == "--help" || option == "-h")
        {
            PrintUsage();
            return 0;
        }
        else
            break;
    }
    if (position >= argc)
    {
        PrintUsage();
        return 2;
    }

    std::optional<Spark::Daemon::OrchestratorIdentityLease> identityLease;
    if (explicitClient != explicitSequence)
    {
        std::fprintf(stderr, "SparkOrchestrator: --client and --sequence must be supplied together\n");
        return 2;
    }
    if (!explicitClient)
    {
        std::string identityError;
        identityLease = Spark::Daemon::OrchestratorIdentityLease::Acquire(
            Spark::Daemon::DefaultOrchestratorIdentityPath(), identityError);
        if (!identityLease)
        {
            std::fprintf(stderr, "SparkOrchestrator: %s\n", identityError.c_str());
            return 1;
        }
        mutation = identityLease->Key();
    }

    const std::string command = argv[position++];
    Spark::Daemon::OrchestrationMessage requestType = Spark::Daemon::OrchestrationMessage::ListRequest;
    bool daemonShutdown = false;
    std::vector<uint8_t> payload;
    if (command == "daemon-shutdown")
    {
        daemonShutdown = true;
    }
    else if (command == "list")
    {
        Spark::Daemon::Wire::Writer writer;
        Spark::Daemon::Wire::WriteVersion(writer);
        payload = writer.Take();
        requestType = Spark::Daemon::OrchestrationMessage::ListRequest;
    }
    else if (command == "define")
    {
        if (position + 2 >= argc)
        {
            PrintUsage();
            return 2;
        }
        Spark::Daemon::ProcessDefinition definition;
        definition.id = argv[position++];
        definition.executable = argv[position++];
        definition.workingDirectory = argv[position++];
        while (position < argc)
            definition.arguments.emplace_back(argv[position++]);
        if (!Spark::Daemon::EncodeProcessDefinition(mutation, definition, payload))
        {
            std::fprintf(stderr, "SparkOrchestrator: definition exceeds protocol bounds\n");
            return 2;
        }
        requestType = Spark::Daemon::OrchestrationMessage::DefineRequest;
    }
    else
    {
        if (position >= argc)
        {
            PrintUsage();
            return 2;
        }
        const std::string id = argv[position];
        if (command == "status")
        {
            Spark::Daemon::EncodeProcessId(id, payload);
            requestType = Spark::Daemon::OrchestrationMessage::StatusRequest;
        }
        else
        {
            if (!Spark::Daemon::EncodeProcessMutation(mutation, id, payload))
            {
                std::fprintf(stderr, "SparkOrchestrator: invalid mutation identity or process id\n");
                return 2;
            }
            if (command == "start")
                requestType = Spark::Daemon::OrchestrationMessage::StartRequest;
            else if (command == "stop")
                requestType = Spark::Daemon::OrchestrationMessage::StopRequest;
            else if (command == "drain")
                requestType = Spark::Daemon::OrchestrationMessage::DrainRequest;
            else if (command == "restart")
                requestType = Spark::Daemon::OrchestrationMessage::RestartRequest;
            else if (command == "undefine")
                requestType = Spark::Daemon::OrchestrationMessage::UndefineRequest;
            else
            {
                PrintUsage();
                return 2;
            }
        }
    }

    Spark::Daemon::DaemonClient client;
    auto connected = client.Connect(socketPath);
    if (!connected)
    {
        std::fprintf(stderr, "SparkOrchestrator: %s\n", connected.error().c_str());
        return 1;
    }
    auto response =
        daemonShutdown
            ? client.Request(Spark::Daemon::ServiceId::Control,
                             static_cast<uint16_t>(Spark::Daemon::ControlMessage::ShutdownRequest), {})
            : client.Request(Spark::Daemon::ServiceId::Orchestration, static_cast<uint16_t>(requestType), payload);
    if (!response)
    {
        std::fprintf(stderr, "SparkOrchestrator: %s\n", response.error().c_str());
        return 1;
    }

    if (!daemonShutdown && (requestType == Spark::Daemon::OrchestrationMessage::ListRequest ||
                            requestType == Spark::Daemon::OrchestrationMessage::StatusRequest))
    {
        std::vector<Spark::Daemon::ProcessStatus> statuses;
        if (!Spark::Daemon::DecodeProcessStatuses(response->payload, statuses, 1024))
        {
            std::fprintf(stderr, "SparkOrchestrator: malformed status response\n");
            return 1;
        }
        for (const auto& status : statuses)
            std::printf("%s\t%s\tpid=%lld\thealth=%u\texit=%d\trestarts=%u\tcrashes=%u\n", status.id.c_str(),
                        StateName(status.state), static_cast<long long>(status.processId),
                        static_cast<unsigned>(status.health), status.exitCode, status.restartCount,
                        status.crashLoopCount);
    }
    else
    {
        std::puts("ok");
    }
    return 0;
}
