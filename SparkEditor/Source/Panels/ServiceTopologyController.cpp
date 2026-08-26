#include "ServiceTopologyController.h"

#include "Utils/DaemonClient.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>

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
        record.snapshot = {};
        const auto removeStaleFile = [&record](const std::filesystem::path& path, const char* description)
        {
            if (path.empty())
                return true;
            std::error_code error;
            std::filesystem::remove(path, error);
            if (!error)
                return true;
            record.snapshot.status = std::string("Could not remove stale ") + description + ": " + error.message();
            return false;
        };
        if (!removeStaleFile(record.spec.stopFile, "stop file") ||
            !removeStaleFile(record.spec.healthFile, "health file"))
            return false;

        Spark::Process::Builder builder(record.spec.executable.string());
        for (const auto& argument : record.spec.arguments)
            builder.Arg(argument);
        auto launched = builder.CaptureStdout().MergeStderrIntoStdout().NoWindow().Launch();
        if (!launched)
        {
            record.snapshot.status = launched.error();
            return false;
        }
        record.process.emplace(std::move(*launched));
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
            size_t drainedLines = 0;
            while (drainedLines < MaxDrainedLogLinesPerUpdate && record.process->TryReadLine(line))
            {
                AppendLogLine(record.snapshot, std::move(line));
                ++drainedLines;
            }
            if (!record.spec.healthFile.empty())
            {
                std::string health;
                if (ReadHealthFile(record.spec.healthFile, health))
                    record.snapshot.health = std::move(health);
                else
                    record.snapshot.health.clear();
            }
            if (const auto exit = record.process->GetExitCode())
            {
                std::istringstream remaining(record.process->ReadAllStdout());
                while (std::getline(remaining, line))
                    AppendLogLine(record.snapshot, std::move(line));
                record.snapshot.running = false;
                record.snapshot.exitCode = *exit;
                record.snapshot.status = *exit == 0 ? "Exited" : "Failed (" + std::to_string(*exit) + ")";
                record.process.reset();
            }
        }
    }

    void ServiceTopologyController::AppendLogLine(TopologyServiceSnapshot& snapshot, std::string line)
    {
        if (line.size() > MaxRetainedLogLineBytes)
        {
            constexpr std::string_view suffix = " [truncated]";
            line.resize(MaxRetainedLogLineBytes - suffix.size());
            line.append(suffix);
        }
        while (snapshot.log.size() >= MaxRetainedLogLines)
            snapshot.log.pop_front();
        snapshot.log.push_back(std::move(line));
    }

    bool ServiceTopologyController::ReadHealthFile(const std::filesystem::path& path, std::string& contents)
    {
        contents.clear();
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error || size > MaxHealthFileBytes)
            return false;

        std::ifstream health(path, std::ios::binary);
        if (!health)
            return false;

        contents.reserve(static_cast<size_t>(size));
        std::array<char, 4096> buffer{};
        while (health && contents.size() < MaxHealthFileBytes)
        {
            const size_t remaining = MaxHealthFileBytes - contents.size();
            health.read(buffer.data(), static_cast<std::streamsize>(std::min(remaining, buffer.size())));
            const std::streamsize count = health.gcount();
            if (count > 0)
                contents.append(buffer.data(), static_cast<size_t>(count));
        }

        if (health.bad() || health.peek() != std::char_traits<char>::eof())
        {
            contents.clear();
            return false;
        }
        return true;
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

    std::vector<std::string> ServiceTopologyController::OrchestratorDefineArguments(
        std::string endpoint, std::string processId, const std::filesystem::path& executable,
        const std::filesystem::path& workingDirectory, std::vector<std::string> processArguments)
    {
        std::vector<std::string> arguments = {"--socket",           std::move(endpoint), "define",
                                              std::move(processId), executable.string(), workingDirectory.string()};
        arguments.insert(arguments.end(), std::make_move_iterator(processArguments.begin()),
                         std::make_move_iterator(processArguments.end()));
        return arguments;
    }

    std::vector<std::string> ServiceTopologyController::OrchestratorMutationArguments(std::string endpoint,
                                                                                      std::string command,
                                                                                      std::string processId)
    {
        return {"--socket", std::move(endpoint), std::move(command), std::move(processId)};
    }
} // namespace SparkEditor
