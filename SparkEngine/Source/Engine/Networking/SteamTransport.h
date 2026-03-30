/**
 * @file SteamTransport.h
 * @brief Stub ITransport for future Steam Networking Sockets integration
 * @author Spark Engine Team
 * @date 2025
 *
 * Placeholder transport that will wrap the Steamworks
 * ISteamNetworkingSockets API once the Steam SDK is integrated.
 * Currently all methods return failure / no-op so the engine
 * compiles and can be tested without the Steam SDK.
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 */

#pragma once
#include "ITransport.h"

#ifdef ENABLE_NETWORKING

namespace Spark::Net
{

    /// @brief Stub transport for Steam Networking Sockets (not yet implemented).
    ///
    /// When the Steamworks SDK is available, this class will use
    /// ISteamNetworkingSockets for relay-based, NAT-traversing packet I/O.
    /// Until then every call gracefully fails so the rest of the engine
    /// can reference SteamTransport without link errors.
    class SteamTransport final : public ITransport
    {
      public:
        SteamTransport() = default;
        ~SteamTransport() override = default;

        // Non-copyable
        SteamTransport(const SteamTransport&) = delete;
        SteamTransport& operator=(const SteamTransport&) = delete;

        bool Initialize(uint16_t /*port*/) override
        {
            // Stub: returns false until Steamworks SDK is linked.
            // With SDK: SteamNetworkingSockets()->InitAuthentication() on the given port.
            return false;
        }

        void Shutdown() override
        {
            // Stub: no-op until Steamworks SDK is linked.
            // With SDK: close all HSteamNetConnection handles and release sockets interface.
        }

        bool Send(const uint8_t* /*data*/, size_t /*size*/, const std::string& /*address*/, uint16_t /*port*/) override
        {
            // Stub: returns false until Steamworks SDK is linked.
            // With SDK: ISteamNetworkingSockets::SendMessageToConnection().
            return false;
        }

        int Receive(uint8_t* /*buffer*/, size_t /*bufferSize*/, std::string& /*fromAddress*/,
                    uint16_t& /*fromPort*/) override
        {
            // Stub: returns -1 until Steamworks SDK is linked.
            // With SDK: ISteamNetworkingSockets::ReceiveMessagesOnConnection().
            return -1;
        }

        bool IsReady() const override { return false; }

        std::string GetTransportName() const override { return "Steam"; }
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
