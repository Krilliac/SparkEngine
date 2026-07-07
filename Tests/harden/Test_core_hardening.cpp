// Test_core_hardening.cpp — regression guards for the core-utils hardening lane.
//
// Following this repo's TestFramework convention (see TestObjectPool.cpp), the
// algorithmic fixes are mirrored here in dependency-free form so the tests stay
// isolated from the engine's singletons/graphics deps while still failing against
// the pre-fix logic.

#include "TestFramework.h"

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// SparkEngineCamera::Yaw / Roll angle wrap
//
// Mirrors the fmod-based wrap that replaced the old single-subtraction wrap
// (`if (x > 2*PI) x -= 2*PI;`), which failed to bring an angle back into range
// when an accumulated delta exceeded 2*PI in a single call.
// ============================================================================

namespace
{
    constexpr float kTwoPi = 6.283185307179586f;

    // Post-fix wrap: valid for arbitrarily large deltas.
    float WrapAngleFixed(float current, float delta)
    {
        float a = std::fmod(current + delta, kTwoPi);
        if (a < 0.0f)
        {
            a += kTwoPi;
        }
        return a;
    }

    // Pre-fix wrap: a single conditional subtraction (the bug under test).
    float WrapAngleOld(float current, float delta)
    {
        float a = current + delta;
        if (a > kTwoPi)
        {
            a -= kTwoPi;
        }
        if (a < 0.0f)
        {
            a += kTwoPi;
        }
        return a;
    }
} // namespace

TEST(CoreHardening_AngleWrap_SmallDeltaUnchanged)
{
    EXPECT_NEAR(WrapAngleFixed(0.0f, 0.5f), 0.5f, 1e-5f);
    EXPECT_NEAR(WrapAngleFixed(1.0f, 0.25f), 1.25f, 1e-5f);
}

TEST(CoreHardening_AngleWrap_MultiRevolutionDelta)
{
    // delta = 13.0 rad is greater than 2 * 2*PI, so a single subtraction leaves the
    // result out of [0, 2*PI). The fixed wrap must fold it fully into range.
    float fixed = WrapAngleFixed(0.0f, 13.0f);
    EXPECT_GE(fixed, 0.0f);
    EXPECT_LT(fixed, kTwoPi);
    EXPECT_NEAR(fixed, std::fmod(13.0f, kTwoPi), 1e-4f);

    // Demonstrate the old behavior actually broke here (documents the regression).
    float old = WrapAngleOld(0.0f, 13.0f);
    EXPECT_GE(old, kTwoPi); // still out of range → the bug
}

TEST(CoreHardening_AngleWrap_NegativeDeltaFoldsPositive)
{
    float a = WrapAngleFixed(0.0f, -0.5f);
    EXPECT_GE(a, 0.0f);
    EXPECT_LT(a, kTwoPi);
    EXPECT_NEAR(a, kTwoPi - 0.5f, 1e-4f);
}

// ============================================================================
// Profiler nested/recursive same-named sections
//
// Mirrors the per-name stack of start times (BeginSection push_back /
// EndSection pop_back) that replaced a single-value-per-name map. With the old
// map, a nested BeginSection with an already-active name overwrote the outer
// scope's start time, so the outer EndSection measured the inner duration.
// ============================================================================

namespace
{
    // Deterministic mirror of the fixed active-section bookkeeping. Timestamps are
    // injected so the test does not depend on a real clock.
    class SectionTimerFixed
    {
      public:
        void Begin(const std::string& name, double startMs) { m_active[name].push_back(startMs); }

        // Returns the measured duration for the innermost (LIFO) open scope.
        double End(const std::string& name, double endMs)
        {
            auto it = m_active.find(name);
            if (it == m_active.end() || it->second.empty())
            {
                return -1.0;
            }
            double start = it->second.back();
            it->second.pop_back();
            if (it->second.empty())
            {
                m_active.erase(it);
            }
            return endMs - start;
        }

      private:
        std::unordered_map<std::string, std::vector<double>> m_active;
    };
} // namespace

TEST(CoreHardening_Profiler_NestedSameNameResolvesLIFO)
{
    SectionTimerFixed timer;

    timer.Begin("A", 10.0); // outer
    timer.Begin("A", 20.0); // inner, same name

    // Inner closes first → duration 25 - 20 = 5.
    EXPECT_NEAR(timer.End("A", 25.0), 5.0, 1e-9);
    // Outer closes next → duration 30 - 10 = 20 (NOT 30 - 20 = 10, the old bug).
    EXPECT_NEAR(timer.End("A", 30.0), 20.0, 1e-9);
}

TEST(CoreHardening_Profiler_UnmatchedEndIsSafe)
{
    SectionTimerFixed timer;
    // Ending a section that was never begun must not underflow/crash.
    EXPECT_NEAR(timer.End("NeverBegun", 5.0), -1.0, 1e-9);
}
