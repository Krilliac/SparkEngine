/**
 * @file ConnectRateLimiter.h
 * @brief NET-100: per-source-IPv4 token bucket for unadmitted Connect datagrams
 * @author Spark Engine Team
 * @date 2026
 *
 * Every Connect a server handles for an endpoint that holds no admitted session costs it
 * work (a handshake response, or at least a ConnectRejected) and sends one datagram to an
 * address the peer may have spoofed. This bucket bounds both per source address: a source
 * that exceeds it is dropped silently, with no reply, so it can neither burn CPU nor use
 * the server as a reflector.
 *
 * The key is the IPv4 address alone, never the port: ports are free to vary.
 * Tracked sources are capped (MAX_TRACKED_SOURCES). When the table is full, sources whose
 * buckets have refilled are forgotten first; if none has, a new source is refused until one
 * does (fail closed under a flood of distinct addresses).
 *
 * Thread affinity: the thread that runs NetworkManager::ProcessIncoming (game thread).
 * Ownership: owned by NetworkManager; holds only addresses and counters.
 * Allocation: one hash-map node per tracked source, bounded by MAX_TRACKED_SOURCES.
 * Scalability tier: connection setup only; O(1) per Connect, O(n) only when the table is full.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace Spark::Net
{

    /** @brief Connect budget per source IPv4 address. */
    struct ConnectRateLimit
    {
        float burst = 16.0f;          ///< Connects a quiet source may send back to back
        float refillPerSecond = 4.0f; ///< Sustained Connects per second per source
    };

    /**
     * @brief Token bucket per source address (see file comment)
     */
    class ConnectRateLimiter
    {
      public:
        static constexpr size_t MAX_TRACKED_SOURCES = 4096;

        /**
         * @brief Replace the limit and forget every tracked source
         * @param limit New budget; a non-positive burst refuses every Connect
         */
        void Configure(const ConnectRateLimit& limit);

        /**
         * @brief Charge one Connect from @p sourceIPv4 at time @p now
         * @param sourceIPv4 Source address in host byte order
         * @param now        Monotonic seconds (NetworkManager server time)
         * @return true when the Connect may be handled, false when it must be dropped unanswered
         */
        [[nodiscard]] bool Allow(uint32_t sourceIPv4, float now);

        /** @brief Forget every tracked source (server start/stop). */
        void Clear() { m_buckets.clear(); }

        /** @brief Number of sources currently tracked. */
        [[nodiscard]] size_t TrackedSources() const { return m_buckets.size(); }

      private:
        struct Bucket
        {
            float tokens = 0.0f;
            float lastRefill = 0.0f;
        };

        [[nodiscard]] float Refilled(const Bucket& bucket, float now) const;
        void ForgetRefilledSources(float now);

        ConnectRateLimit m_limit;
        std::unordered_map<uint32_t, Bucket> m_buckets;
    };

} // namespace Spark::Net
