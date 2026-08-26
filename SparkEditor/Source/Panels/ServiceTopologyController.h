/** @file ServiceTopologyController.h @brief Real local service process/control model. */
#pragma once

#include "Utils/Process.h"

#include <array>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace SparkEditor
{
    enum class TopologyService : size_t
    {
        Daemon,
        Orchestrator,
        Collaboration,
        Gateway,
        Count
    };

    struct TopologyServiceSpec
    {
        std::filesystem::path executable;
        std::vector<std::string> arguments;
        std::string localEndpoint;
        std::filesystem::path healthFile;
        std::filesystem::path stopFile;
    };

    struct TopologyServiceSnapshot
    {
        bool running = false;
        std::optional<int> exitCode;
        std::string status = "Idle";
        std::string health;
        std::deque<std::string> log;
    };

    class ServiceTopologyController
    {
      public:
        ServiceTopologyController() = default;
        ~ServiceTopologyController();
        void Configure(TopologyService service, TopologyServiceSpec spec);
        [[nodiscard]] bool Start(TopologyService service);
        void Stop(TopologyService service);
        void StopAll();
        void Update();
        [[nodiscard]] const TopologyServiceSnapshot& Snapshot(TopologyService service) const;

        [[nodiscard]] static std::vector<std::string> DaemonArguments(std::string endpoint,
                                                                      const std::filesystem::path& allowedRoot,
                                                                      const std::filesystem::path& stateFile);
        [[nodiscard]] static std::vector<std::string> GatewayArguments(const std::filesystem::path& config,
                                                                       const std::filesystem::path& health,
                                                                       const std::filesystem::path& stop);
        [[nodiscard]] static std::vector<std::string> EndpointArguments(std::string endpoint);
        [[nodiscard]] static std::vector<std::string> OrchestratorStatusArguments(std::string endpoint);
        [[nodiscard]] static std::vector<std::string> OrchestratorDefineArguments(
            std::string endpoint, std::string processId, const std::filesystem::path& executable,
            const std::filesystem::path& workingDirectory, std::vector<std::string> processArguments);
        [[nodiscard]] static std::vector<std::string> OrchestratorMutationArguments(std::string endpoint,
                                                                                    std::string command,
                                                                                    std::string processId);

        static constexpr size_t MaxRetainedLogLines = 1024;
        static constexpr size_t MaxRetainedLogLineBytes = 16 * 1024;
        static constexpr size_t MaxDrainedLogLinesPerUpdate = 128;
        static constexpr size_t MaxHealthFileBytes = 256 * 1024;
        static void AppendLogLine(TopologyServiceSnapshot& snapshot, std::string line);
        static bool ReadHealthFile(const std::filesystem::path& path, std::string& contents);

      private:
        struct Record
        {
            TopologyServiceSpec spec;
            TopologyServiceSnapshot snapshot;
            std::optional<Spark::Process> process;
        };
        static constexpr size_t Index(TopologyService service) { return static_cast<size_t>(service); }
        std::array<Record, static_cast<size_t>(TopologyService::Count)> m_records;
    };
} // namespace SparkEditor
