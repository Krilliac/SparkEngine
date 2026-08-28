/**
 * @file INetworkRuntime.h
 * @brief Minimal networking runtime interface used by DedicatedServer.
 */

#pragma once

#include "NetworkManager.h"

#ifdef ENABLE_NETWORKING

#include <functional>
#include <unordered_map>

namespace Spark::Net
{

    /// @brief DedicatedServer-facing networking runtime abstraction.
    class INetworkRuntime
    {
      public:
        using MessageHandler = std::function<void(const NetworkMessage&)>;

        virtual ~INetworkRuntime() = default;

        virtual bool Initialize() = 0;
        virtual bool StartServer(uint16_t port, int maxClients, const NetworkEndpointPolicy& endpointPolicy,
                                 bool allowLanAdvertisement) = 0;
        virtual void StopServer() = 0;
        virtual void Shutdown() = 0;
        virtual void Update(float deltaTime) = 0;

        virtual void SendToClient(ClientID client, const NetworkMessage& msg) = 0;
        virtual void SendToAll(const NetworkMessage& msg) = 0;
        virtual void SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg) = 0;

        virtual void RegisterHandler(MessageType type, MessageHandler handler) = 0;
        virtual void ClearHandlers() = 0;

        virtual std::unordered_map<ClientID, ClientInfo> GetClients() const = 0;
        virtual NetworkStats GetStats() const = 0;
        virtual void KickClient(ClientID client, const std::string& reason) = 0;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
