#include "ServiceTopologyPanel.h"

#include "ServiceTopologyController.h"
#include "../Core/ProjectManager.h"
#include "../Utils/EditorProcessLaunch.h"

#include <imgui.h>

#include <filesystem>

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
        std::filesystem::path project = ProjectManager::GetActiveProjectPath();
        if (project.empty())
            project = std::filesystem::current_path();
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
             m_daemonEndpoint});
        m_controller->Configure(TopologyService::Collaboration,
                                {executable("SparkCollabServer"),
                                 ServiceTopologyController::EndpointArguments(m_collabEndpoint), m_collabEndpoint});
        m_controller->Configure(TopologyService::Orchestrator,
                                {executable("SparkOrchestrator"),
                                 ServiceTopologyController::OrchestratorStatusArguments(m_daemonEndpoint),
                                 m_daemonEndpoint});
        const auto health = project / "Temp/spark-gateway-health.json";
        const auto stop = project / "Temp/spark-gateway.stop";
        m_controller->Configure(TopologyService::Gateway,
                                {executable("SparkGateway"),
                                 ServiceTopologyController::GatewayArguments(project / m_gatewayConfig, health, stop),
                                 {},
                                 health,
                                 stop});
    }
} // namespace SparkEditor
