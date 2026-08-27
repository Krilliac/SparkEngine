/**
 * @file NetworkIntegration.h
 * @brief Cross-links ITransport and NetworkSecurity with NetworkManager
 *
 * Experimental helper that composes a pluggable transport with placeholder
 * security utilities. It is not the active NetworkManager wire path and must
 * not be treated as authenticated or production-secure networking.
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
 * ## Security layer
 * ```
 *   Outgoing: Serialize -> Encrypt (XOR) -> Send
 *   Incoming: Receive -> Decrypt (XOR) -> Deserialize
 *   Connection: Token-based with expiry validation
 * ```
 *
 * @see NetworkManager.h, ITransport.h, NetworkSecurity.h
 */

#pragma once

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

        /// Enable encryption
        bool enableEncryption = true;
    };

    /**
     * @brief Manages the complete network stack with transport and security.
     *
     * Provides a unified interface for creating transport instances,
     * applying security layers, and integrating with the NetworkManager.
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
                m_security.reset();
                m_initialized = false;
                return false;
            }

            // Initialize security layer (NetworkSecurity auto-generates its own key)
            if (config.enableEncryption)
            {
                m_security = std::make_unique<NetworkSecurity>();
            }

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

            m_security.reset();
            m_transport.reset();
            m_initialized = false;
        }

        /**
         * @brief Generate a connection token.
         */
        NetworkSecurity::Token GenerateConnectionToken()
        {
            if (m_security)
            {
                return m_security->GenerateConnectionToken();
            }
            return {};
        }

        /**
         * @brief Validate a connection token.
         */
        bool ValidateToken(const NetworkSecurity::Token& token)
        {
            if (m_security)
            {
                return m_security->ValidateConnectionToken(token);
            }
            return true;
        }

        /**
         * @brief Encrypt data before sending.
         */
        std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& data) const
        {
            if (m_security)
            {
                return NetworkSecurity::Encrypt(data, m_security->GetEncryptionKey());
            }
            return data;
        }

        /**
         * @brief Decrypt received data.
         */
        std::vector<uint8_t> Decrypt(const std::vector<uint8_t>& data) const
        {
            if (m_security)
            {
                return NetworkSecurity::Decrypt(data, m_security->GetEncryptionKey());
            }
            return data;
        }

        // -- Accessors --

        ITransport* GetTransport() { return m_transport.get(); }
        NetworkSecurity* GetSecurity() { return m_security.get(); }
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
            ss << "  Encryption:     " << (m_security ? "ENABLED" : "DISABLED") << "\n";
            ss << "  Server:         " << m_config.serverAddress << ":" << m_config.serverPort << "\n";
            return ss.str();
        }

      private:
        std::unique_ptr<ITransport> m_transport;
        std::unique_ptr<NetworkSecurity> m_security;
        NetworkStackConfig m_config;
        std::atomic<bool> m_initialized{false};
    };

} // namespace Spark::Net
