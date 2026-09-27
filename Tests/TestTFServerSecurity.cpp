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
#include "Account/TFAccountSystem.h"
#include "Net/TFClientMsgRouting.h"
#include "Net/TFRepProtocol.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

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

// TFMsg::LoadoutExtChange was enter-world gated and dispatched inside
// RouteClientMessage, but RegisterNetHandlers never registered it, so every
// socket client's grenade/suit save was dropped as an unknown message type.
// Registration, teardown and the gate now read one list; every gated id is
// therefore socket-routed.
TEST(TFSec_EveryGatedClientMsgIsSocketRouted)
{
    static_assert(IsEnteredWorldGatedMsg(TFMsg::LoadoutExtChange));
    static_assert(IsEnteredWorldGatedMsg(TFMsg::LoadoutChange));
    static_assert(IsEnteredWorldGatedMsg(TFMsg::ClientInput));
    static_assert(IsEnteredWorldGatedMsg(TFMsg::FactionSelect));
    EXPECT_TRUE(std::find(kTFEnteredWorldGatedMsgs.begin(), kTFEnteredWorldGatedMsgs.end(), TFMsg::LoadoutExtChange) !=
                kTFEnteredWorldGatedMsgs.end());

    // Onboarding and credential ids are how a session enters the world: never gated.
    for (const TFMsg id : kTFOnboardingMsgs)
        EXPECT_FALSE(IsEnteredWorldGatedMsg(id));
    for (const TFMsg id : kTFCredentialMsgs)
        EXPECT_FALSE(IsEnteredWorldGatedMsg(id));

    // No id is listed twice (a duplicate would silently hide a missing one).
    std::vector<TFMsg> all(kTFEnteredWorldGatedMsgs.begin(), kTFEnteredWorldGatedMsgs.end());
    all.insert(all.end(), kTFOnboardingMsgs.begin(), kTFOnboardingMsgs.end());
    all.insert(all.end(), kTFCredentialMsgs.begin(), kTFCredentialMsgs.end());
    std::sort(all.begin(), all.end());
    EXPECT_TRUE(std::adjacent_find(all.begin(), all.end()) == all.end());
    EXPECT_EQ(all.size(), size_t{24});
}

// The stored hash is database-controlled. VerifyPassword used to trust its
// embedded cost (any nonzero uint32 iteration count, any derived-key length),
// so a tampered or corrupt row such as "pbkdf2-sha256$4294967295$00$00" made
// every login attempt for that username run ~4.3e9 HMAC rounds before the
// password was even compared. Without the bounds this test hangs until the
// CTest timeout.
TEST(TFSec_StoredHashParametersAreBoundedBeforeDerivation)
{
    const std::string good = TFAccountSystem::HashPassword("correcthorse1", TFAccountSystem::GenerateSalt());
    EXPECT_TRUE(TFAccountSystem::VerifyPassword("correcthorse1", good));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("wrongpassword", good));

    // good == "pbkdf2-sha256$150000$<32 hex salt>$<64 hex dk>"
    const size_t p1 = good.find('$');
    const size_t p2 = good.find('$', p1 + 1);
    const size_t p3 = good.find('$', p2 + 1);
    ASSERT_TRUE(p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos);
    const std::string salt = good.substr(p2 + 1, p3 - p2 - 1);
    const std::string dk = good.substr(p3 + 1);
    EXPECT_EQ(salt.size(), size_t{32});
    EXPECT_EQ(dk.size(), size_t{64});
    const auto withIters = [&](const std::string& iters)
    { return std::string("pbkdf2-sha256$") + iters + "$" + salt + "$" + dk; };

    // Unbounded cost: rejected without deriving.
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("4294967295")));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("anything", "pbkdf2-sha256$4294967295$00$00"));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("600001")));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("99999")));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("0")));

    // The iteration field must be a fully consumed decimal number.
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("+150000")));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters(" 150000")));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("150000abc")));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("")));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("4294967296150000")));

    // Salt and derived key must be exactly the sizes HashPassword writes (a
    // longer derived key multiplies the PBKDF2 block count).
    const std::string longDk = std::string("pbkdf2-sha256$150000$") + salt + "$" + dk + dk + dk + dk;
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", longDk));
    const std::string shortSalt = std::string("pbkdf2-sha256$150000$") + salt.substr(0, 16) + "$" + dk;
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", shortSalt));
    const std::string badHexSalt = std::string("pbkdf2-sha256$150000$") + std::string(32, 'z') + "$" + dk;
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", badHexSalt));
}
