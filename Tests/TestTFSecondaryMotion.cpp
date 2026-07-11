/**
 * @file TestTFSecondaryMotion.cpp
 * @brief TERRAFRONT secondary-motion chain math (W8): Verlet pendulum chains
 *        with hard link constraints — exact link lengths every frame, teleport
 *        snap instead of whip, damped settling toward the rest pose, impulse
 *        deflection, dt clamping, and the handle registry.
 *
 * Game/TFSecondaryMotion.cpp is compiled into SparkTests explicitly (the
 * TFDatabase.cpp pattern — see Tests/CMakeLists.txt): it is pure presentation
 * math (DirectXMath on Windows / Core stubs elsewhere), no Jolt, no net, no
 * GPU resources.
 */
#include "TestFramework.h"

#include "Game/TFSecondaryMotion.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Terrafront;
using DirectX::XMFLOAT3;

namespace
{
    constexpr float kLenTol = 1.0e-3f; ///< link-length tolerance (float accumulation)

    float Dist(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    /// Every adjacent joint pair must sit exactly one link length apart.
    void ExpectConstraintsHold(const TFPendulumChain& chain, float linkLengthM)
    {
        for (int i = 0; i < chain.LinkCount(); ++i)
        {
            XMFLOAT3 a{}, b{};
            chain.GetLinkEndpoints(i, a, b);
            EXPECT_NEAR(Dist(a, b), linkLengthM, kLenTol);
        }
    }

    XMFLOAT3 TipOf(const TFPendulumChain& chain)
    {
        XMFLOAT3 a{}, b{};
        chain.GetLinkEndpoints(chain.LinkCount() - 1, a, b);
        return b;
    }
} // namespace

TEST(TFSecondaryMotion_FirstUpdateSnapsToRestPose)
{
    TFPendulumParams p;
    p.linkCount = 4;
    p.linkLengthM = 0.1f;

    TFPendulumChain chain;
    chain.Configure(p);
    EXPECT_FALSE(chain.Ready());
    EXPECT_EQ(chain.LinkCount(), 4);

    const XMFLOAT3 anchor{3.0f, 5.0f, -2.0f};
    chain.Update(anchor, 1.0f / 60.0f);
    EXPECT_TRUE(chain.Ready());

    // Rest pose: joints stacked straight down the (default) restDir from the anchor.
    for (int i = 0; i < chain.LinkCount(); ++i)
    {
        XMFLOAT3 a{}, b{};
        chain.GetLinkEndpoints(i, a, b);
        EXPECT_NEAR(a.x, anchor.x, kLenTol);
        EXPECT_NEAR(a.z, anchor.z, kLenTol);
        EXPECT_NEAR(a.y, anchor.y - p.linkLengthM * static_cast<float>(i), kLenTol);
        EXPECT_NEAR(b.y, anchor.y - p.linkLengthM * static_cast<float>(i + 1), kLenTol);
    }
}

TEST(TFSecondaryMotion_ConstraintLengthsHoldUnderMotion)
{
    TFPendulumParams p;
    p.linkCount = 5;
    p.linkLengthM = 0.07f;

    TFPendulumChain chain;
    chain.Configure(p);

    // Swing the anchor around aggressively (well under teleportSnapM per step)
    // with periodic impulses: the ordered projection pass must keep every link
    // rigid on every single frame.
    XMFLOAT3 anchor{0.0f, 2.0f, 0.0f};
    chain.Update(anchor, 1.0f / 60.0f);
    for (int frame = 0; frame < 240; ++frame)
    {
        const float t = static_cast<float>(frame) / 60.0f;
        anchor.x = std::sin(t * 5.0f) * 0.8f;
        anchor.y = 2.0f + std::cos(t * 3.0f) * 0.4f;
        anchor.z = std::sin(t * 2.0f) * 0.6f;
        if (frame % 30 == 0)
            chain.AddImpulse(XMFLOAT3{4.0f, 1.0f, -3.0f}); // weapon-fire style punch
        chain.Update(anchor, 1.0f / 60.0f);
        ExpectConstraintsHold(chain, p.linkLengthM);
    }
}

TEST(TFSecondaryMotion_TeleportSnapsInsteadOfWhipping)
{
    TFPendulumParams p;
    p.linkCount = 3;
    p.linkLengthM = 0.05f;
    p.teleportSnapM = 2.0f;

    TFPendulumChain chain;
    chain.Configure(p);

    XMFLOAT3 anchor{0.0f, 1.0f, 0.0f};
    chain.Update(anchor, 1.0f / 60.0f);
    // Build up some lateral velocity so a whip WOULD be visible if the snap failed.
    for (int frame = 0; frame < 30; ++frame)
    {
        anchor.x += 0.02f;
        chain.Update(anchor, 1.0f / 60.0f);
    }

    // Redeploy-style jump (farAnchor: bare far is a legacy Windows macro) beyond teleportSnapM: chain must arrive at rest,
    // not swing across the map.
    const XMFLOAT3 farAnchor{500.0f, 30.0f, -500.0f};
    chain.Update(farAnchor, 1.0f / 60.0f);
    ExpectConstraintsHold(chain, p.linkLengthM);
    const XMFLOAT3 tip = TipOf(chain);
    EXPECT_NEAR(tip.x, farAnchor.x, kLenTol);
    EXPECT_NEAR(tip.y, farAnchor.y - p.linkLengthM * static_cast<float>(p.linkCount), kLenTol);
    EXPECT_NEAR(tip.z, farAnchor.z, kLenTol);

    // And the snap also killed the pre-jump velocity: holding the anchor still
    // must not let the tip drift sideways (gravity only pulls straight down).
    for (int frame = 0; frame < 30; ++frame)
        chain.Update(farAnchor, 1.0f / 60.0f);
    const XMFLOAT3 settled = TipOf(chain);
    EXPECT_NEAR(settled.x, farAnchor.x, 1.0e-2f);
    EXPECT_NEAR(settled.z, farAnchor.z, 1.0e-2f);
}

TEST(TFSecondaryMotion_ImpulseDeflectsThenDampingSettlesBack)
{
    TFPendulumParams p;
    p.linkCount = 3;
    p.linkLengthM = 0.05f;
    p.dampingPerSec = 4.0f;

    TFPendulumChain chain;
    chain.Configure(p);

    const XMFLOAT3 anchor{1.0f, 2.0f, 3.0f};
    chain.Update(anchor, 1.0f / 60.0f);

    // A lateral impulse must visibly deflect the tip off the rest line...
    chain.AddImpulse(XMFLOAT3{6.0f, 0.0f, 0.0f});
    chain.Update(anchor, 1.0f / 60.0f);
    float maxDeflect = 0.0f;
    for (int frame = 0; frame < 30; ++frame)
    {
        chain.Update(anchor, 1.0f / 60.0f);
        maxDeflect = std::max(maxDeflect, std::fabs(TipOf(chain).x - anchor.x));
        ExpectConstraintsHold(chain, p.linkLengthM);
    }
    EXPECT_GT(maxDeflect, 0.01f);

    // ...and damping + gravity must bring it back under the anchor.
    for (int frame = 0; frame < 600; ++frame)
        chain.Update(anchor, 1.0f / 60.0f);
    const XMFLOAT3 tip = TipOf(chain);
    EXPECT_NEAR(tip.x, anchor.x, 5.0e-3f);
    EXPECT_NEAR(tip.z, anchor.z, 5.0e-3f);
    EXPECT_NEAR(tip.y, anchor.y - p.linkLengthM * static_cast<float>(p.linkCount), 5.0e-3f);
}

TEST(TFSecondaryMotion_HitchSafeDtAndBadInputs)
{
    TFPendulumParams p;
    p.linkCount = 4;
    p.linkLengthM = 0.06f;

    TFPendulumChain chain;
    chain.Configure(p);

    const XMFLOAT3 anchor{0.0f, 1.0f, 0.0f};
    chain.Update(anchor, 1.0f / 60.0f);

    // Alt-tab hitch: a huge dt is clamped, the chain stays finite and rigid.
    chain.AddImpulse(XMFLOAT3{50.0f, 20.0f, -50.0f});
    chain.Update(anchor, 10.0f);
    ExpectConstraintsHold(chain, p.linkLengthM);

    // Zero / negative dt: no integration, still rigid.
    chain.Update(anchor, 0.0f);
    chain.Update(anchor, -1.0f);
    ExpectConstraintsHold(chain, p.linkLengthM);

    // Non-finite anchor is ignored entirely (positions unchanged).
    const XMFLOAT3 before = TipOf(chain);
    chain.Update(XMFLOAT3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, 1.0f / 60.0f);
    const XMFLOAT3 after = TipOf(chain);
    EXPECT_NEAR(before.x, after.x, 1.0e-6f);
    EXPECT_NEAR(before.y, after.y, 1.0e-6f);
    EXPECT_NEAR(before.z, after.z, 1.0e-6f);
}

TEST(TFSecondaryMotion_ConfigureClampsDegenerateParams)
{
    TFPendulumChain chain;

    TFPendulumParams p;
    p.linkCount = 0;      // clamped up to 1
    p.linkLengthM = 0.0f; // clamped up to a minimum length
    p.restDir[0] = 0.0f;  // zero restDir falls back to straight down
    p.restDir[1] = 0.0f;
    p.restDir[2] = 0.0f;
    chain.Configure(p);
    EXPECT_EQ(chain.LinkCount(), 1);

    const XMFLOAT3 anchor{0.0f, 0.0f, 0.0f};
    chain.Update(anchor, 1.0f / 60.0f);
    XMFLOAT3 a{}, b{};
    chain.GetLinkEndpoints(0, a, b);
    EXPECT_TRUE(b.y < a.y); // fell back to hanging downward
    EXPECT_GT(Dist(a, b), 0.0f);

    p.linkCount = 99; // clamped down to 16
    chain.Configure(p);
    EXPECT_EQ(chain.LinkCount(), 16);

    // Out-of-range link queries are safe.
    chain.GetLinkEndpoints(-1, a, b);
    EXPECT_NEAR(Dist(a, b), 0.0f, 1.0e-6f);
    chain.GetLinkEndpoints(chain.LinkCount(), a, b);
    EXPECT_NEAR(Dist(a, b), 0.0f, 1.0e-6f);
}

TEST(TFSecondaryMotion_RegistryAttachFindDetach)
{
    TFSecondaryMotion& reg = TFSecondaryMotion::Get();

    TFPendulumParams p;
    p.linkCount = 2;
    const TFPendulumHandle h1 = reg.AttachPendulum(p);
    const TFPendulumHandle h2 = reg.AttachPendulum(p);
    EXPECT_NE(h1, TFPendulumHandle{0});
    EXPECT_NE(h2, TFPendulumHandle{0});
    EXPECT_NE(h1, h2);

    EXPECT_NE(reg.Find(h1), nullptr);
    EXPECT_NE(reg.Find(h2), nullptr);
    EXPECT_EQ(reg.Find(TFPendulumHandle{0}), nullptr);

    reg.Detach(h1);
    EXPECT_EQ(reg.Find(h1), nullptr);
    EXPECT_NE(reg.Find(h2), nullptr);
    reg.Detach(h1); // double-detach is a no-op
    reg.Detach(TFPendulumHandle{0});
    reg.Detach(h2);
    EXPECT_EQ(reg.Find(h2), nullptr);
}
