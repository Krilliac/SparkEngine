/** @file ServiceTopologyPanel.h @brief Local daemon/collab/gateway topology controls. */
#pragma once

#include "../Core/EditorPanel.h"

#include <memory>
#include <string>
#include <vector>

namespace SparkEditor
{
    class ServiceTopologyController;

    class ServiceTopologyPanel final : public EditorPanel
    {
      public:
        ServiceTopologyPanel();
        ~ServiceTopologyPanel() override;
        bool Initialize() override;
        void Update(float) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string&, void*) override { return false; }
        std::string GetTypeName() const override { return "ServiceTopologyPanel"; }

      private:
        void ConfigureController();
        void RunOrchestrator(std::vector<std::string> arguments);
        std::unique_ptr<ServiceTopologyController> m_controller;
        std::string m_daemonEndpoint = ".spark-daemon.sock";
        std::string m_collabEndpoint = ".spark-collab.sock";
        std::string m_gatewayConfig = "Config/gateway.ini";
        std::string m_serverConfig = "Config/server.ini";
        std::string m_serverId = "local-area";
        std::string m_orchestratorNotice; ///< Last orchestrator command rejection, shown in the panel.
    };
} // namespace SparkEditor
