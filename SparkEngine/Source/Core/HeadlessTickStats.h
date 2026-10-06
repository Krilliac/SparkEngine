/**
 * @file HeadlessTickStats.h
 * @brief Bounded fixed-bucket histogram of headless tick work time.
 *
 * The headless/dedicated-server loops (SparkEngineLinuxHeadless.cpp and
 * SparkEngineWindowsHeadless.cpp) sleep to a fixed 60 Hz cadence, so wall-clock
 * frame time says nothing about engine cost. They record the pre-sleep work
 * duration of every tick here instead and print one machine-readable
 * `SPARK_HEADLESS_TICK_STATS` record at shutdown, which
 * tools/perf-budget/collect_headless_result.py turns into a perf-budget result.
 * The record also carries the host's own peak resident set, read in-process
 * (Linux `VmHWM`, Windows `PeakWorkingSetSize`): the parent's `wait4`
 * `ru_maxrss` is not usable because Linux folds the pre-exec image's
 * high-water mark (the launching harness) into it.
 *
 * Memory is fixed regardless of run length (a soak does not grow it). Values
 * below 128 us are exact; larger values fall into log-linear buckets with 64
 * sub-buckets per power of two (< 1.6% relative width). Reported percentiles
 * are the bucket's inclusive upper bound clamped to the exact observed maximum,
 * so they never under-report a lower-is-better cost.
 */
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace Spark
{
    /**
     * @brief Fixed-size tick work-time histogram with nearest-rank percentiles.
     */
    class HeadlessTickStats
    {
      public:
        /// Values below this are stored in exact 1 us buckets.
        static constexpr uint64_t EXACT_LIMIT_US = 128;
        /// Sub-buckets per power of two above EXACT_LIMIT_US.
        static constexpr uint64_t SUB_BUCKETS = 64;
        /// Largest representable sample (~2.4 hours); longer ticks saturate here.
        static constexpr uint64_t MAX_TRACKED_US = (uint64_t{1} << 33) - 1;
        /// Total bucket count (exact range plus 26 log-linear octaves).
        static constexpr size_t BUCKET_COUNT = 128 + 26 * 64;

        /**
         * @brief Record one tick's work duration.
         * @param microseconds Work time in microseconds; saturates at MAX_TRACKED_US.
         */
        void Record(uint64_t microseconds) noexcept
        {
            const uint64_t clamped = microseconds > MAX_TRACKED_US ? MAX_TRACKED_US : microseconds;
            ++m_buckets[BucketIndex(clamped)];
            ++m_count;
            if (clamped > m_max)
                m_max = clamped;
        }

        /// @return Number of recorded samples.
        [[nodiscard]] uint64_t Count() const noexcept { return m_count; }

        /// @return Exact largest recorded sample in microseconds (0 when empty).
        [[nodiscard]] uint64_t MaxUs() const noexcept { return m_max; }

        /**
         * @brief Nearest-rank percentile, reported as the containing bucket's upper bound.
         * @param percent Percentile in (0, 100]; values outside are clamped.
         * @return Percentile in microseconds, never above MaxUs(); 0 when empty.
         */
        [[nodiscard]] uint64_t PercentileUs(double percent) const noexcept
        {
            if (m_count == 0)
                return 0;
            if (!(percent > 0.0))
                percent = 0.0;
            if (percent > 100.0)
                percent = 100.0;

            // Nearest rank: the smallest sample with at least percent% of the
            // samples at or below it. Rank is 1-based and at least 1.
            const double exactRank = percent / 100.0 * static_cast<double>(m_count);
            uint64_t rank = static_cast<uint64_t>(exactRank);
            if (static_cast<double>(rank) < exactRank)
                ++rank;
            if (rank == 0)
                rank = 1;

            uint64_t cumulative = 0;
            for (size_t index = 0; index < BUCKET_COUNT; ++index)
            {
                cumulative += m_buckets[index];
                if (cumulative >= rank)
                {
                    const uint64_t upper = BucketUpperBound(index);
                    return upper < m_max ? upper : m_max;
                }
            }
            return m_max;
        }

        /**
         * @brief Print the single `SPARK_HEADLESS_TICK_STATS` shutdown record to stdout.
         * @param nullRhiActive True when the loop ran against a live NullRHI device.
         * @param peakRssKib The process's own peak resident set in KiB, or 0 when the
         *        platform cannot measure it (the collector rejects 0).
         */
        void EmitRecord(bool nullRhiActive, uint64_t peakRssKib) const
        {
            std::fprintf(stdout,
                         "SPARK_HEADLESS_TICK_STATS backend=%s frames=%llu p50_us=%llu p99_us=%llu max_us=%llu "
                         "peak_rss_kib=%llu\n",
                         nullRhiActive ? "null" : "none", static_cast<unsigned long long>(m_count),
                         static_cast<unsigned long long>(PercentileUs(50.0)),
                         static_cast<unsigned long long>(PercentileUs(99.0)), static_cast<unsigned long long>(m_max),
                         static_cast<unsigned long long>(peakRssKib));
            std::fflush(stdout);
        }

        /**
         * @brief Extract the `VmHWM` value from Linux `/proc/self/status` text.
         *
         * `VmHWM` is the address space's resident high-water mark; exec starts a
         * fresh address space, so unlike `ru_maxrss` it never includes the
         * launching process's memory.
         * @param statusText Contents of `/proc/self/status`.
         * @return Peak resident set in KiB, or 0 when the line is absent or malformed.
         */
        [[nodiscard]] static constexpr uint64_t ParseVmHwmKib(std::string_view statusText) noexcept
        {
            constexpr std::string_view key = "VmHWM:";
            size_t lineStart = 0;
            while (lineStart < statusText.size())
            {
                size_t lineEnd = statusText.find('\n', lineStart);
                if (lineEnd == std::string_view::npos)
                    lineEnd = statusText.size();
                const std::string_view line = statusText.substr(lineStart, lineEnd - lineStart);
                lineStart = lineEnd + 1;
                if (!line.starts_with(key))
                    continue;

                size_t pos = key.size();
                while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
                    ++pos;
                const size_t digitsStart = pos;
                uint64_t value = 0;
                while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9')
                {
                    // 16 digits of KiB is far beyond any real address space; longer is malformed.
                    if (pos - digitsStart >= 16)
                        return 0;
                    value = value * 10 + static_cast<uint64_t>(line[pos] - '0');
                    ++pos;
                }
                if (pos == digitsStart || line.substr(pos) != " kB")
                    return 0;
                return value;
            }
            return 0;
        }

        /// @return Bucket index holding @p microseconds (must be <= MAX_TRACKED_US).
        [[nodiscard]] static constexpr size_t BucketIndex(uint64_t microseconds) noexcept
        {
            if (microseconds < EXACT_LIMIT_US)
                return static_cast<size_t>(microseconds);
            // shift >= 1 keeps (value >> shift) in [64, 128).
            const uint64_t shift = static_cast<uint64_t>(std::bit_width(microseconds)) - 7;
            return static_cast<size_t>(EXACT_LIMIT_US + (shift - 1) * SUB_BUCKETS +
                                       ((microseconds >> shift) - SUB_BUCKETS));
        }

        /// @return Largest microsecond value that maps to bucket @p index.
        [[nodiscard]] static constexpr uint64_t BucketUpperBound(size_t index) noexcept
        {
            if (index < EXACT_LIMIT_US)
                return index;
            const uint64_t offset = index - EXACT_LIMIT_US;
            const uint64_t shift = offset / SUB_BUCKETS + 1;
            const uint64_t subBucket = offset % SUB_BUCKETS + SUB_BUCKETS;
            return ((subBucket + 1) << shift) - 1;
        }

      private:
        std::array<uint64_t, BUCKET_COUNT> m_buckets{};
        uint64_t m_count = 0;
        uint64_t m_max = 0;
    };

    static_assert(HeadlessTickStats::BucketIndex(HeadlessTickStats::MAX_TRACKED_US) ==
                      HeadlessTickStats::BUCKET_COUNT - 1,
                  "bucket table must end exactly at MAX_TRACKED_US");
    static_assert(HeadlessTickStats::BucketUpperBound(HeadlessTickStats::BUCKET_COUNT - 1) ==
                  HeadlessTickStats::MAX_TRACKED_US);
} // namespace Spark
