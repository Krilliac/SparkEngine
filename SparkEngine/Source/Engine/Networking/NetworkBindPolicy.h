/**
 * @file NetworkBindPolicy.h
 * @brief Fail-closed IPv4 bind and peer-admission policy for development networking.
 */

#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

namespace Spark::Net
{
    /** @brief The only remote address classes available before authenticated transport exists. */
    enum class NetworkPeerScope : uint8_t
    {
        LoopbackOnly,
        PrivateLan
    };

    /** @brief Why an explicitly requested endpoint policy was rejected. */
    enum class NetworkEndpointPolicyError : uint8_t
    {
        None,
        EmptyValue,
        NonNumericAddress,
        DisallowedAddress
    };

    /** @brief Parse a strict dotted-decimal IPv4 address into host byte order. */
    [[nodiscard]] inline bool ParseIPv4Address(std::string_view address, uint32_t& output) noexcept
    {
        if (address.empty())
            return false;

        uint32_t packed = 0;
        size_t begin = 0;
        for (int index = 0; index < 4; ++index)
        {
            const size_t separator = address.find('.', begin);
            const size_t end = separator == std::string_view::npos ? address.size() : separator;
            if (end == begin || (index < 3 && separator == std::string_view::npos) ||
                (index == 3 && separator != std::string_view::npos))
                return false;

            unsigned int octet = 0;
            const auto result = std::from_chars(address.data() + begin, address.data() + end, octet);
            if (result.ec != std::errc{} || result.ptr != address.data() + end || octet > 255u)
                return false;

            packed = (packed << 8u) | octet;
            begin = end + 1;
        }

        output = packed;
        return true;
    }

    /** @brief Whether a host-order IPv4 address belongs to 127/8. */
    [[nodiscard]] inline constexpr bool IsIPv4LoopbackAddress(uint32_t address) noexcept
    {
        return (address & 0xFF000000u) == 0x7F000000u;
    }

    /** @brief Whether a numeric IPv4 address belongs to the complete 127/8 loopback block. */
    [[nodiscard]] inline bool IsIPv4LoopbackAddress(std::string_view address) noexcept
    {
        uint32_t parsed = 0;
        return ParseIPv4Address(address, parsed) && IsIPv4LoopbackAddress(parsed);
    }

    /** @brief Whether a host-order IPv4 address is in RFC1918 private-use space. */
    [[nodiscard]] inline constexpr bool IsIPv4PrivateAddress(uint32_t address) noexcept
    {
        return (address & 0xFF000000u) == 0x0A000000u || (address & 0xFFF00000u) == 0xAC100000u ||
               (address & 0xFFFF0000u) == 0xC0A80000u;
    }

    /**
     * @brief Conservative unicast check for explicit RFC1918 endpoint values.
     *
     * Without an interface prefix length the exact subnet network/broadcast
     * addresses cannot be derived. Rejecting final octets 0 and 255 prevents
     * the common accidental network/broadcast requests and fails closed for
     * ambiguous addresses.
     */
    [[nodiscard]] inline constexpr bool IsConcretePrivateUnicastAddress(uint32_t address) noexcept
    {
        const uint32_t finalOctet = address & 0xFFu;
        return IsIPv4PrivateAddress(address) && finalOctet != 0u && finalOctet != 255u;
    }

    /**
     * @brief Captured endpoint boundary threaded unchanged through one socket lifecycle.
     *
     * Consumers can copy the value but cannot mutate its address, scope, or
     * validation result after construction.
     */
    class NetworkEndpointPolicy final
    {
      public:
        /** @brief Safe default: bind 127.0.0.1 and admit only 127/8 peers. */
        constexpr NetworkEndpointPolicy() noexcept = default;

        [[nodiscard]] static constexpr NetworkEndpointPolicy Loopback(uint32_t address = 0x7F000001u) noexcept
        {
            return IsIPv4LoopbackAddress(address) ? NetworkEndpointPolicy(address, NetworkPeerScope::LoopbackOnly,
                                                                          NetworkEndpointPolicyError::None)
                                                  : Invalid(NetworkEndpointPolicyError::DisallowedAddress);
        }

        [[nodiscard]] static constexpr NetworkEndpointPolicy PrivateLan(uint32_t address) noexcept
        {
            return IsConcretePrivateUnicastAddress(address)
                       ? NetworkEndpointPolicy(address, NetworkPeerScope::PrivateLan, NetworkEndpointPolicyError::None)
                       : Invalid(NetworkEndpointPolicyError::DisallowedAddress);
        }

        [[nodiscard]] static constexpr NetworkEndpointPolicy Invalid(NetworkEndpointPolicyError error) noexcept
        {
            return NetworkEndpointPolicy(0x7F000001u, NetworkPeerScope::LoopbackOnly, error);
        }

