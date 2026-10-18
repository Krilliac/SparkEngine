/**
 * @file ConnectRateLimiter.cpp
 * @brief Per-source token bucket for unadmitted Connect datagrams (see ConnectRateLimiter.h)
 */

#include "ConnectRateLimiter.h"

#include <algorithm>
#include <iterator>

namespace Spark::Net
{

    void ConnectRateLimiter::Configure(const ConnectRateLimit& limit)
    {
        m_limit = limit;
        m_buckets.clear();
    }

    float ConnectRateLimiter::Refilled(const Bucket& bucket, float now) const
    {
        // Server time restarts at zero with each server lifecycle; a bucket from before that
        // is treated as fully refilled rather than trusted.
        if (now < bucket.lastRefill)
        {
            return m_limit.burst;
        }
        return std::min(m_limit.burst, bucket.tokens + (now - bucket.lastRefill) * m_limit.refillPerSecond);
    }

    void ConnectRateLimiter::ForgetRefilledSources(float now)
    {
        for (auto it = m_buckets.begin(); it != m_buckets.end();)
        {
            it = Refilled(it->second, now) >= m_limit.burst ? m_buckets.erase(it) : std::next(it);
        }
    }

    bool ConnectRateLimiter::Allow(uint32_t sourceIPv4, float now)
    {
        if (m_limit.burst < 1.0f)
        {
            return false;
        }

        auto it = m_buckets.find(sourceIPv4);
        if (it == m_buckets.end())
        {
            if (m_buckets.size() >= MAX_TRACKED_SOURCES)
            {
                ForgetRefilledSources(now);
                if (m_buckets.size() >= MAX_TRACKED_SOURCES)
                {
                    return false; // every tracked source is still active: refuse newcomers
                }
            }
            it = m_buckets.emplace(sourceIPv4, Bucket{m_limit.burst, now}).first;
        }

        Bucket& bucket = it->second;
        bucket.tokens = Refilled(bucket, now);
        bucket.lastRefill = now;
        if (bucket.tokens < 1.0f)
        {
            return false;
        }
        bucket.tokens -= 1.0f;
        return true;
    }

} // namespace Spark::Net
