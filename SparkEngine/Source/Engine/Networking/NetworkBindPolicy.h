/**
 * @file NetworkBindPolicy.h
 * @brief Process-local policy for confining development and test listeners.
 */

#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace Spark::Net
{
    /** @brief Immutable interface scope selected before a socket is created. */
    enum class NetworkBindScope : uint8_t
    {
        LoopbackOnly,
        AllInterfaces
    };

    /** @brief Whether a configured value explicitly names IPv4 loopback. */
    [[nodiscard]] inline bool IsLoopbackBindMode(std::string_view value) noexcept
    {
        return value == "loopback" || value == "localhost" || value == "127.0.0.1";
    }

    /**
     * @brief Whether an explicitly configured mode requests all interfaces.
     *
     * This is intentionally strict: unauthenticated development protocols must
     * not become remotely reachable because of a typo or an unknown mode.
     */
    [[nodiscard]] inline bool IsAllInterfacesBindMode(std::string_view value) noexcept
    {
        return value == "all" || value == "any" || value == "public" || value == "0.0.0.0";
    }

    /**
     * @brief Resolve the fail-closed bind policy from an optional environment value.
     *
     * Unset, empty, and unknown values all select loopback. Only the explicit
     * all-interface values accepted by IsAllInterfacesBindMode open a listener
     * beyond the local machine.
     */
    [[nodiscard]] inline bool ShouldUseLoopbackNetworkBind(const char* configuredMode) noexcept
    {
        return configuredMode == nullptr || !IsAllInterfacesBindMode(configuredMode);
    }

    /** @brief Capture an immutable socket scope from one configuration value. */
    [[nodiscard]] inline NetworkBindScope ResolveNetworkBindScope(const char* configuredMode) noexcept
    {
        return ShouldUseLoopbackNetworkBind(configuredMode) ? NetworkBindScope::LoopbackOnly
                                                            : NetworkBindScope::AllInterfaces;
    }

    /** @brief Capture the process environment once for a new non-managed endpoint. */
    [[nodiscard]] inline NetworkBindScope CaptureNetworkBindScope() noexcept
    {
        return ResolveNetworkBindScope(std::getenv("SPARK_NETWORK_BIND_MODE"));
    }

    /** @brief Whether sockets should bind only to the IPv4 loopback interface. */
    [[nodiscard]] inline bool UseLoopbackNetworkBind() noexcept
    {
        return CaptureNetworkBindScope() == NetworkBindScope::LoopbackOnly;
    }

    /** @brief Whether a numeric IPv4 address belongs to the complete 127/8 loopback block. */
    [[nodiscard]] inline bool IsIPv4LoopbackAddress(std::string_view address) noexcept
    {
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
            if (result.ec != std::errc{} || result.ptr != address.data() + end || octet > 255u ||
                (index == 0 && octet != 127u))
                return false;
            begin = end + 1;
        }
        return true;
    }

    [[nodiscard]] inline bool ShouldUseLoopbackForUnauthenticatedTool(std::string_view configuredMode) noexcept
    {
        return !IsAllInterfacesBindMode(configuredMode);
    }

    /**
     * @brief Safe bind policy for legacy unauthenticated tool protocols.
     *
     * Loopback is the default. Remote/LAN access requires the explicit
     * SPARK_NETWORK_BIND_MODE=all (or an equivalent accepted value) opt-in.
     */
    [[nodiscard]] inline bool UseLoopbackNetworkBindForUnauthenticatedTool() noexcept
    {
        return ShouldUseLoopbackNetworkBind(std::getenv("SPARK_NETWORK_BIND_MODE"));
    }
} // namespace Spark::Net
