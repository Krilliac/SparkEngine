/**
 * @file TFPerfCounters.h
 * @brief Cheap, allocation-free scoped timers for profiling the authoritative
 *        server tick (TF-W13 server-perf lane).
 *
 * OWNERSHIP: NEW file, owned by the server-perf lane. Header-only so ANY
 * system's FixedUpdate can drop in a ScopedTimer with just this include --
 * no dependency on TFServerSim.
 *
 * Usage (drop-in, one line, no other code changes):
 *   #include "Utils/TFPerfCounters.h"
 *   void SomeSystem::FixedUpdate(float fixedDeltaTime)
 *   {
 *       Terrafront::TFPerfCounters::ScopedTimer _t(Terrafront::TFPerfCounters::Phase::AI);
 *       ... existing tick body, unchanged ...
 *   }
 *
 * The `tf_perf` console command (Console/TFCommands.cpp) reads
 * TFPerfCounters::Instance().Report(kServerTickHz) and prints it. Phases with
 * zero recorded samples are omitted from the report (a system that hasn't
 * wired in a ScopedTimer yet, or hasn't ticked this run, must not show a
 * misleading 0.00 ms average).
 *
 * GOTCHAS:
 *  - steady_clock ONLY for the elapsed-time measurement below (see the
 *    msvc-cv-steady-clock-hang playbook: MSVC condition_variable waits on
 *    system_clock can hang across a wall-clock backward step). This header
 *    never waits/blocks on a clock -- it only samples steady_clock::now() at
 *    scope entry/exit -- so that hang class does not apply here, but do not
 *    "fix" this by swapping in system_clock for wall-clock friendliness.
 *  - NEVER add wall-clock reads to actual gameplay/simulation logic; this
 *    profiler is the one sanctioned exception (it measures, it never drives
 *    simulation decisions).
 *  - Instance() is a function-local (Meyer's) singleton. Per-image DLL
 *    statics caveat: this is safe ONLY because every caller (TFServerSim,
 *    TFRegionSystem, TFBotSystem, TFReplication, TFVehicleNet, TFCommands)
 *    links into the SAME SparkGameMMOFPS module DLL and therefore shares one
 *    image. Do not reuse this header from a second, separately-linked binary
 *    expecting to observe the same counters -- it would get its own
 *    independent instance.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace Terrafront
{

    class TFPerfCounters
    {
      public:
        /// Major FixedUpdate phases the server tick is profiled across.
        /// Extend this list (and PhaseName below) rather than adding a
        /// parallel profiling mechanism elsewhere.
        enum class Phase : uint8_t
        {
            Movement = 0,     // TFServerSim::TickMovement
            Capture,          // TFRegionSystem::FixedUpdate capture sweep
            PhysicsStep,      // PhysicsSystem::StepFixed (the single per-process Jolt step)
            ReplicationBuild, // TFServerSim move-state prep + TFReplication/TFVehicleNet broadcast build
            AI,               // TFBotSystem::FixedUpdate
            COUNT
        };

        static constexpr size_t kRingSize = 120; // ~2s of samples at 60 Hz

        static TFPerfCounters& Instance()
        {
            static TFPerfCounters s_instance;
            return s_instance;
        }

        /// RAII scoped timer: records elapsed steady_clock ms into `phase` on
        /// destruction. Construct at the top of the code block being timed;
        /// let it go out of scope at the end of that block.
        class ScopedTimer
        {
          public:
            explicit ScopedTimer(Phase phase, TFPerfCounters& counters = Instance())
                : m_counters(counters), m_phase(phase), m_start(std::chrono::steady_clock::now())
            {
            }
            ~ScopedTimer()
            {
                const auto end = std::chrono::steady_clock::now();
                const double ms = std::chrono::duration<double, std::milli>(end - m_start).count();
                m_counters.Record(m_phase, ms);
            }
            ScopedTimer(const ScopedTimer&) = delete;
            ScopedTimer& operator=(const ScopedTimer&) = delete;

          private:
            TFPerfCounters& m_counters;
            Phase m_phase;
            std::chrono::steady_clock::time_point m_start;
        };

        void Record(Phase phase, double ms)
        {
            Ring& r = m_rings[static_cast<size_t>(phase)];
            constexpr uint32_t kRingSize32 = static_cast<uint32_t>(kRingSize);
            if (r.count == kRingSize32)
                r.sum -= r.samples[r.head]; // about to overwrite the oldest sample
            else
                ++r.count;
            r.samples[r.head] = ms;
            r.sum += ms;
            r.head = (r.head + 1) % kRingSize32;
            if (ms > r.peak)
                r.peak = ms;
        }

        double AverageMs(Phase phase) const
        {
            const Ring& r = m_rings[static_cast<size_t>(phase)];
            return r.count == 0 ? 0.0 : r.sum / static_cast<double>(r.count);
        }

        double PeakMs(Phase phase) const { return m_rings[static_cast<size_t>(phase)].peak; }
        uint32_t SampleCount(Phase phase) const { return m_rings[static_cast<size_t>(phase)].count; }

        static const char* PhaseName(Phase p)
        {
            switch (p)
            {
            case Phase::Movement:
                return "movement";
            case Phase::Capture:
                return "capture";
            case Phase::PhysicsStep:
                return "physics";
            case Phase::ReplicationBuild:
                return "replication";
            case Phase::AI:
                return "ai";
            default:
                return "?";
            }
        }

        /// tf_perf console command body: per-phase averages/peaks + tick
        /// budget headroom against a fixed-step rate (kServerTickHz == 60 ->
        /// 16.667 ms budget). Phases with zero samples are omitted.
        std::string Report(double fixedHz) const
        {
            std::ostringstream os;
            os << std::fixed << std::setprecision(3);
            const double budgetMs = fixedHz > 0.0 ? 1000.0 / fixedHz : 0.0;
            double totalAvg = 0.0;
            bool any = false;
            os << "[TF] server tick perf (budget " << budgetMs << " ms @ " << fixedHz << " Hz):";
            for (size_t i = 0; i < static_cast<size_t>(Phase::COUNT); ++i)
            {
                const Phase p = static_cast<Phase>(i);
                const Ring& r = m_rings[i];
                if (r.count == 0)
                    continue;
                any = true;
                const double avg = AverageMs(p);
                totalAvg += avg;
                os << "\n  " << PhaseName(p) << ": avg " << avg << " ms  peak " << r.peak << " ms  (n=" << r.count
                   << ")";
            }
            if (!any)
            {
                os << "\n  (no samples yet -- ScopedTimer instances not wired into a running phase)";
                return os.str();
            }
            const double headroomMs = budgetMs - totalAvg;
            const double headroomPct = budgetMs > 0.0 ? (headroomMs / budgetMs * 100.0) : 0.0;
            os << "\n  measured total: " << totalAvg << " ms   headroom: " << headroomMs << " ms (" << headroomPct
               << "%)";
            return os.str();
        }

        /// Clears all recorded samples (does not reset which phases exist).
        void Reset()
        {
            for (Ring& r : m_rings)
                r = Ring{};
        }

      private:
        TFPerfCounters() = default;

        struct Ring
        {
            std::array<double, kRingSize> samples{};
            double sum = 0.0;
            double peak = 0.0;
            uint32_t head = 0;
            uint32_t count = 0;
        };

        std::array<Ring, static_cast<size_t>(Phase::COUNT)> m_rings{};
    };

} // namespace Terrafront
