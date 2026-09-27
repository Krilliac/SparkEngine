/**
 * @file TestTFServerSecurity.cpp
 * @brief TERRAFRONT (SparkGameMMOFPS) server trust-boundary regressions from the
 *        security review: client-controlled view angles, socket routing of every
 *        gated client id, session/character exclusivity, faction binding,
 *        server-supplied character names, stored-hash parameter bounds and logout.
 *
 * SparkTests does not compile TFServerSim; each fix is exercised through the
 * header-only rule or the standalone module .cpp that the server path calls.
 */
#include "TestFramework.h"
#include "Net/TFRepProtocol.h"

#include <cfloat>
#include <cmath>
#include <limits>

using namespace Terrafront;

namespace
{
    constexpr float kWrapPiBound = 3.14159265f;

    bool IsWrapped(float a)
    {
        return std::isfinite(a) && a >= -kWrapPiBound && a <= kWrapPiBound;
    }
} // namespace

// A huge finite client viewYaw used to spin QuantAim::WrapPi forever: at
// |a| >= 2^27, a - 2*pi rounds back to a, so the authoritative tick never
// returned (walking, seated and turret-seat input paths all call it). Without
// the constant-time wrap this test hangs until the CTest timeout.
TEST(TFSec_WrapPiIsConstantTimeForHugeFiniteAngles)
{
    const float huge[] = {1e30f,         -1e30f, FLT_MAX, -FLT_MAX,
                          134217728.0f, // 2^27: first magnitude where one 2*pi step is lost
                          -134217728.0f, 1e8f,   -1e8f};
    for (const float a : huge)
    {
        const float w = QuantAim::WrapPi(a);
        EXPECT_TRUE(IsWrapped(w));
    }

    // Non-finite input is total (0), never a loop or a propagated NaN.
    EXPECT_EQ(QuantAim::WrapPi(std::numeric_limits<float>::infinity()), 0.0f);
    EXPECT_EQ(QuantAim::WrapPi(-std::numeric_limits<float>::infinity()), 0.0f);
    EXPECT_EQ(QuantAim::WrapPi(std::numeric_limits<float>::quiet_NaN()), 0.0f);

    // Quantization of a hostile angle stays inside the int16 wire range.
    const QuantAim q = QuantAim::From(1e30f, -FLT_MAX);
    EXPECT_TRUE(q.yaw >= -31416 && q.yaw <= 31416);
    EXPECT_TRUE(q.pitch >= -31416 && q.pitch <= 31416);
}

TEST(TFSec_WrapPiKeepsInRangeAnglesAndWrapsSmallMultiples)
{
    // In-range input is returned bit-for-bit (client prediction parity).
    const float inRange[] = {0.0f, 1.0f, -1.0f, 3.0f, -3.0f, 3.14159265f, -3.14159265f};
    for (const float a : inRange)
        EXPECT_EQ(QuantAim::WrapPi(a), a);

    const float twoPi = 6.28318531f;
    EXPECT_NEAR(QuantAim::WrapPi(1.0f + twoPi), 1.0f, 1e-5f);
    EXPECT_NEAR(QuantAim::WrapPi(-1.0f - twoPi), -1.0f, 1e-5f);
    EXPECT_NEAR(QuantAim::WrapPi(0.5f + 3.0f * twoPi), 0.5f, 1e-4f);
    EXPECT_NEAR(QuantAim::WrapPi(4.0f), 4.0f - twoPi, 1e-5f);
    EXPECT_NEAR(QuantAim::WrapPi(-4.0f), -4.0f + twoPi, 1e-5f);
}
