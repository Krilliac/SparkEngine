/**
 * @file InstabilitySimulator.cpp
 * @brief Implementation of artificial network instability injection
 */

#include "InstabilitySimulator.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <random>

namespace Spark::Net
{

    // ========================================================================
    // Construction
    // ========================================================================

    InstabilitySimulator::InstabilitySimulator()
    {
        // Seed the RNG state from std::random_device
        std::random_device rd;
        m_rngState[0] = (static_cast<uint64_t>(rd()) << 32) | rd();
        m_rngState[1] = (static_cast<uint64_t>(rd()) << 32) | rd();

        // Ensure state is not all zeros (xoshiro requirement)
        if (m_rngState[0] == 0 && m_rngState[1] == 0)
        {
            m_rngState[0] = 0xDEADBEEFCAFEBABEULL;
            m_rngState[1] = 0x0123456789ABCDEFULL;
        }
    }

    // ========================================================================
    // RNG (xoshiro128+ variant for speed)
    // ========================================================================

    float InstabilitySimulator::RandomFloat()
    {
        // xoshiro128+ step
        uint64_t s0 = m_rngState[0];
        uint64_t s1 = m_rngState[1];
        uint64_t result = s0 + s1;

        s1 ^= s0;
        m_rngState[0] = ((s0 << 24) | (s0 >> 40)) ^ s1 ^ (s1 << 16);
        m_rngState[1] = (s1 << 37) | (s1 >> 27);

        // Convert to float in [0, 1)
        return static_cast<float>(result >> 40) / static_cast<float>(1ULL << 24);
    }

    // ========================================================================
    // Settings
    // ========================================================================

    void InstabilitySimulator::SetSettings(const InstabilitySettings& settings)
    {
        std::lock_guard lock(m_mutex);
        m_settings = settings;

        // Clamp percentages to valid range
        m_settings.packetLossPercent = std::clamp(m_settings.packetLossPercent, 0.0f, 100.0f);
        m_settings.reorderPercent = std::clamp(m_settings.reorderPercent, 0.0f, 100.0f);
        m_settings.latencyMs = std::max(m_settings.latencyMs, 0.0f);
        m_settings.jitterMs = std::max(m_settings.jitterMs, 0.0f);
    }

    const InstabilitySettings& InstabilitySimulator::GetSettings() const
    {
        // No lock needed — InstabilitySettings is small and read atomically enough
        // for diagnostic purposes. The send path locks before calling decision methods.
        return m_settings;
    }

    // ========================================================================
    // Decision Methods
    // ========================================================================

    bool InstabilitySimulator::ShouldDropPacket()
    {
        std::lock_guard lock(m_mutex);

        if (!m_settings.enabled || m_settings.packetLossPercent <= 0.0f)
        {
            return false;
        }

        float roll = RandomFloat() * 100.0f;
        return roll < m_settings.packetLossPercent;
    }

    float InstabilitySimulator::GetDelayMs()
    {
        std::lock_guard lock(m_mutex);

        if (!m_settings.enabled)
        {
            return 0.0f;
        }

        float delay = m_settings.latencyMs;

        if (m_settings.jitterMs > 0.0f)
        {
            // Random jitter in [-jitterMs, +jitterMs]
            float jitter = (RandomFloat() * 2.0f - 1.0f) * m_settings.jitterMs;
            delay += jitter;
        }

        return std::max(delay, 0.0f);
    }

    bool InstabilitySimulator::ShouldReorder()
    {
        std::lock_guard lock(m_mutex);

        if (!m_settings.enabled || m_settings.reorderPercent <= 0.0f)
        {
            return false;
        }

        float roll = RandomFloat() * 100.0f;
        return roll < m_settings.reorderPercent;
    }

    // ========================================================================
    // Packet Queue
    // ========================================================================

    void InstabilitySimulator::QueuePacket(std::vector<uint8_t> data, float sendTimeMs)
    {
        std::lock_guard lock(m_mutex);

        DelayedPacket packet;
        packet.data = std::move(data);
        packet.deliveryTimeMs = sendTimeMs;

        // Insert sorted by delivery time for efficient retrieval
        auto insertPos =
            std::lower_bound(m_delayedQueue.begin(), m_delayedQueue.end(), sendTimeMs,
                             [](const DelayedPacket& pkt, float time) { return pkt.deliveryTimeMs < time; });

        m_delayedQueue.insert(insertPos, std::move(packet));
    }

    std::vector<std::vector<uint8_t>> InstabilitySimulator::GetReadyPackets(float currentTimeMs)
    {
        std::lock_guard lock(m_mutex);

        std::vector<std::vector<uint8_t>> ready;

        // Since the queue is sorted by delivery time, we can stop at the first
        // packet that isn't ready yet.
        while (!m_delayedQueue.empty() && m_delayedQueue.front().deliveryTimeMs <= currentTimeMs)
        {
            ready.push_back(std::move(m_delayedQueue.front().data));
            m_delayedQueue.erase(m_delayedQueue.begin());
        }

        return ready;
    }

    // ========================================================================
    // Console / Status
    // ========================================================================

    std::string InstabilitySimulator::Console_GetStatus() const
    {
        std::lock_guard lock(m_mutex);

        std::string status;
        status += std::format("InstabilitySimulator: {}\n", m_settings.enabled ? "ENABLED" : "disabled");

        if (m_settings.enabled)
        {
            status += std::format("  Latency:     {:.1f} ms\n", m_settings.latencyMs);
            status += std::format("  Jitter:      +/-{:.1f} ms\n", m_settings.jitterMs);
            status += std::format("  Packet loss: {:.1f}%\n", m_settings.packetLossPercent);
            status += std::format("  Reorder:     {:.1f}%\n", m_settings.reorderPercent);
            status += std::format("  Queued pkts: {}\n", m_delayedQueue.size());
        }

        return status;
    }

    // ========================================================================
    // Shutdown
    // ========================================================================

    void InstabilitySimulator::Shutdown()
    {
        std::lock_guard lock(m_mutex);
        m_delayedQueue.clear();
        m_settings = InstabilitySettings{};
    }

} // namespace Spark::Net
