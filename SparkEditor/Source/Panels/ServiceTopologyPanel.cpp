#include "ServiceTopologyPanel.h"

#include "ServiceTopologyController.h"
#include "../Core/ProjectManager.h"
#include "../Utils/EditorProcessLaunch.h"

#include <imgui.h>

#include <filesystem>
#include <string_view>
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

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
        ImGui::SetNextWindowSize(Spark::Ui::Px2(860.0f, 520.0f), ImGuiCond_Appearing);
        if (BeginPanel())
        {
            ImGui::PushTextWrapPos(0.0f);
            Spark::Ui::SecondaryText(
                "Local production topology. Start area SparkServer processes before Gateway; the gateway "
                "intentionally fails startup if its key or area control pipes are unavailable.");
            ImGui::PopTextWrapPos();
            const std::filesystem::path project = ProjectManager::GetActiveProjectPath();
            const bool hasProject = !project.empty();
            if (!hasProject)
                Spark::Ui::StatusText(Spark::Ui::Status::Warning,
                                      "Open a project before starting project-owned services.");
            {
                const Spark::Ui::ScopedDisabled disabled(!hasProject, "Open a project first.");
                if (Spark::Ui::SecondaryButton("Start daemon"))
                    (void)m_controller->Start(TopologyService::Daemon);
                ImGui::SameLine();
                if (Spark::Ui::SecondaryButton("Start collaboration"))
                    (void)m_controller->Start(TopologyService::Collaboration);
                ImGui::SameLine();
                if (Spark::Ui::SecondaryButton("Start gateway"))
                    (void)m_controller->Start(TopologyService::Gateway);
                ImGui::SameLine();
                if (Spark::Ui::SecondaryButton("Refresh orchestrator"))
                    (void)m_controller->Start(TopologyService::Orchestrator);
                ImGui::SameLine();
                if (Spark::Ui::DangerButton("Stop all"))
                    m_controller->StopAll();
            }

            Spark::Ui::SubHeader("Dedicated server orchestration");
            ImGui::PushTextWrapPos(0.0f);
            Spark::Ui::SecondaryText("The daemon owns this server process. Define it once, then start, drain, stop, "
                                     "restart, or remove it through the authenticated local orchestration service.");
            ImGui::PopTextWrapPos();
            const std::filesystem::path serverConfig = project / m_serverConfig;
            if (!std::filesystem::is_regular_file(serverConfig))
                Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Missing %s", serverConfig.string().c_str());

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
                Spark::Ui::StatusText(Spark::Ui::Status::Info,
                                      "Orchestrator busy - waiting for the previous command to finish.");
            {
                const Spark::Ui::ScopedDisabled disabled(!hasProject || orchestratorBusy,
                                                         hasProject ? "Waiting for the previous orchestrator command."
                                                                    : "Open a project first.");
                if (Spark::Ui::SecondaryButton("Define server"))
                    RunOrchestrator(ServiceTopologyController::OrchestratorDefineArguments(
                        m_daemonEndpoint, m_serverId, serverExecutable, project, {"--config", serverConfig.string()}));
                ImGui::SameLine();
                if (Spark::Ui::SecondaryButton("Start server"))
                    RunOrchestrator(ServiceTopologyController::OrchestratorMutationArguments(m_daemonEndpoint, "start",
                                                                                             m_serverId));
                ImGui::SameLine();
                if (Spark::Ui::SecondaryButton("Drain"))
                    RunOrchestrator(ServiceTopologyController::OrchestratorMutationArguments(m_daemonEndpoint, "drain",
                                                                                             m_serverId));
                ImGui::SameLine();
                if (Spark::Ui::SecondaryButton("Restart"))
                    RunOrchestrator(ServiceTopologyController::OrchestratorMutationArguments(m_daemonEndpoint,
                                                                                             "restart", m_serverId));
                ImGui::SameLine();
                if (Spark::Ui::SecondaryButton("Stop server"))
                    RunOrchestrator(
                        ServiceTopologyController::OrchestratorMutationArguments(m_daemonEndpoint, "stop", m_serverId));
                ImGui::SameLine();
                if (Spark::Ui::DangerButton("Undefine"))
                    RunOrchestrator(ServiceTopologyController::OrchestratorMutationArguments(m_daemonEndpoint,
                                                                                             "undefine", m_serverId));
            }

            if (!m_orchestratorNotice.empty())
                Spark::Ui::StatusText(Spark::Ui::Status::Error, "%s", m_orchestratorNotice.c_str());

            Spark::Ui::SubHeader("Services");
            for (size_t index = 0; index < static_cast<size_t>(TopologyService::Count); ++index)
            {
                const auto service = static_cast<TopologyService>(index);
                const auto& snapshot = m_controller->Snapshot(service);
                ImGui::TextUnformatted(Name(service));
                ImGui::SameLine();
                Spark::Ui::StatusText(snapshot.running ? Spark::Ui::Status::Success : Spark::Ui::Status::Neutral, "%s",
                                      snapshot.status.c_str());
                if (!snapshot.health.empty() && ImGui::TreeNode((std::string("Health##") + Name(service)).c_str()))
                {
                    ImGui::PushTextWrapPos(0.0f);
                    Spark::Ui::MonoText("%s", snapshot.health.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::TreePop();
                }
                if (!snapshot.log.empty() && ImGui::TreeNode((std::string("Log##") + Name(service)).c_str()))
                {
                    for (const auto& line : snapshot.log)
                        Spark::Ui::MonoText("%s", line.c_str());
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
