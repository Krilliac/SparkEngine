#include "DedicatedServerProcessController.h"

#include <cmath>
#include <fstream>
#include <sstream>

namespace SparkEditor
{
    namespace
    {
        constexpr auto GracefulStopTimeout = std::chrono::seconds(5);

        std::string Number(float value)
        {
            std::ostringstream stream;
            stream << value;
            return stream.str();
        }
    } // namespace

    DedicatedServerProcessController::~DedicatedServerProcessController()
    {
        RequestStop();
        if (m_process && !m_process->WaitForExit(std::chrono::seconds(2)))
            m_process->Kill();
    }

    std::vector<std::string> DedicatedServerProcessController::CreateArguments(
        const DedicatedServerLaunchRequest& request, std::string& error)
    {
        error.clear();
        if (request.executable.empty())
            error = "SparkServer executable is not configured";
        else if (request.module.empty() == request.manifest.empty())
            error = "Select exactly one game module or module manifest";
        else if (request.port == 0)
            error = "Server port must be non-zero";
        else if (request.maxClients == 0)
            error = "Max clients must be non-zero";
        else if (!std::isfinite(request.tickRate) || request.tickRate < 1.0f || request.tickRate > 1000.0f)
            error = "Tick rate must be between 1 and 1000 Hz";
        else if (request.stopFile.empty())
            error = "A stop sentinel path is required for graceful shutdown";
        if (!error.empty())
            return {};

        std::vector<std::string> arguments;
        arguments.reserve(22);
        if (!request.module.empty())
        {
            arguments.emplace_back("--module");
            arguments.push_back(request.module.string());
        }
        else
        {
            arguments.emplace_back("--manifest");
            arguments.push_back(request.manifest.string());
        }
        arguments.insert(arguments.end(),
                         {"--port", std::to_string(request.port), "--max-clients", std::to_string(request.maxClients),
                          "--tick-rate", Number(request.tickRate), "--stop-file", request.stopFile.string()});
        if (!request.serverName.empty())
            arguments.insert(arguments.end(), {"--name", request.serverName});
        if (!request.map.empty())
            arguments.insert(arguments.end(), {"--map", request.map});
        if (!request.healthFile.empty())
            arguments.insert(arguments.end(),
                             {"--health-file", request.healthFile.string(), "--status-interval-ms", "250"});
        if (request.lanOnly)
            arguments.emplace_back("--lan-only");
        if (!request.lanBroadcast)
            arguments.emplace_back("--no-lan-broadcast");
        return arguments;
    }

    bool DedicatedServerProcessController::PackageExecutable(const std::filesystem::path& sourceExecutable,
                                                             const std::filesystem::path& outputDirectory,
                                                             std::string_view outputName, std::string& error)
    {
        error.clear();
        std::error_code filesystemError;
        if (!std::filesystem::is_regular_file(sourceExecutable, filesystemError))
        {
            error = "Built SparkServer executable was not found: " + sourceExecutable.string();
            return false;
        }
        if (outputDirectory.empty() || outputName.empty() || std::filesystem::path(outputName).has_parent_path())
        {
            error = "Server package output and executable name must be simple, non-empty paths";
            return false;
        }
        std::filesystem::create_directories(outputDirectory, filesystemError);
        if (filesystemError)
        {
            error = "Could not create server package directory: " + filesystemError.message();
            return false;
        }
        std::filesystem::path filename(outputName);
        if (filename.extension().empty())
            filename += sourceExecutable.extension();
        std::filesystem::copy_file(sourceExecutable, outputDirectory / filename,
                                   std::filesystem::copy_options::overwrite_existing, filesystemError);
        if (filesystemError)
        {
            error = "Could not package SparkServer: " + filesystemError.message();
            return false;
        }
        return true;
    }