        [[nodiscard]] constexpr bool IsValid() const noexcept { return m_error == NetworkEndpointPolicyError::None; }

        [[nodiscard]] constexpr uint32_t BindAddress() const noexcept { return m_bindAddress; }
        [[nodiscard]] constexpr NetworkPeerScope PeerScope() const noexcept { return m_peerScope; }
        [[nodiscard]] constexpr NetworkEndpointPolicyError Error() const noexcept { return m_error; }

        /** @brief Whether a host-order IPv4 peer can cross this endpoint boundary. */
        [[nodiscard]] constexpr bool AllowsPeerAddress(uint32_t address) const noexcept
        {
            if (!IsValid())
                return false;
            return m_peerScope == NetworkPeerScope::LoopbackOnly ? IsIPv4LoopbackAddress(address)
                                                                 : IsConcretePrivateUnicastAddress(address);
        }

      private:
        constexpr NetworkEndpointPolicy(uint32_t bindAddress, NetworkPeerScope peerScope,
                                        NetworkEndpointPolicyError error) noexcept
            : m_bindAddress(bindAddress), m_peerScope(peerScope), m_error(error)
        {
        }

        uint32_t m_bindAddress = 0x7F000001u;
        NetworkPeerScope m_peerScope = NetworkPeerScope::LoopbackOnly;
        NetworkEndpointPolicyError m_error = NetworkEndpointPolicyError::None;
    };

    /** @brief Resolve one explicit value without consulting mutable process state. */
    [[nodiscard]] inline NetworkEndpointPolicy ResolveNetworkEndpointPolicy(std::string_view configuredAddress) noexcept
    {
        if (configuredAddress.empty())
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::EmptyValue);
        if (configuredAddress == "local" || configuredAddress == "loopback" || configuredAddress == "localhost")
            return NetworkEndpointPolicy::Loopback();

        uint32_t address = 0;
        if (!ParseIPv4Address(configuredAddress, address))
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::NonNumericAddress);
        if (IsIPv4LoopbackAddress(address))
            return NetworkEndpointPolicy::Loopback(address);
        if (IsConcretePrivateUnicastAddress(address))
            return NetworkEndpointPolicy::PrivateLan(address);
        return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::DisallowedAddress);
    }

    /** @brief Null means no request and therefore the safe loopback default. */
    [[nodiscard]] inline NetworkEndpointPolicy ResolveNetworkEndpointPolicy(const char* configuredAddress) noexcept
    {
        return configuredAddress == nullptr ? NetworkEndpointPolicy{}
                                            : ResolveNetworkEndpointPolicy(std::string_view{configuredAddress});
    }

    /**
     * @brief Capture the process bind request once for a new endpoint lifecycle.
     *
     * SPARK_NETWORK_BIND_ADDRESS is canonical. SPARK_NETWORK_BIND_MODE remains
     * a migration input, but its old wildcard values now produce an invalid
     * policy and make startup fail rather than opening every interface.
     */
    [[nodiscard]] inline NetworkEndpointPolicy CaptureNetworkEndpointPolicy() noexcept
    {
        if (const char* address = std::getenv("SPARK_NETWORK_BIND_ADDRESS"))
            return ResolveNetworkEndpointPolicy(address);
        return ResolveNetworkEndpointPolicy(std::getenv("SPARK_NETWORK_BIND_MODE"));
    }

    /** @brief Stable, actionable diagnostic for rejected endpoint requests. */
    [[nodiscard]] inline std::string_view NetworkEndpointPolicyErrorText(NetworkEndpointPolicyError error) noexcept
    {
        switch (error)
        {
        case NetworkEndpointPolicyError::None:
            return "";
        case NetworkEndpointPolicyError::EmptyValue:
            return "bind address is empty";
        case NetworkEndpointPolicyError::NonNumericAddress:
            return "bind address must be 'loopback' or a numeric IPv4 address";
        case NetworkEndpointPolicyError::DisallowedAddress:
            return "bind address must be loopback or a concrete RFC1918 unicast address; wildcard, public, test, "
                   "multicast, broadcast, and CGNAT addresses are forbidden";
        }
        return "bind address policy is invalid";
    }

    /** @brief Render a host-order IPv4 address for logs and status output. */
    [[nodiscard]] inline std::string FormatIPv4Address(uint32_t address)
    {
        return std::to_string((address >> 24u) & 0xFFu) + "." + std::to_string((address >> 16u) & 0xFFu) + "." +
               std::to_string((address >> 8u) & 0xFFu) + "." + std::to_string(address & 0xFFu);
    }
} // namespace Spark::Net
