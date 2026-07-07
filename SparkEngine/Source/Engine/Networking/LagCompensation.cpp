/**
 * @file LagCompensation.cpp
 * @brief Server-side hit rewind implementation
 */

#include "LagCompensation.h"

#ifdef ENABLE_NETWORKING

#include <algorithm>
#include <cmath>

namespace Spark::Net
{

    namespace
    {

        constexpr float kEpsilon = 1e-12f;

        /// Smallest non-negative t with |o + t*d - center| == radius, or false.
        bool RaySphere(const float o[3], const float d[3], const float cx, const float cy, const float cz, float radius,
                       float& outT)
        {
            float ox = o[0] - cx, oy = o[1] - cy, oz = o[2] - cz;
            float b = 2.0f * (ox * d[0] + oy * d[1] + oz * d[2]);
            float c = ox * ox + oy * oy + oz * oz - radius * radius;
            float disc = b * b - 4.0f * c; // a == 1 for normalized dir
            if (disc < 0.0f)
                return false;
            float sq = std::sqrt(disc);
            float t = (-b - sq) * 0.5f;
            if (t < 0.0f)
                t = (-b + sq) * 0.5f;
            if (t < 0.0f)
                return false;
            outT = t;
            return true;
        }

        /**
         * Ray vs vertical (Y-up) capsule. pose.pos is the capsule's
         * bottom-center; see RewindPose. dir must be normalized.
         */
        bool RayVerticalCapsule(const float o[3], const float d[3], const RewindPose& pose, float& outT)
        {
            const float r = pose.radius;
            if (r <= 0.0f)
                return false;

            const float cx = pose.pos[0];
            const float cz = pose.pos[2];
            float ylo = pose.pos[1] + r;               // bottom cap-sphere center
            float yhi = pose.pos[1] + pose.height - r; // top cap-sphere center
            if (yhi < ylo)                             // height <= 2r: degenerate sphere
                ylo = yhi = pose.pos[1] + pose.height * 0.5f;

            // Origin inside the capsule -> immediate hit at t=0.
            {
                float segY = std::clamp(o[1], ylo, yhi);
                float dx = o[0] - cx, dy = o[1] - segY, dz = o[2] - cz;
                if (dx * dx + dy * dy + dz * dz < r * r)
                {
                    outT = 0.0f;
                    return true;
                }
            }

            float best = -1.0f;

            // Cylinder side (2D quadratic in XZ since the axis is vertical).
            float a = d[0] * d[0] + d[2] * d[2];
            if (a > kEpsilon)
            {
                float ox = o[0] - cx, oz = o[2] - cz;
                float b = 2.0f * (ox * d[0] + oz * d[2]);
                float c = ox * ox + oz * oz - r * r;
                float disc = b * b - 4.0f * a * c;
                if (disc >= 0.0f)
                {
                    float t = (-b - std::sqrt(disc)) / (2.0f * a);
                    if (t >= 0.0f)
                    {
                        float y = o[1] + d[1] * t;
                        if (y >= ylo && y <= yhi)
                            best = t;
                    }
                }
            }

            // Cap spheres.
            float t = 0.0f;
            if (RaySphere(o, d, cx, ylo, cz, r, t) && (best < 0.0f || t < best))
                best = t;
            if (RaySphere(o, d, cx, yhi, cz, r, t) && (best < 0.0f || t < best))
                best = t;

            if (best < 0.0f)
                return false;
            outT = best;
            return true;
        }

        void LerpPose(const RewindPose& a, const RewindPose& b, float t, RewindPose& out)
        {
            out.entityId = a.entityId;
            for (int i = 0; i < 3; ++i)
                out.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
            out.radius = a.radius + (b.radius - a.radius) * t;
            out.height = a.height + (b.height - a.height) * t;
        }

        const RewindPose* FindPose(const std::vector<RewindPose>& poses, uint32_t id)
        {
            for (const auto& p : poses)
                if (p.entityId == id)
                    return &p;
            return nullptr;
        }

    } // namespace

