/**
 * @file NetworkInterpolation.cpp
 * @brief Implementation of client-side network entity interpolation
 * @author Spark Engine Team
 * @date 2025
 */

#include "../../Core/Platform.h"
#include "NetworkInterpolation.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <algorithm>

using namespace DirectX;

namespace Spark::Net
{

    void NetworkInterpolationBuffer::AddSnapshot(const InterpolationSnapshot& snapshot)
    {
        m_snapshots[m_writeIndex] = snapshot;
        m_writeIndex = (m_writeIndex + 1) % MAX_SNAPSHOTS;
        if (m_count < MAX_SNAPSHOTS)
        {
            m_count++;
        }
    }

    InterpolationSnapshot NetworkInterpolationBuffer::Sample(float currentTime) const
    {
        if (m_count == 0)
        {
            return InterpolationSnapshot{};
        }

        if (m_count == 1)
        {
            return GetSnapshot(0);
        }

        float renderTime = currentTime - m_interpolationDelay;

        // Find the two snapshots bracketing renderTime
        // Snapshots are stored chronologically; GetSnapshot(0) is the oldest
        const auto& oldest = GetSnapshot(0);
        const auto& newest = GetSnapshot(m_count - 1);

        if (renderTime <= oldest.timestamp)
        {
            return oldest;
        }

        if (renderTime >= newest.timestamp)
        {
            return newest;
        }

        // Find the pair of snapshots surrounding renderTime
        for (size_t i = 0; i < m_count - 1; ++i)
        {
            const auto& a = GetSnapshot(i);
            const auto& b = GetSnapshot(i + 1);

            if (renderTime >= a.timestamp && renderTime <= b.timestamp)
            {
                float range = b.timestamp - a.timestamp;
                float t = (range > 0.0001f) ? (renderTime - a.timestamp) / range : 0.0f;

                InterpolationSnapshot result;
                result.timestamp = renderTime;

                // Lerp position
                XMVECTOR posA = XMLoadFloat3(&a.position);
                XMVECTOR posB = XMLoadFloat3(&b.position);
                XMStoreFloat3(&result.position, XMVectorLerp(posA, posB, t));

                // Slerp rotation
                XMVECTOR rotA = XMLoadFloat4(&a.rotation);
                XMVECTOR rotB = XMLoadFloat4(&b.rotation);
                XMStoreFloat4(&result.rotation, XMQuaternionSlerp(rotA, rotB, t));

                return result;
            }
        }

        // Fallback (should not reach here)
        return newest;
    }

    void NetworkInterpolationBuffer::SetInterpolationDelay(float seconds)
    {
        m_interpolationDelay = std::clamp(seconds, 0.0f, 1.0f);
    }

    void NetworkInterpolationBuffer::Clear()
    {
        m_count = 0;
        m_writeIndex = 0;
    }

    const InterpolationSnapshot& NetworkInterpolationBuffer::GetSnapshot(size_t logicalIndex) const
    {
        // logicalIndex 0 = oldest snapshot
        // If buffer is full, oldest is at m_writeIndex; otherwise oldest is at 0
        size_t startIndex = (m_count == MAX_SNAPSHOTS) ? m_writeIndex : 0;
        size_t actualIndex = (startIndex + logicalIndex) % MAX_SNAPSHOTS;
        return m_snapshots[actualIndex];
    }

} // namespace Spark::Net
