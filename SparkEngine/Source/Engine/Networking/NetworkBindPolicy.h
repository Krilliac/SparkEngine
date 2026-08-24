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
     * Production remains reachable on all interfaces by default. Test and
     * development tools can set SPARK_NETWORK_BIND_MODE=loopback before any
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
} // namespace Spark::Net
