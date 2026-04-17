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
 *
 * @warning **Framework stub — no Steamworks SDK linked.**
 *          `Initialize()` always returns `false`, `Send()` always returns
 *          `false`, `Receive()` always returns `-1`, and `IsReady()`
 *          always returns `false`. Selecting `TransportType::Steam` in
 *          `NetworkStackConfig` therefore produces a non-functional
 *          `NetworkStack` — `NetworkStack::Initialize()` will still
 *          succeed but the transport cannot open or process sockets.
 *          Prefer `TransportType::UDP` until the Steamworks SDK is
 *          integrated. The class is kept compilable so that
 *          `NetworkStackConfig::TransportType::Steam` and the
 *          corresponding `case` in `NetworkIntegration.h:104-106` can
 *          remain part of the public API — when a future session adds
 *          `steamworks_sdk` to `ThirdParty/` and gates it behind a new
 *          `ENABLE_STEAMWORKS` CMake flag, the inline bodies below will
 *          be replaced with real `SteamNetworkingSockets()` calls and
 *          every existing consumer becomes a live Steam client without
 *          any API surface changes.
 */

#pragma once
#include "ITransport.h"
#include "Utils/LogMacros.h"

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
            // Stub fallback when Steamworks SDK is not integrated in this build.
            SPARK_LOG_ERROR(Spark::LogCategory::Network,
                            "SteamTransport::Initialize failed: Steamworks SDK not linked. "
                            "Rebuild with ENABLE_STEAMWORKS and SteamNetworkingSockets support.");
            return false;
        }

        void Shutdown() override
        {
            // Stub: no-op until Steamworks SDK is linked.
            // With SDK: close all HSteamNetConnection handles and release sockets interface.
        }

        bool Send(const uint8_t* /*data*/, size_t /*size*/, const std::string& /*address*/, uint16_t /*port*/) override
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network,
                            "SteamTransport::Send failed: transport unavailable (Steamworks SDK not linked).");
            return false;
        }

        int Receive(uint8_t* /*buffer*/, size_t /*bufferSize*/, std::string& /*fromAddress*/,
                    uint16_t& /*fromPort*/) override
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network,
                            "SteamTransport::Receive failed: transport unavailable (Steamworks SDK not linked).");
            return -1;
        }

        bool IsReady() const override { return false; }

        std::string GetTransportName() const override { return "Steam"; }
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
