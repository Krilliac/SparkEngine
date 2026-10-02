/**
 * @file InstabilitySimulator.cpp
 * @brief Implementation of artificial network instability injection
 */

#include "InstabilitySimulator.h"
#include "../../Core/EngineSettings.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/ScopeGuard.h"
#include "../../Utils/SecureMemory.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <random>
#include <utility>

namespace Spark::Net
{

    InstabilitySimulator::DelayedPacket& InstabilitySimulator::DelayedPacket::operator=(DelayedPacket&& other) noexcept
    {
        if (this == &other)
            return *this;
        Spark::SecureClear(data);
        data = std::move(other.data);
        deliveryTimeMs = other.deliveryTimeMs;
        sequence = other.sequence;
        lifecycleEpoch = other.lifecycleEpoch;
        destinationKey = other.destinationKey;
        localOnly = other.localOnly;
        return *this;
    }

    InstabilitySimulator::DelayedPacket::~DelayedPacket()
    {
        Spark::SecureClear(data);
    }

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
        m_settings.duplicatePercent = std::clamp(m_settings.duplicatePercent, 0.0f, 100.0f);
        m_settings.latencyMs = std::max(m_settings.latencyMs, 0.0f);
        m_settings.jitterMs = std::max(m_settings.jitterMs, 0.0f);
        m_settings.reorderHoldMs = std::max(m_settings.reorderHoldMs, 0.0f);
        if (m_settings.seed != 0)
        {
            // splitmix64 expansion of the seed into the two xoshiro words; the
            // outputs of distinct splitmix steps are never both zero.
            uint64_t z = m_settings.seed;
            for (uint64_t& word : m_rngState)
            {
                z += 0x9E3779B97F4A7C15ULL;
                uint64_t mixed = z;
                mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
                mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
                word = mixed ^ (mixed >> 31);
            }
        }
        if (!m_settings.enabled)
            m_delayedQueue.clear();
        SPARK_LOG_INFO(Spark::LogCategory::Network,
                       "InstabilitySimulator settings: enabled=%d loss=%.1f%% latency=%.1fms jitter=%.1fms "
                       "reorder=%.1f%% duplicate=%.1f%% seed=%llu",
                       m_settings.enabled ? 1 : 0, m_settings.packetLossPercent, m_settings.latencyMs,
                       m_settings.jitterMs, m_settings.reorderPercent, m_settings.duplicatePercent,
                       static_cast<unsigned long long>(m_settings.seed));
    }

    InstabilitySettings InstabilitySimulator::GetSettings() const
    {
        std::lock_guard lock(m_mutex);
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

    bool InstabilitySimulator::ShouldDuplicate()
    {
        std::lock_guard lock(m_mutex);

        if (!m_settings.enabled || m_settings.duplicatePercent <= 0.0f)
        {
            return false;
        }

        float roll = RandomFloat() * 100.0f;
        return roll < m_settings.duplicatePercent;
    }

    // ========================================================================
    // Packet Queue
    // ========================================================================

    void InstabilitySimulator::QueuePacket(std::vector<uint8_t> data, float sendTimeMs, bool localOnly,
                                           uint32_t sequence, uint64_t lifecycleEpoch, uint64_t destinationKey)
    {
        // The caller moves its serialized wire buffer into this parameter. If
        // locking or insertion throws before DelayedPacket takes ownership,
        // make sure that plaintext copy is still overwritten.
        const auto clearInput = Spark::MakeScopeExit([&data] { Spark::SecureClear(data); });
        std::lock_guard lock(m_mutex);
        if (!m_settings.enabled)
            return;

        DelayedPacket packet;
        packet.data = std::move(data);
        packet.deliveryTimeMs = sendTimeMs;
        packet.sequence = sequence;
        packet.lifecycleEpoch = lifecycleEpoch;
        packet.destinationKey = destinationKey;
        packet.localOnly = localOnly;

        // Insert sorted by delivery time (stable for equal times, so packets
        // queued with the same delay keep their send order)
        auto insertPos =
            std::upper_bound(m_delayedQueue.begin(), m_delayedQueue.end(), sendTimeMs,
                             [](float time, const DelayedPacket& pkt) { return time < pkt.deliveryTimeMs; });

        m_delayedQueue.insert(insertPos, std::move(packet));
    }

    std::vector<InstabilitySimulator::DelayedPacket> InstabilitySimulator::GetReadyPackets(float currentTimeMs)
    {
        std::lock_guard lock(m_mutex);

        std::vector<DelayedPacket> ready;

        // Since the queue is sorted by delivery time, we can stop at the first
        // packet that isn't ready yet.
        while (!m_delayedQueue.empty() && m_delayedQueue.front().deliveryTimeMs <= currentTimeMs)
        {
            ready.push_back(std::move(m_delayedQueue.front()));
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
            status += std::format("  Reorder:     {:.1f}% (hold {:.1f} ms)\n", m_settings.reorderPercent,
                                  m_settings.reorderHoldMs);
            status += std::format("  Duplicate:   {:.1f}%\n", m_settings.duplicatePercent);
            status += std::format("  Seed:        {}\n", m_settings.seed);
            status += std::format("  Queued pkts: {}\n", m_delayedQueue.size());
        }

        return status;
    }

    size_t InstabilitySimulator::GetQueuedPacketCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_delayedQueue.size();
    }

    size_t InstabilitySimulator::DiscardPacketsThroughLifecycle(uint64_t lifecycleEpoch)
    {
        std::lock_guard lock(m_mutex);
        return std::erase_if(m_delayedQueue, [lifecycleEpoch](const DelayedPacket& packet)
                             { return packet.lifecycleEpoch != 0 && packet.lifecycleEpoch <= lifecycleEpoch; });
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

    // ========================================================================
    // EngineSettings bridge
    // ========================================================================

    namespace
    {
        float FiniteClamped(float value, float lo, float hi)
        {
            return std::isfinite(value) ? std::clamp(value, lo, hi) : 0.0f;
        }
    } // namespace

    InstabilitySettings ImpairmentFromEngineSettings(const ::EngineSettings& settings)
    {
        const auto& net = settings.Network();
        constexpr float kMaxDelayMs = 60000.0f;

        InstabilitySettings out;
        out.latencyMs = FiniteClamped(net.simulatedLatencyMs, 0.0f, kMaxDelayMs);
        out.jitterMs = FiniteClamped(net.simulatedJitterMs, 0.0f, kMaxDelayMs);
        out.packetLossPercent = FiniteClamped(net.simulatedPacketLoss, 0.0f, 1.0f) * 100.0f;
        out.reorderPercent = FiniteClamped(net.simulatedReorderPercent, 0.0f, 100.0f);
        out.duplicatePercent = FiniteClamped(net.simulatedDuplicatePercent, 0.0f, 100.0f);
        out.seed = net.simulatedImpairmentSeed > 0 ? static_cast<uint64_t>(net.simulatedImpairmentSeed) : 0;
        out.enabled = out.latencyMs > 0.0f || out.jitterMs > 0.0f || out.packetLossPercent > 0.0f ||
                      out.reorderPercent > 0.0f || out.duplicatePercent > 0.0f;
        return out;
    }

    InstabilitySettings ApplyImpairmentSettings(const ::EngineSettings& settings)
    {
        const InstabilitySettings impairment = ImpairmentFromEngineSettings(settings);
        auto& simulator = InstabilitySimulator::GetInstance();
        simulator.SetSettings(impairment);
        if (impairment.enabled)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network,
                           "Network impairment ENABLED (dev only): latency=%.1fms jitter=%.1fms loss=%.1f%% "
                           "reorder=%.1f%% duplicate=%.1f%% seed=%llu",
                           impairment.latencyMs, impairment.jitterMs, impairment.packetLossPercent,
                           impairment.reorderPercent, impairment.duplicatePercent,
                           static_cast<unsigned long long>(impairment.seed));
        }
        return simulator.GetSettings();
    }

} // namespace Spark::Net
