/** @file ServiceTopologyPanel.h @brief Local daemon/collab/gateway topology controls. */
#pragma once

#include "../Core/EditorPanel.h"

#include <memory>
#include <string>

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
        std::unique_ptr<ServiceTopologyController> m_controller;
        std::string m_daemonEndpoint = ".spark-daemon.sock";
        std::string m_collabEndpoint = ".spark-collab.sock";
        std::string m_gatewayConfig = "Config/gateway.ini";
    };
} // namespace SparkEditor
