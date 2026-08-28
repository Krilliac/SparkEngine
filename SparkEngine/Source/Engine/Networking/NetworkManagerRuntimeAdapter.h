/**
 * @file NetworkManagerRuntimeAdapter.h
 * @brief INetworkRuntime adapter backed by NetworkManager.
 */

#pragma once

#include "INetworkRuntime.h"

#ifdef ENABLE_NETWORKING

namespace Spark::Net
{

    class NetworkManagerRuntimeAdapter final : public INetworkRuntime
    {
      public:
        explicit NetworkManagerRuntimeAdapter(NetworkManager& networkManager);

        bool Initialize() override;
        bool StartServer(uint16_t port, int maxClients, const NetworkEndpointPolicy& endpointPolicy) override;
        void StopServer() override;
        void Shutdown() override;
        void Update(float deltaTime) override;

        void SendToClient(ClientID client, const NetworkMessage& msg) override;
        void SendToAll(const NetworkMessage& msg) override;
        void SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg) override;

        void RegisterHandler(MessageType type, MessageHandler handler) override;
        void ClearHandlers() override;

        std::unordered_map<ClientID, ClientInfo> GetClients() const override;
        NetworkStats GetStats() const override;
        void KickClient(ClientID client, const std::string& reason) override;

      private:
        NetworkManager& m_networkManager;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