    bool DedicatedServerProcessController::Launch(const DedicatedServerLaunchRequest& request)
    {
        if (m_process && m_process->IsRunning())
        {
            m_snapshot.error = "SparkServer is already running";
            return false;
        }

        std::string error;
        const auto arguments = CreateArguments(request, error);
        if (!error.empty())
        {
            m_snapshot = {DedicatedServerProcessState::Failed, {}, {}, std::move(error)};
            return false;
        }

        std::error_code filesystemError;
        std::filesystem::remove(request.stopFile, filesystemError);
        filesystemError.clear();
        if (!request.healthFile.empty())
            std::filesystem::remove(request.healthFile, filesystemError);

        Spark::Process::Builder builder(request.executable.string());
        if (!request.workingDirectory.empty())
            builder.WorkingDirectory(request.workingDirectory.string());
        for (const auto& argument : arguments)
            builder.Arg(argument);
        auto launched = builder.CaptureStdout().MergeStderrIntoStdout().NoWindow().Launch();
        if (!launched)
        {
            m_snapshot = {DedicatedServerProcessState::Failed, {}, {}, launched.error()};
            return false;
        }

        m_request = request;
        m_process.emplace(std::move(*launched));
        m_snapshot = {DedicatedServerProcessState::Running, {}, {}, {}};
        m_logLines.clear();
        m_logLines.emplace_back("[Editor] SparkServer process launched");
        return true;
    }

    void DedicatedServerProcessController::RequestStop()
    {
        if (!m_process || !m_process->IsRunning() || m_snapshot.state == DedicatedServerProcessState::Stopping)
            return;
        std::error_code error;
        std::filesystem::create_directories(m_request.stopFile.parent_path(), error);
        std::ofstream sentinel(m_request.stopFile, std::ios::binary | std::ios::trunc);
        if (sentinel)
        {
            sentinel << "stop\n";
            m_snapshot.state = DedicatedServerProcessState::Stopping;
            m_stopRequestedAt = std::chrono::steady_clock::now();
            m_logLines.emplace_back("[Editor] Graceful stop requested");
        }
        else
        {
            m_snapshot.error = "Could not create SparkServer stop sentinel";
            m_process->Kill();
        }
    }

    void DedicatedServerProcessController::Update()
    {
        if (!m_process)
            return;
        std::string line;
        while (m_process->TryReadLine(line))
            m_logLines.push_back(std::move(line));

        if (!m_request.healthFile.empty())
        {
            std::ifstream health(m_request.healthFile, std::ios::binary);
            if (health)
                m_snapshot.healthJson.assign(std::istreambuf_iterator<char>(health), {});
        }

        if (const auto exitCode = m_process->GetExitCode())
            FinishExitedProcess(*exitCode);
        else if (m_snapshot.state == DedicatedServerProcessState::Stopping &&
                 std::chrono::steady_clock::now() - m_stopRequestedAt >= GracefulStopTimeout)
        {
            m_logLines.emplace_back("[Editor] Graceful stop timed out; terminating SparkServer");
            m_process->Kill();
        }
    }

    DedicatedServerProcessSnapshot DedicatedServerProcessController::GetSnapshot() const
    {
        return m_snapshot;
    }

    std::vector<std::string> DedicatedServerProcessController::DrainLogLines()
    {
        std::vector<std::string> result;
        result.swap(m_logLines);
        return result;
    }

    void DedicatedServerProcessController::FinishExitedProcess(int exitCode)
    {
        AppendMultiline(m_process->ReadAllStdout(), "");
        m_snapshot.exitCode = exitCode;
        m_snapshot.state = exitCode == 0 ? DedicatedServerProcessState::Exited : DedicatedServerProcessState::Failed;
        if (exitCode != 0 && m_snapshot.error.empty())
            m_snapshot.error = "SparkServer exited with code " + std::to_string(exitCode);
        m_logLines.emplace_back("[Editor] SparkServer exited with code " + std::to_string(exitCode));
        m_process.reset();
    }

    void DedicatedServerProcessController::AppendMultiline(std::string text, std::string_view prefix)
    {
        std::istringstream stream(std::move(text));
        std::string line;
        while (std::getline(stream, line))
            m_logLines.emplace_back(std::string(prefix) + line);
    }
} // namespace SparkEditor
