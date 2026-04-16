/**
 * @file ServiceBase.h
 * @brief Polymorphic interface for a pluggable daemon service.
 *
 * A service handles every message addressed to its `ServiceId`. The server
 * owns a map of `ServiceId → unique_ptr<ServiceBase>` and calls
 * `HandleMessage` on the registered service for each incoming frame.
 *
 * Services should not reach across to other services directly — cross-service
 * composition happens above this layer (e.g. the asset service might call
 * the shader service via its own client handle).
 */

#pragma once

#include "Utils/DaemonProtocol.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace Spark::Daemon
{

    /// Response a service chooses to send back to the client. Return
    /// `std::nullopt` for one-way / notification messages with no reply.
    struct ServiceResponse
    {
        uint16_t messageType = 0;
        std::vector<uint8_t> payload;
    };

    /**
     * @brief Base class for a daemon-side service.
     *
     * Implementations must be thread-safe: the server may dispatch messages
     * from multiple connection threads concurrently.
     */
    class ServiceBase
    {
      public:
        virtual ~ServiceBase() = default;

        /// The service ID this implementation handles.
        [[nodiscard]] virtual ServiceId GetServiceId() const noexcept = 0;

        /// Short name for logging (e.g. "control", "asset").
        [[nodiscard]] virtual const char* GetName() const noexcept = 0;

        /**
         * @brief Handle an incoming request frame.
         *
         * @param messageType  Request message type (service-specific enum).
         * @param payload      Request payload bytes.
         * @return             Optional response. `std::nullopt` = no reply.
         */
        virtual std::optional<ServiceResponse> HandleMessage(uint16_t messageType,
                                                             const std::vector<uint8_t>& payload) = 0;
    };

} // namespace Spark::Daemon