    // ========================================================================
    // Ring buffer
    // ========================================================================

    void LagCompensation::Configure(float windowSec, float tickHz)
    {
        m_windowSec = std::max(windowSec, 0.0f);
        m_tickHz = std::max(tickHz, 1.0f);
        m_capacity = std::max<size_t>(2, static_cast<size_t>(std::ceil(m_windowSec * m_tickHz)) + 1);
        m_ring.assign(m_capacity, {});
        m_head = 0;
        m_count = 0;
    }

    void LagCompensation::Clear()
    {
        for (auto& snap : m_ring)
            snap.poses.clear(); // keep allocations, drop content
        m_head = 0;
        m_count = 0;
    }

    const LagCompensation::Snapshot& LagCompensation::At(size_t logicalIndex) const
    {
        return m_ring[(m_head + logicalIndex) % m_capacity];
    }

    void LagCompensation::RecordSnapshot(double serverTime, const std::vector<RewindPose>& poses)
    {
        size_t slot;
        if (m_count == m_capacity)
        {
            slot = m_head; // overwrite oldest
            m_head = (m_head + 1) % m_capacity;
        }
        else
        {
            slot = (m_head + m_count) % m_capacity;
            ++m_count;
        }
        m_ring[slot].time = serverTime;
        m_ring[slot].poses = poses;

        // Evict snapshots that fell out of the window. Keep the last snapshot
        // at-or-before (serverTime - windowSec) so that time can still be
        // bracketed for interpolation.
        const double cutoff = serverTime - static_cast<double>(m_windowSec);
        while (m_count >= 2 && At(1).time <= cutoff)
        {
            m_head = (m_head + 1) % m_capacity;
            --m_count;
        }
    }

    // ========================================================================
    // Rewound raycast
    // ========================================================================

    uint32_t LagCompensation::RewindRaycast(double rewindTime, const float origin[3], const float dir[3], float maxDist,
                                            uint32_t ignoreEntity, float outHitPoint[3], float* outDist) const
    {
        if (m_count == 0 || maxDist <= 0.0f)
            return 0;

        float dirLenSq = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
        if (dirLenSq < kEpsilon)
            return 0;

        // Clamp into the recorded range, then find the bracketing snapshots.
        const double oldest = At(0).time;
        const double newest = At(m_count - 1).time;
        const double t = std::clamp(rewindTime, oldest, newest);

        size_t lo = 0;
        while (lo + 1 < m_count && At(lo + 1).time <= t)
            ++lo;
        const Snapshot& before = At(lo);
        const Snapshot& after = At(std::min(lo + 1, m_count - 1));

        float alpha = 0.0f;
        const double span = after.time - before.time;
        if (span > 0.0)
            alpha = static_cast<float>((t - before.time) / span);

        uint32_t bestId = 0;
        float bestT = maxDist;

        auto testPose = [&](const RewindPose& pose)
        {
            if (pose.entityId == 0 || pose.entityId == ignoreEntity)
                return;
            float hitT = 0.0f;
            if (RayVerticalCapsule(origin, dir, pose, hitT) && hitT <= bestT)
            {
                bestT = hitT;
                bestId = pose.entityId;
            }
        };

        // Entities in `before` (lerped when also present in `after`)...
        for (const RewindPose& pb : before.poses)
        {
            if (const RewindPose* pa = FindPose(after.poses, pb.entityId))
            {
                RewindPose lerped;
                LerpPose(pb, *pa, alpha, lerped);
                testPose(lerped);
            }
            else
            {
                testPose(pb);
            }
        }
        // ...plus entities only present in `after` (just spawned).
        if (&before != &after)
        {
            for (const RewindPose& pa : after.poses)
            {
                if (!FindPose(before.poses, pa.entityId))
                    testPose(pa);
            }
        }

        if (bestId != 0)
        {
            if (outHitPoint)
            {
                for (int i = 0; i < 3; ++i)
                    outHitPoint[i] = origin[i] + dir[i] * bestT;
            }
            if (outDist)
                *outDist = bestT;
        }
        return bestId;
    }

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
