/**
 * @file SubTickInput.h
 * @brief Sub-tick input recording for precise input timing within server ticks
 * @author Spark Engine Team
 * @date 2026
 *
 * Records input actions at sub-tick precision so the server can process
 * inputs at their exact timestamp within a tick, improving hit registration
 * accuracy for fast-paced FPS gameplay.
 */

#pragma once
#include "../../Core/Platform.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace Spark::Net
{

    // ========================================================================
    // Sub-Tick Input Sample
    // ========================================================================

    /**
     * @brief A single input snapshot timestamped at sub-tick precision.
     *
     * Each sample captures the full input state at a specific moment within
     * a server tick. The tickFraction field records where in the tick [0,1)
     * the input occurred, allowing the server to replay inputs at their
     * exact sub-tick time for improved hit detection accuracy.
     */
    struct SubTickInputSample
    {
        uint32_t sequenceNumber = 0;    ///< Monotonically increasing input sequence
        float tickFraction = 0.0f;      ///< [0,1) position within the current server tick
        uint32_t inputBitfield = 0;     ///< Compressed input state (fire, jump, etc.)
        XMFLOAT3 aimDirection{0, 0, 1}; ///< Player aim direction at sample time
        float timestamp = 0.0f;         ///< Client-side high-resolution timestamp (seconds)
    };

    // ========================================================================
    // Sub-Tick Input Buffer
    // ========================================================================

    /**
     * @brief Collects sub-tick input samples for a single tick and processes
     * them on the server at their exact sub-tick timestamps.
     *
     * Client-side usage:
     * @code
     *   SubTickInputBuffer buffer;
     *   // During a tick, record inputs at their exact times:
     *   buffer.RecordInput({seq, fraction, bits, aimDir, clientTime});
     *   // Send buffer contents with the tick's network packet
     * @endcode
     *
     * Server-side usage:
     * @code
     *   buffer.ProcessInputsForTick(tickNum, tickDuration,
     *       [](const SubTickInputSample& sample, float exactTime) {
     *           // Process input at exactTime within the tick
     *       });
     * @endcode
     */
    class SubTickInputBuffer
    {
      public:
        /// @brief Record an input sample into the buffer
        /// @param sample  The sub-tick input sample to record
        void RecordInput(const SubTickInputSample& sample);

        /// @brief Clear all recorded samples (call at start of each tick)
        void Clear();

        /// @brief Get all recorded samples (read-only)
        /// @return Reference to the internal sample vector
        const std::vector<SubTickInputSample>& GetSamples() const;

        /**
         * @brief Process all buffered inputs at their exact sub-tick timestamps.
         *
         * Iterates samples in sequence order, computing each sample's exact
         * time within the tick as: tickFraction * tickDuration. The handler
         * receives both the sample and its computed exact time.
         *
         * @param tickNumber   Current server tick number (for validation)
         * @param tickDuration Duration of one server tick in seconds
         * @param handler      Callback invoked for each sample with its exact time
         */
        void ProcessInputsForTick(uint32_t tickNumber, float tickDuration,
                                  std::function<void(const SubTickInputSample&, float exactTime)> handler) const;

        /// @brief Get the number of samples currently buffered
        size_t GetSampleCount() const;

      private:
        std::vector<SubTickInputSample> m_samples;
        uint32_t m_nextSequence = 0;
    };

} // namespace Spark::Net
