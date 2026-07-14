/**
 * @file NetworkInterpolation.h
 * @brief Client-side interpolation buffer for smooth remote entity rendering
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a circular buffer of timestamped entity snapshots and an interpolation
 * sampler that produces smooth positions and rotations for remote entities. The
 * buffer renders entities slightly in the past (configurable delay, default 100 ms)
 * to ensure two bracketing snapshots are always available for interpolation.
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <array>
#include <cstddef>

namespace Spark::Net
{

    /**
     * @brief A single timestamped snapshot of an entity's network state.
     */
    struct InterpolationSnapshot
    {
        XMFLOAT3 position{0, 0, 0};    ///< World-space position at this point in time.
        XMFLOAT4 rotation{0, 0, 0, 1}; ///< Orientation quaternion (x, y, z, w).
        float timestamp = 0.0f;        ///< Server time when this state was authoritative.
    };

    /**
     * @brief Circular buffer that stores recent entity snapshots and interpolates
     *        between them for smooth client-side rendering of remote entities.
     *
     * Usage:
     * @code
     *   NetworkInterpolationBuffer buffer;
     *   buffer.SetInterpolationDelay(0.1f); // 100ms behind
     *
     *   // On receiving a server state update:
     *   buffer.AddSnapshot({position, rotation, serverTime});
     *
     *   // Each render frame:
     *   auto state = buffer.Sample(currentTime);
     *   entity.SetPosition(state.position);
     * @endcode
     *
     * The default interpolation delay is 100 ms, which is suitable for most FPS games
     * running at 20 Hz server tick rate.
     */
    class NetworkInterpolationBuffer
    {
      public:
        /**
         * @brief Add a new snapshot to the buffer.
         *
         * Snapshots should be added in chronological order. If the buffer is full,
         * the oldest snapshot is overwritten.
         *
         * @param snapshot  The timestamped entity state to store.
         */
        void AddSnapshot(const InterpolationSnapshot& snapshot);

        /**
         * @brief Sample an interpolated state at the given time.
         *
         * The actual render time is computed as `currentTime - interpolationDelay`.
         * The method finds the two snapshots bracketing this render time and
         * linearly interpolates position (Lerp) and rotation (Slerp) between them.
         *
         * - If no snapshots exist, returns a default-initialized snapshot.
         * - If only one snapshot exists, returns that snapshot.
         * - If render time is before all snapshots, returns the oldest.
         * - If render time is after all snapshots, returns the newest (no extrapolation).
         *
         * @param currentTime  The current local time (seconds).
         * @return             Interpolated entity state.
         */
        InterpolationSnapshot Sample(float currentTime) const;

        /**
         * @brief Set the interpolation delay (how far behind real-time to render).
         * @param seconds  Delay in seconds. Clamped to [0, 1]. Default: 0.1 (100 ms).
         */
        void SetInterpolationDelay(float seconds);

        /**
         * @brief Get the current interpolation delay.
         * @return  Delay in seconds.
         */
        float GetInterpolationDelay() const { return m_interpolationDelay; }

        /**
         * @brief Remove all snapshots from the buffer.
         */
        void Clear();

        /**
         * @brief Get the number of snapshots currently stored.
         * @return  Snapshot count.
         */
        size_t GetSnapshotCount() const { return m_count; }

      private:
        static constexpr size_t MAX_SNAPSHOTS = 32;

        std::array<InterpolationSnapshot, MAX_SNAPSHOTS> m_snapshots{};
        size_t m_count = 0;
        size_t m_writeIndex = 0;
        float m_interpolationDelay = 0.1f;

        /**
         * @brief Get a snapshot by its logical index (0 = oldest).
         */
        const InterpolationSnapshot& GetSnapshot(size_t logicalIndex) const;
    };

} // namespace Spark::Net
