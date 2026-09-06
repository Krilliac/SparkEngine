#include "ServiceTopologyPanel.h"

#include "ServiceTopologyController.h"
#include "../Core/ProjectManager.h"
#include "../Utils/EditorProcessLaunch.h"

#include <imgui.h>

#include <filesystem>
#include <string_view>

namespace SparkEditor
{
    namespace
    {
        const char* Name(TopologyService service)
        {
            switch (service)
            {
            case TopologyService::Daemon:
                return "Daemon";
            case TopologyService::Orchestrator:
                return "Orchestrator status";
            case TopologyService::Collaboration:
                return "Collaboration broker";
            case TopologyService::Gateway:
                return "Gateway";
            default:
                return "Unknown";
            }
        }
    } // namespace

    ServiceTopologyPanel::ServiceTopologyPanel() : EditorPanel("Service Topology", "service_topology") {}
    ServiceTopologyPanel::~ServiceTopologyPanel() = default;

    bool ServiceTopologyPanel::Initialize()
    {
        m_controller = std::make_unique<ServiceTopologyController>();
        ConfigureController();
        return true;
    }

    void ServiceTopologyPanel::Update(float)
    {
        if (m_controller)
            m_controller->Update();
    }

    void ServiceTopologyPanel::Render()
    {
        if (!IsVisible())
            return;
        // The editor can initialize before a project is opened. Refresh idle
        // launch specifications so paths follow the active project.
        ConfigureController();
        // Keep the complete control row and all four service summaries visible
        // the first time this operational panel is opened.
        ImGui::SetNextWindowSize(ImVec2(860.0f, 520.0f), ImGuiCond_Appearing);
        if (BeginPanel())
        {
            ImGui::TextWrapped("Local production topology. Start area SparkServer processes before Gateway; the "
                               "gateway intentionally fails startup if its key or area control pipes are unavailable.");
            const std::filesystem::path project = ProjectManager::GetActiveProjectPath();
            const bool hasProject = !project.empty();
            if (!hasProject)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                                   "Open a project before starting project-owned services.");
            ImGui::BeginDisabled(!hasProject);
            if (ImGui::Button("Start daemon"))
                (void)m_controller->Start(TopologyService::Daemon);
            ImGui::SameLine();
            if (ImGui::Button("Start collaboration"))
                (void)m_controller->Start(TopologyService::Collaboration);
            ImGui::SameLine();
            if (ImGui::Button("Start gateway"))
                (void)m_controller->Start(TopologyService::Gateway);
            ImGui::SameLine();
            if (ImGui::Button("Refresh orchestrator"))
                (void)m_controller->Start(TopologyService::Orchestrator);
            ImGui::SameLine();
            if (ImGui::Button("Stop all"))
                m_controller->StopAll();
            ImGui::EndDisabled();

            ImGui::SeparatorText("Dedicated server orchestration");
            ImGui::TextWrapped("The daemon owns this server process. Define it once, then start, drain, stop, restart, "
                               "or remove it through the authenticated local orchestration service.");
            const std::filesystem::path serverConfig = project / m_serverConfig;
            if (!std::filesystem::is_regular_file(serverConfig))
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Missing %s", serverConfig.string().c_str());

            const std::filesystem::path binaries = GetEditorExecutableDirectory();
#ifdef _WIN32
            const std::filesystem::path serverExecutable = binaries / "SparkServer.exe";
#else
            const std::filesystem::path serverExecutable = binaries / "SparkServer";
#endif
            // Configure()/Start() are both no-ops while the previous SparkOrchestrator
            // invocation is alive, so the buttons must be disabled rather than silently
            // dropping the command.
            const bool orchestratorBusy = m_controller->Snapshot(TopologyService::Orchestrator).running;
            if (orchestratorBusy)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                                   "Orchestrator busy - waiting for the previous command to finish.");
            ImGui::BeginDisabled(!hasProject || orchestratorBusy);
            if (ImGui::Button("Define server"))
                RunOrchestrator(ServiceTopologyController::OrchestratorDefineArguments(
                    m_daemonEndpoint, m_serverId, serverExecutable, project, {"--config", serverConfig.string()}));
            ImGui::SameLine();
            if (ImGui::Button("Start server"))
                RunOrchestrator(
                    ServiceTopologyController::OrchestratorMutationArguments(m_daemonEndpoint, "start", m_serverId));
            ImGui::SameLine();
            if (ImGui::Button("Drain"))
                RunOrchestrator(
                    ServiceTopologyController::OrchestratorMutationArguments(m_daemonEndpoint, "drain", m_serverId));
            ImGui::SameLine();
            if (ImGui::Button("Restart"))
                RunOrchestrator(
                    ServiceTopologyController::OrchestratorMutationArguments(m_daemonEndpoint, "restart", m_serverId));
            ImGui::SameLine();
            if (ImGui::Button("Stop server"))
                RunOrchestrator(
                    ServiceTopologyController::OrchestratorMutationArguments(m_daemonEndpoint, "stop", m_serverId));
            ImGui::SameLine();
            if (ImGui::Button("Undefine"))
                RunOrchestrator(
                    ServiceTopologyController::OrchestratorMutationArguments(m_daemonEndpoint, "undefine", m_serverId));
            ImGui::EndDisabled();

