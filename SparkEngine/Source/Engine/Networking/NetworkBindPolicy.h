/**
 * @file NetworkBindPolicy.h
 * @brief Fail-closed IPv4 bind and peer-admission policy for development networking.
 */

#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
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
        MissingPrefix,
        InvalidPrefix,
        DisallowedAddress,
        NetworkAddress,
        BroadcastAddress
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

            // Canonical dotted decimal has no alternate octal-looking form.
            if (end - begin > 1 && address[begin] == '0')
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

    /** @brief Whether an address is a concrete host in the fixed 127/8 loopback block. */
    [[nodiscard]] inline constexpr bool IsConcreteLoopbackUnicastAddress(uint32_t address) noexcept
    {
        return IsIPv4LoopbackAddress(address) && address != 0x7F000000u && address != 0x7FFFFFFFu;
    }

    /** @brief Whether a host-order IPv4 address is in RFC1918 private-use space. */
    [[nodiscard]] inline constexpr bool IsIPv4PrivateAddress(uint32_t address) noexcept
    {
        return (address & 0xFF000000u) == 0x0A000000u || (address & 0xFFF00000u) == 0xAC100000u ||
               (address & 0xFFFF0000u) == 0xC0A80000u;
    }

    /** @brief Host-order subnet mask, or no value when the prefix is outside 0..32. */
    [[nodiscard]] inline constexpr std::optional<uint32_t> IPv4PrefixMask(uint8_t prefixLength) noexcept
    {
        if (prefixLength > 32u)
            return std::nullopt;
        return prefixLength == 0u ? 0u : (0xFFFFFFFFu << (32u - prefixLength));
    }

    /** @brief Minimum prefix that keeps a subnet wholly inside its RFC1918 allocation. */
    [[nodiscard]] inline constexpr uint8_t MinimumPrivatePrefix(uint32_t address) noexcept
    {
        if ((address & 0xFF000000u) == 0x0A000000u)
            return 8u;
        if ((address & 0xFFF00000u) == 0xAC100000u)
            return 12u;
        if ((address & 0xFFFF0000u) == 0xC0A80000u)
            return 16u;
        return 0u;
    }

    /** @brief Whether an RFC1918 address/prefix denotes a concrete broadcast-capable LAN host. */
    [[nodiscard]] inline constexpr bool IsConcretePrivateUnicastAddress(uint32_t address,
                                                                         uint8_t prefixLength) noexcept
    {
        const uint8_t minimumPrefix = MinimumPrivatePrefix(address);
        if (minimumPrefix == 0u || prefixLength < minimumPrefix || prefixLength > 30u)
            return false;

        const auto maskValue = IPv4PrefixMask(prefixLength);
        if (!maskValue)
            return false;
        const uint32_t mask = *maskValue;
        const uint32_t network = address & mask;
        const uint32_t broadcast = network | ~mask;
        return IsIPv4PrivateAddress(network) && IsIPv4PrivateAddress(broadcast) && address != network &&
               address != broadcast;
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
            return IsConcreteLoopbackUnicastAddress(address)
                       ? NetworkEndpointPolicy(address, 8u, NetworkPeerScope::LoopbackOnly,
                                               NetworkEndpointPolicyError::None)
                       : Invalid(NetworkEndpointPolicyError::DisallowedAddress);
        }

        [[nodiscard]] static constexpr NetworkEndpointPolicy PrivateLan(uint32_t address, uint8_t prefixLength) noexcept
        {
            return IsConcretePrivateUnicastAddress(address, prefixLength)
                       ? NetworkEndpointPolicy(address, prefixLength, NetworkPeerScope::PrivateLan,
                                               NetworkEndpointPolicyError::None)
                       : Invalid(NetworkEndpointPolicyError::DisallowedAddress);
        }

        [[nodiscard]] static constexpr NetworkEndpointPolicy Invalid(NetworkEndpointPolicyError error) noexcept
        {
            return NetworkEndpointPolicy(0x7F000001u, 8u, NetworkPeerScope::LoopbackOnly, error);
        }

        [[nodiscard]] constexpr bool IsValid() const noexcept { return m_error == NetworkEndpointPolicyError::None; }

        [[nodiscard]] constexpr uint32_t BindAddress() const noexcept { return m_bindAddress; }
        [[nodiscard]] constexpr uint8_t SubnetPrefixLength() const noexcept { return m_prefixLength; }
        [[nodiscard]] constexpr uint32_t NetworkAddress() const noexcept
        {
            const auto mask = IPv4PrefixMask(m_prefixLength);
            return mask ? (m_bindAddress & *mask) : 0u;
        }
        [[nodiscard]] constexpr uint32_t BroadcastAddress() const noexcept
        {
            const auto mask = IPv4PrefixMask(m_prefixLength);
            return mask ? (NetworkAddress() | ~*mask) : 0u;
        }
        [[nodiscard]] constexpr NetworkPeerScope PeerScope() const noexcept { return m_peerScope; }
        [[nodiscard]] constexpr NetworkEndpointPolicyError Error() const noexcept { return m_error; }

        /** @brief Whether a host-order IPv4 peer can cross this endpoint boundary. */
        [[nodiscard]] constexpr bool AllowsPeerAddress(uint32_t address) const noexcept
        {
            if (!IsValid())
                return false;
            if (m_peerScope == NetworkPeerScope::LoopbackOnly)
                return IsConcreteLoopbackUnicastAddress(address);
            const auto mask = IPv4PrefixMask(m_prefixLength);
            return mask && IsConcretePrivateUnicastAddress(address, m_prefixLength) &&
                   (address & *mask) == NetworkAddress();
        }

      private:
        constexpr NetworkEndpointPolicy(uint32_t bindAddress, uint8_t prefixLength, NetworkPeerScope peerScope,
                                        NetworkEndpointPolicyError error) noexcept
            : m_bindAddress(bindAddress), m_prefixLength(prefixLength), m_peerScope(peerScope), m_error(error)
        {
        }

        uint32_t m_bindAddress = 0x7F000001u;
        uint8_t m_prefixLength = 8u;
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

        const size_t slash = configuredAddress.find('/');
        const std::string_view addressText = configuredAddress.substr(0, slash);
        uint32_t address = 0;
        if (!ParseIPv4Address(addressText, address))
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::NonNumericAddress);
        if (IsIPv4LoopbackAddress(address))
            return slash == std::string_view::npos
                       ? NetworkEndpointPolicy::Loopback(address)
                       : NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::InvalidPrefix);
        if (!IsIPv4PrivateAddress(address))
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::DisallowedAddress);
        if (slash == std::string_view::npos)
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::MissingPrefix);

        const std::string_view prefixText = configuredAddress.substr(slash + 1u);
        if (prefixText.empty() || prefixText.find('/') != std::string_view::npos ||
            (prefixText.size() > 1u && prefixText.front() == '0'))
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::InvalidPrefix);
        unsigned int prefixLength = 0;
        const auto prefixResult =
            std::from_chars(prefixText.data(), prefixText.data() + prefixText.size(), prefixLength);
        if (prefixResult.ec != std::errc{} || prefixResult.ptr != prefixText.data() + prefixText.size() ||
            prefixLength > 30u || prefixLength < MinimumPrivatePrefix(address))
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::InvalidPrefix);

        const auto maskValue = IPv4PrefixMask(static_cast<uint8_t>(prefixLength));
        if (!maskValue)
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::InvalidPrefix);
        const uint32_t mask = *maskValue;
        const uint32_t network = address & mask;
        const uint32_t broadcast = network | ~mask;
        if (!IsIPv4PrivateAddress(network) || !IsIPv4PrivateAddress(broadcast))
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::InvalidPrefix);
        if (address == network)
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::NetworkAddress);
        if (address == broadcast)
            return NetworkEndpointPolicy::Invalid(NetworkEndpointPolicyError::BroadcastAddress);
        if (IsConcretePrivateUnicastAddress(address, static_cast<uint8_t>(prefixLength)))
            return NetworkEndpointPolicy::PrivateLan(address, static_cast<uint8_t>(prefixLength));
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
            return "bind address must be 'loopback', a canonical loopback IPv4 address, or canonical IPv4 CIDR";
        case NetworkEndpointPolicyError::MissingPrefix:
            return "an RFC1918 LAN bind requires an explicit subnet prefix (for example 192.168.1.20/24)";
        case NetworkEndpointPolicyError::InvalidPrefix:
            return "LAN prefix must be canonical, keep the subnet inside RFC1918 space, and leave host space "
                   "(/30 or shorter)";
        case NetworkEndpointPolicyError::DisallowedAddress:
            return "bind address must be loopback or a concrete RFC1918 unicast CIDR; wildcard, public, test, "
                   "multicast, and CGNAT addresses are forbidden";
        case NetworkEndpointPolicyError::NetworkAddress:
            return "bind address is the configured subnet network address";
        case NetworkEndpointPolicyError::BroadcastAddress:
            return "bind address is the configured subnet broadcast address";
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
