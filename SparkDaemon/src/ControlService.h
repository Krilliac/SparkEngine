/**
 * @file ControlService.h
 * @brief Built-in ping/version/shutdown/stats service. Always registered.
 */

#pragma once

#include "ServiceBase.h"

#include <atomic>
#include <functional>

namespace Spark::Daemon
{

    /**
     * @brief Provides the live DaemonStats reply for `StatsRequest`.
     *
     * The server injects this at construction so `ControlService` does not
     * carry a back-pointer to `DaemonServer` (keeps the dependency graph
     * one-directional: server → service, not the other way).
     */
    using DaemonStatsProvider = std::function<DaemonStats()>;

    /**
     * @brief Handles Control service messages (ping, version, shutdown, stats).
     *
     * Holds a reference to an `std::atomic<bool>` "should-stop" flag owned by
     * the server; a `ShutdownRequest` sets the flag so the server's accept
     * loop exits.
     */
    class ControlService final : public ServiceBase
    {
      public:
        ControlService(std::atomic<bool>& shouldStop, DaemonStatsProvider statsProvider = {}) noexcept
            : m_shouldStop(shouldStop), m_statsProvider(std::move(statsProvider))
        {
        }

        [[nodiscard]] ServiceId GetServiceId() const noexcept override { return ServiceId::Control; }
        [[nodiscard]] const char* GetName() const noexcept override { return "control"; }

        std::optional<ServiceResponse> HandleMessage(uint16_t messageType,
                                                     const std::vector<uint8_t>& payload) override;

      private:
        std::atomic<bool>& m_shouldStop;
        DaemonStatsProvider m_statsProvider;
    };

} // namespace Spark::Daemon
