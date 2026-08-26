#include "ServiceTopologyController.h"

#include "Utils/DaemonClient.h"

#include <fstream>
#include <sstream>

namespace SparkEditor
{
    ServiceTopologyController::~ServiceTopologyController()
    {
        StopAll();
        for (auto& record : m_records)
        {
            if (record.process && !record.process->WaitForExit(std::chrono::seconds(2)))
                record.process->Kill();
        }
    }

    void ServiceTopologyController::Configure(TopologyService service, TopologyServiceSpec spec)
    {
        Record& record = m_records[Index(service)];
        if (!record.process || !record.process->IsRunning())
            record.spec = std::move(spec);
    }

    bool ServiceTopologyController::Start(TopologyService service)
    {
        Record& record = m_records[Index(service)];
        if (record.process && record.process->IsRunning())
            return false;
        if (record.spec.executable.empty())
        {
            record.snapshot.status = "Executable is not configured";
            return false;
        }
        std::error_code error;
        if (!record.spec.stopFile.empty())
            std::filesystem::remove(record.spec.stopFile, error);
        Spark::Process::Builder builder(record.spec.executable.string());
        for (const auto& argument : record.spec.arguments)
            builder.Arg(argument);
        auto launched = builder.CaptureStdout().MergeStderrIntoStdout().Launch();
        if (!launched)
        {
            record.snapshot.status = launched.error();
            return false;
        }
        record.process.emplace(std::move(*launched));
        record.snapshot = {};
        record.snapshot.running = true;
        record.snapshot.status = "Running";
        return true;
    }

    void ServiceTopologyController::Stop(TopologyService service)
    {
        Record& record = m_records[Index(service)];
        if (!record.process || !record.process->IsRunning())
            return;
        if (service == TopologyService::Daemon || service == TopologyService::Collaboration)
        {
            Spark::Daemon::DaemonClient client;
            if (client.Connect(record.spec.localEndpoint))
                (void)client.Request(Spark::Daemon::ServiceId::Control,
                                     static_cast<uint16_t>(Spark::Daemon::ControlMessage::ShutdownRequest), {});
        }
        else if (service == TopologyService::Gateway && !record.spec.stopFile.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(record.spec.stopFile.parent_path(), error);
            std::ofstream stop(record.spec.stopFile, std::ios::trunc);
            stop << "stop\n";
        }
        else
            record.process->Kill();
        record.snapshot.status = "Stopping";
    }

    void ServiceTopologyController::StopAll()
    {
        Stop(TopologyService::Gateway);
        Stop(TopologyService::Collaboration);
        Stop(TopologyService::Daemon);
        Stop(TopologyService::Orchestrator);
    }

    void ServiceTopologyController::Update()
    {
        for (Record& record : m_records)
        {
            if (!record.process)
                continue;
            std::string line;
            while (record.process->TryReadLine(line))
                record.snapshot.log.push_back(std::move(line));
            if (!record.spec.healthFile.empty())
            {
                std::ifstream health(record.spec.healthFile, std::ios::binary);
                if (health)
                    record.snapshot.health.assign(std::istreambuf_iterator<char>(health), {});
            }
            if (const auto exit = record.process->GetExitCode())
            {
                std::istringstream remaining(record.process->ReadAllStdout());
                while (std::getline(remaining, line))
                    record.snapshot.log.push_back(std::move(line));
                record.snapshot.running = false;
                record.snapshot.exitCode = *exit;
                record.snapshot.status = *exit == 0 ? "Exited" : "Failed (" + std::to_string(*exit) + ")";
                record.process.reset();
            }
        }
    }

    const TopologyServiceSnapshot& ServiceTopologyController::Snapshot(TopologyService service) const
    {
        return m_records[Index(service)].snapshot;
    }

    std::vector<std::string> ServiceTopologyController::DaemonArguments(std::string endpoint,
                                                                        const std::filesystem::path& allowedRoot,
                                                                        const std::filesystem::path& stateFile)
    {
        return {"--socket",           std::move(endpoint),         "--orchestrator-allow-root",
                allowedRoot.string(), "--orchestrator-state-file", stateFile.string()};
    }

    std::vector<std::string> ServiceTopologyController::GatewayArguments(const std::filesystem::path& config,
                                                                         const std::filesystem::path& health,
                                                                         const std::filesystem::path& stop)
    {
        return {"--config", config.string(), "--health-file", health.string(), "--stop-file", stop.string()};
    }

    std::vector<std::string> ServiceTopologyController::EndpointArguments(std::string endpoint)
    {
        return {"--socket", std::move(endpoint)};
    }

    std::vector<std::string> ServiceTopologyController::OrchestratorStatusArguments(std::string endpoint)
    {
        return {"--socket", std::move(endpoint), "list"};
    }
} // namespace SparkEditor
