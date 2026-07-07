/**
 * @file LagCompensation.h
 * @brief Server-side hit rewind — ring buffer of entity poses + rewound raycast
 * @author Spark Engine Team
 * @date 2026
 *
 * Generic lag compensation for shooter-style modules: every server tick the
 * game records a snapshot of entity poses (vertical capsules); when a fire
 * event arrives, the server rewinds to the shooter's perceived time
 * (serverTime - RTT/2 - interpolation delay) and raycasts against the
 * historical poses instead of the current ones.
 *
 * Design:
 * - Fixed-capacity ring buffer sized from Configure(windowSec, tickHz);
 *   snapshots older than the window are evicted.
 * - RewindRaycast() finds the two snapshots bracketing the rewind time,
 *   lerps each entity's pose, then intersects the ray with each entity's
 *   vertical capsule. Nearest hit wins.
 * - Game-agnostic: entity ids are opaque uint32_t; id 0 is reserved for
 *   "no hit" and poses carrying id 0 are ignored.
 *
 * Threading contract: NOT thread-safe. Record and raycast from the same
 * thread (the server simulation tick — e.g. an IAreaSimulation::OnAreaTick).
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 */

#pragma once

#ifdef ENABLE_NETWORKING

#include <cstdint>
#include <vector>

namespace Spark::Net
{

    /**
     * @brief One entity's pose in a snapshot, as a vertical (Y-up) capsule.
     *
     * pos is the capsule's bottom-center (feet). The capsule spans
     * [pos.y, pos.y + height] vertically with the given radius. If
     * height <= 2*radius the capsule degenerates to a sphere centered at
     * pos.y + height/2.
     */
    struct RewindPose
    {
        uint32_t entityId; ///< Opaque entity id; 0 is reserved (ignored)
        float pos[3];      ///< Capsule bottom-center (world space)
        float radius;      ///< Capsule radius
        float height;      ///< Total capsule height (feet to head)
    };

    /**
     * @brief Ring buffer of pose snapshots with lag-compensated raycasts.
     */
    class LagCompensation
    {
      public:
        /// Default window: 250 ms at 60 Hz (see Configure()).
        LagCompensation() { Configure(0.25f, 60.0f); }

        /**
         * @brief Size the history ring for windowSec of snapshots at tickHz.
         *
         * Clears any recorded history. Capacity is ceil(windowSec * tickHz)+1
         * so a rewind exactly windowSec into the past can still be bracketed.
         */
        void Configure(float windowSec, float tickHz);

        /**
         * @brief Record one snapshot of all rewindable entity poses.
         * @param serverTime Monotonic server time in seconds. Must be
         *        non-decreasing across calls; snapshots older than the
         *        configured window (relative to serverTime) are evicted.
         * @param poses Poses for this tick (copied into the ring slot).
         */
        void RecordSnapshot(double serverTime, const std::vector<RewindPose>& poses);

        /**
         * @brief Raycast against poses rewound to rewindTime.
         *
         * rewindTime is clamped to the recorded history range. Poses are
         * lerped between the two bracketing snapshots (entities present in
         * only one snapshot use that snapshot's pose unlerped).
         *
         * @param rewindTime  Target server time to rewind to (seconds)
         * @param origin      Ray origin (world space)
         * @param dir         Ray direction — must be normalized
         * @param maxDist     Maximum hit distance
         * @param ignoreEntity Entity id excluded from the test (e.g. shooter)
         * @param outHitPoint Optional (may be null): world-space hit point
         * @param outDist     Optional (may be null): hit distance along dir
         * @return Hit entity id, or 0 for no hit.
         */
        uint32_t RewindRaycast(double rewindTime, const float origin[3], const float dir[3], float maxDist,
                               uint32_t ignoreEntity, float outHitPoint[3], float* outDist) const;

        /// Drop all recorded history (configuration is kept).
        void Clear();

      private:
        struct Snapshot
        {
            double time = 0.0;
            std::vector<RewindPose> poses;
        };

        const Snapshot& At(size_t logicalIndex) const; ///< 0 == oldest

        float m_windowSec = 0.25f;
        float m_tickHz = 60.0f;
        size_t m_capacity = 0;
        size_t m_head = 0;  ///< Ring index of the oldest snapshot
        size_t m_count = 0; ///< Number of valid snapshots
        std::vector<Snapshot> m_ring;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
