/**
 * @file NetworkManagerRuntimeAdapter.cpp
 * @brief INetworkRuntime adapter backed by NetworkManager.
 */

#include "NetworkManagerRuntimeAdapter.h"

#ifdef ENABLE_NETWORKING

namespace Spark::Net
{

    NetworkManagerRuntimeAdapter::NetworkManagerRuntimeAdapter(NetworkManager& networkManager)
        : m_networkManager(networkManager)
    {
    }

    bool NetworkManagerRuntimeAdapter::Initialize()
    {
        return m_networkManager.Initialize();
    }

    bool NetworkManagerRuntimeAdapter::StartServer(uint16_t port, int maxClients,
                                                   const NetworkEndpointPolicy& endpointPolicy,
                                                   bool allowLanAdvertisement)
    {
        return m_networkManager.StartServer(port, maxClients, endpointPolicy, allowLanAdvertisement);
    }

    void NetworkManagerRuntimeAdapter::StopServer()
    {
        m_networkManager.StopServer();
    }

    void NetworkManagerRuntimeAdapter::Shutdown()
    {
        m_networkManager.Shutdown();
    }

    void NetworkManagerRuntimeAdapter::Update(float deltaTime)
    {
        m_networkManager.Update(deltaTime);
    }

    void NetworkManagerRuntimeAdapter::SendToClient(ClientID client, const NetworkMessage& msg)
    {
        m_networkManager.SendToClient(client, msg);
    }

    void NetworkManagerRuntimeAdapter::SendToAll(const NetworkMessage& msg)
    {
        m_networkManager.SendToAll(msg);
    }

    void NetworkManagerRuntimeAdapter::SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg)
    {
        m_networkManager.SendToAllExcept(excludeClient, msg);
    }

    void NetworkManagerRuntimeAdapter::RegisterHandler(MessageType type, MessageHandler handler)
    {
        m_networkManager.RegisterHandler(type, std::move(handler));
    }

    void NetworkManagerRuntimeAdapter::ClearHandlers()
    {
        m_networkManager.ClearHandlers();
    }

    std::unordered_map<ClientID, ClientInfo> NetworkManagerRuntimeAdapter::GetClients() const
    {
        return m_networkManager.GetClients();
    }

    NetworkStats NetworkManagerRuntimeAdapter::GetStats() const
    {
        return m_networkManager.GetStats();
    }

    void NetworkManagerRuntimeAdapter::KickClient(ClientID client, const std::string& reason)
    {
        m_networkManager.KickClient(client, reason);
    }

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
