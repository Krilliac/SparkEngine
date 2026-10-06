/**
 * @file NetworkIntegration.h
 * @brief Composes a pluggable ITransport with a connection-token registry
 *
 * Experimental helper that selects a transport backend and owns a
 * single-use connection-token registry. It is not the active NetworkManager
 * wire path and applies no packet encryption: it must not be treated as
 * authenticated or production-secure networking.
 *
 * ## Transport layer
 * ```
 *   NetworkManager
 *        |
 *   ITransport (abstract)
 *        |
 *   +--- UDPTransport (raw UDP sockets)
 *   +--- SteamTransport (Steamworks P2P)
 * ```
 *
 * ## Connection tokens
 * ```
 *   GenerateConnectionToken: CSPRNG token, fails closed on entropy failure
 *   ValidateToken:           constant-time, single-use, expiring; rejects
 *                            everything when the stack is not initialized
 * ```
 * Tokens are not peer authentication and do not protect packet contents.
 *
 * @see NetworkManager.h, ITransport.h, NetworkSecurity.h
 */

#pragma once

#ifdef ENABLE_NETWORKING

#include "ITransport.h"
#include "UDPTransport.h"
#include "SteamTransport.h"
#include "NetworkSecurity.h"

#include <atomic>
#include <memory>
#include <string>
#include <sstream>
#include "Utils/LogMacros.h"

namespace Spark::Net
{

    /**
     * @brief Configuration for the integrated network stack.
     */
    struct NetworkStackConfig
    {
        /// Transport backend to use
        enum class TransportType
        {
            UDP,
            Steam
        };

        TransportType transport = TransportType::UDP;

        /// Server address for UDP transport
        std::string serverAddress = "127.0.0.1";
        uint16_t serverPort = 27015;
    };

    /**
     * @brief Composes a transport with a connection-token registry.
     *
     * Provides a unified interface for creating transport instances and
     * issuing connection tokens. This class does not provide confidentiality,
     * integrity, or peer authentication.
     */
    class NetworkStack
    {
      public:
        NetworkStack() = default;
        ~NetworkStack() = default;

        // Non-copyable
        NetworkStack(const NetworkStack&) = delete;
        NetworkStack& operator=(const NetworkStack&) = delete;

        /**
         * @brief Initialize the network stack with the given configuration.
         */
        bool Initialize(const NetworkStackConfig& config)
        {
            if (m_initialized)
                return true;

            m_config = config;

            // Create transport based on configuration
            switch (config.transport)
            {
            case NetworkStackConfig::TransportType::UDP:
                m_transport = std::make_unique<UDPTransport>();
                break;
            case NetworkStackConfig::TransportType::Steam:
                m_transport = std::make_unique<SteamTransport>();
                break;
            }

            if (!m_transport)
                return false;

            if (!m_transport->Initialize(config.serverPort))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Network,
                                "NetworkStack::Initialize failed: transport '%s' could not initialize on port %u",
                                m_transport->GetTransportName().c_str(), static_cast<unsigned>(config.serverPort));
                m_transport.reset();
                m_initialized = false;
                return false;
            }

            m_tokens = std::make_unique<NetworkSecurity>();
            m_initialized = true;
            return true;
        }

        /**
         * @brief Shut down the network stack.
         */
        void Shutdown()
        {
            if (!m_initialized)
                return;

            if (m_transport)
            {
                m_transport->Shutdown();
            }

            m_tokens.reset();
            m_transport.reset();
            m_initialized = false;
        }

        /**
         * @brief Issue a single-use connection token from the OS CSPRNG.
         * @param outToken Receives the token; zeroed on failure.
         * @return false if the stack is not initialized or the CSPRNG failed.
         */
        [[nodiscard]] bool GenerateConnectionToken(NetworkSecurity::Token& outToken)
        {
            if (!m_tokens)
            {
                outToken.fill(0);
                return false;
            }
            return m_tokens->GenerateConnectionToken(outToken);
        }

        /**
         * @brief Match and consume a token this stack issued.
         * @return false for unknown, reused, or expired tokens, and always
         *         false while the stack is not initialized (fail closed).
         */
        [[nodiscard]] bool ValidateToken(const NetworkSecurity::Token& token)
        {
            return m_tokens && m_tokens->ValidateConnectionToken(token);
        }

        // -- Accessors --

        ITransport* GetTransport() { return m_transport.get(); }
        bool IsInitialized() const { return m_initialized; }
        const NetworkStackConfig& GetConfig() const { return m_config; }

        /**
         * @brief Console integration: network stack status.
         */
        std::string Console_GetStatus() const
        {
            std::stringstream ss;
            ss << "=== Network Stack ===\n";
            ss << "  Initialized:    " << (m_initialized ? "YES" : "NO") << "\n";
            ss << "  Transport:      ";
            switch (m_config.transport)
            {
            case NetworkStackConfig::TransportType::UDP:
                ss << "UDP";
                break;
            case NetworkStackConfig::TransportType::Steam:
                ss << "Steam P2P";
                break;
            }
            ss << "\n";
            ss << "  Ready:          " << ((m_transport && m_transport->IsReady()) ? "YES" : "NO") << "\n";
            ss << "  Server:         " << m_config.serverAddress << ":" << m_config.serverPort << "\n";
            return ss.str();
        }

      private:
        std::unique_ptr<ITransport> m_transport;
        std::unique_ptr<NetworkSecurity> m_tokens;
        NetworkStackConfig m_config;
        std::atomic<bool> m_initialized{false};
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