            if (!m_orchestratorNotice.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_orchestratorNotice.c_str());

            ImGui::Separator();
            for (size_t index = 0; index < static_cast<size_t>(TopologyService::Count); ++index)
            {
                const auto service = static_cast<TopologyService>(index);
                const auto& snapshot = m_controller->Snapshot(service);
                ImGui::Text("%s: %s", Name(service), snapshot.status.c_str());
                if (!snapshot.health.empty() && ImGui::TreeNode((std::string("Health##") + Name(service)).c_str()))
                {
                    ImGui::TextWrapped("%s", snapshot.health.c_str());
                    ImGui::TreePop();
                }
                if (!snapshot.log.empty() && ImGui::TreeNode((std::string("Log##") + Name(service)).c_str()))
                {
                    for (const auto& line : snapshot.log)
                        ImGui::TextUnformatted(line.c_str());
                    ImGui::TreePop();
                }
            }
        }
        EndPanel();
    }

    void ServiceTopologyPanel::Shutdown()
    {
        if (m_controller)
            m_controller->StopAll();
    }

    void ServiceTopologyPanel::ConfigureController()
    {
        const std::filesystem::path binaries = GetEditorExecutableDirectory();
        const std::filesystem::path project = ProjectManager::GetActiveProjectPath();
        if (project.empty())
        {
            for (size_t index = 0; index < static_cast<size_t>(TopologyService::Count); ++index)
                m_controller->Configure(static_cast<TopologyService>(index), {});
            return;
        }
#ifdef _WIN32
        constexpr std::string_view suffix = ".exe";
#else
        constexpr std::string_view suffix = "";
#endif
        const auto executable = [&](std::string_view name)
        { return binaries / (std::string(name) + std::string(suffix)); };
        m_controller->Configure(
            TopologyService::Daemon,
            {executable("SparkDaemon"),
             ServiceTopologyController::DaemonArguments(m_daemonEndpoint, project, project / "Temp/orchestrator.state"),
             project, m_daemonEndpoint});
        m_controller->Configure(TopologyService::Collaboration,
                                {executable("SparkCollabServer"),
                                 ServiceTopologyController::EndpointArguments(m_collabEndpoint), project,
                                 m_collabEndpoint});
        m_controller->Configure(TopologyService::Orchestrator,
                                {executable("SparkOrchestrator"),
                                 ServiceTopologyController::OrchestratorStatusArguments(m_daemonEndpoint), project,
                                 m_daemonEndpoint});
        const auto health = project / "Temp/spark-gateway-health.json";
        const auto stop = project / "Temp/spark-gateway.stop";
        m_controller->Configure(TopologyService::Gateway,
                                {executable("SparkGateway"),
                                 ServiceTopologyController::GatewayArguments(project / m_gatewayConfig, health, stop),
                                 project,
                                 {},
                                 health,
                                 stop,
                                 project / "Config/gateway.key"});
    }

    void ServiceTopologyPanel::RunOrchestrator(std::vector<std::string> arguments)
    {
        const std::filesystem::path binaries = GetEditorExecutableDirectory();
#ifdef _WIN32
        const std::filesystem::path executable = binaries / "SparkOrchestrator.exe";
#else
        const std::filesystem::path executable = binaries / "SparkOrchestrator";
#endif
        const std::filesystem::path project = ProjectManager::GetActiveProjectPath();
        if (project.empty())
        {
            m_orchestratorNotice = "Open a project before running orchestrator commands.";
            return;
        }
        m_controller->Configure(TopologyService::Orchestrator,
                                {executable, std::move(arguments), project, m_daemonEndpoint});
        if (m_controller->Start(TopologyService::Orchestrator))
        {
            m_orchestratorNotice.clear();
            return;
        }
        // Start() refuses while the previous process is alive and when the spec is
        // unusable; report it instead of dropping the click.
        m_orchestratorNotice =
            "Orchestrator command was not started: " + m_controller->Snapshot(TopologyService::Orchestrator).status;
    }
} // namespace SparkEditor
