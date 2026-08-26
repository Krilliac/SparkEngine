/**
 * @file NetworkBindPolicy.h
 * @brief Process-local policy for confining development and test listeners.
 */

#pragma once

#include <cstdlib>
#include <string_view>

namespace Spark::Net
{
    /**
     * @brief Whether sockets should bind only to the IPv4 loopback interface.
     *
     * Production listeners remain reachable on all interfaces by default. Test
     * and development tools can set SPARK_NETWORK_BIND_MODE=loopback before any
     * socket is created, avoiding public listeners and Windows Firewall prompts.
     */
    [[nodiscard]] inline bool IsLoopbackBindMode(std::string_view value) noexcept
    {
        return value == "loopback" || value == "localhost" || value == "127.0.0.1";
    }

    [[nodiscard]] inline bool UseLoopbackNetworkBind() noexcept
    {
        const char* value = std::getenv("SPARK_NETWORK_BIND_MODE");
        return value != nullptr && IsLoopbackBindMode(value);
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
        const char* value = std::getenv("SPARK_NETWORK_BIND_MODE");
        return value == nullptr || ShouldUseLoopbackForUnauthenticatedTool(value);
    }
} // namespace Spark::Net
