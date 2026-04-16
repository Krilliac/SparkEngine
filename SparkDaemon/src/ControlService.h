/**
 * @file ControlService.h
 * @brief Built-in ping/version/shutdown service. Always registered.
 */

#pragma once

#include "ServiceBase.h"

#include <atomic>

namespace Spark::Daemon
{

    /**
     * @brief Handles Control service messages (ping, version, shutdown).
     *
     * Holds a reference to an `std::atomic<bool>` "should-stop" flag owned by
     * the server; a `ShutdownRequest` sets the flag so the server's accept
     * loop exits.
     */
    class ControlService final : public ServiceBase
    {
      public:
        explicit ControlService(std::atomic<bool>& shouldStop) noexcept : m_shouldStop(shouldStop) {}

        [[nodiscard]] ServiceId GetServiceId() const noexcept override { return ServiceId::Control; }
        [[nodiscard]] const char* GetName() const noexcept override { return "control"; }

        std::optional<ServiceResponse> HandleMessage(uint16_t messageType,
                                                     const std::vector<uint8_t>& payload) override;

      private:
        std::atomic<bool>& m_shouldStop;
    };

} // namespace Spark::Daemon
