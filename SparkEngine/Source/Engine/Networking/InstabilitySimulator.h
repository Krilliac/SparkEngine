/**
 * @file InstabilitySimulator.h
 * @brief Artificial network instability injection for testing and debugging
 * @author Spark Engine Team
 * @date 2026
 *
 * Wraps UDP send/receive paths to inject configurable artificial latency,
 * jitter, packet loss, and reordering. Enable via console commands
 * (net.lag, net.loss, net.jitter, net.reorder) during development to
 * stress-test netcode under adverse conditions.
 */

#pragma once
#include "../../Core/Platform.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Spark::Net
{

    // ========================================================================
    // Instability Settings
    // ========================================================================

    /**
     * @brief Configuration for artificial network instability.
     *
     * All values are safe at their defaults (zero / disabled). Set 'enabled'
     * to true and adjust individual parameters to begin injecting instability.
     */
    struct InstabilitySettings
    {
        float latencyMs = 0.0f;         ///< Base added latency in milliseconds
        float jitterMs = 0.0f;          ///< +/- variance on latency in milliseconds
        float packetLossPercent = 0.0f; ///< Chance of dropping a packet (0-100)
        float reorderPercent = 0.0f;    ///< Chance of reordering a packet (0-100)
        bool enabled = false;           ///< Master toggle — no effects when false
    };

    // ========================================================================
    // InstabilitySimulator
    // ========================================================================

    /**
     * @brief Singleton that injects artificial network instability for testing.
     *
     * Insert this into the packet send/receive path:
     * @code
     *   auto& sim = InstabilitySimulator::GetInstance();
     *   if (sim.GetSettings().enabled)
     *   {
     *       if (sim.ShouldDropPacket()) return; // simulated loss
     *       float delay = sim.GetDelayMs();
     *       sim.QueuePacket(std::move(data), currentTimeMs + delay);
     *   }
     *   // Later, in the send loop:
     *   auto ready = sim.GetReadyPackets(currentTimeMs);
     *   for (auto& pkt : ready) { actualSend(pkt); }
     * @endcode
     */
    class InstabilitySimulator
    {
      public:
        /// @brief Get the singleton instance
        static InstabilitySimulator& GetInstance()
        {
            static InstabilitySimulator instance;
            return instance;
        }

        InstabilitySimulator(const InstabilitySimulator&) = delete;
        InstabilitySimulator& operator=(const InstabilitySimulator&) = delete;

        /// @brief Apply new instability settings
        /// @param settings  Configuration to apply
        void SetSettings(const InstabilitySettings& settings);

        /// @brief Get the current instability settings
        /// @return Current settings (read-only reference)
        const InstabilitySettings& GetSettings() const;

        /// @brief Check if a packet should be dropped based on packetLossPercent
        /// @return true if the packet should be discarded
        bool ShouldDropPacket();

        /// @brief Compute the delay to apply to a packet (latency + random jitter)
        /// @return Delay in milliseconds (>= 0)
        float GetDelayMs();

        /// @brief Check if a packet should be reordered based on reorderPercent
        /// @return true if the packet should be delayed for reordering
        bool ShouldReorder();

        /// @brief Queue a packet for delayed delivery
        /// @param data        Raw packet bytes
        /// @param sendTimeMs  Absolute time (ms) when the packet should be released
        void QueuePacket(std::vector<uint8_t> data, float sendTimeMs);

        /// @brief Retrieve all packets whose delivery time has passed
        /// @param currentTimeMs  Current time in milliseconds
        /// @return Vector of packet data buffers ready for actual transmission
        std::vector<std::vector<uint8_t>> GetReadyPackets(float currentTimeMs);

        /// @brief Get a human-readable status string for console display
        /// @return Formatted status string
        std::string Console_GetStatus() const;

        /// @brief Release all queued packets and reset state
        void Shutdown();

      private:
        InstabilitySimulator();

        /// A packet waiting for its scheduled delivery time
        struct DelayedPacket
        {
            DelayedPacket() = default;
            DelayedPacket(const DelayedPacket&) = delete;
            DelayedPacket& operator=(const DelayedPacket&) = delete;
            DelayedPacket(DelayedPacket&&) noexcept = default;
            DelayedPacket& operator=(DelayedPacket&& other) noexcept;
            ~DelayedPacket();

            std::vector<uint8_t> data;
            float deliveryTimeMs = 0.0f; ///< Absolute time when this packet should be sent
        };

        mutable std::mutex m_mutex;
        InstabilitySettings m_settings;
        std::vector<DelayedPacket> m_delayedQueue;

        // RNG state (mt19937 seeded at construction)
        class RngImpl;
        struct RngState;
        // Stored as raw floats for uniform distribution — see .cpp
        uint64_t m_rngState[2] = {}; ///< Xoshiro128+ state (compact, no heap allocation)

        /// @brief Generate a random float in [0, 1)
        float RandomFloat();
    };

} // namespace Spark::Net
