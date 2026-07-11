/**
 * @file TestTFCaptureMath.cpp
 * @brief TERRAFRONT capture-point state machine: the per-region tick math of
 *        the authoritative capture loop — contested freeze, attacker
 *        accumulation, besieger switch, defender 2x bleed-back, empty 1x
 *        decay, and the flip-at-full transition.
 *
 * Module sources are NOT compiled into SparkTests, so the per-region step is
 * reimplemented standalone, mirroring TFRegionSystem::TickCapture
 * (TFRegionSystem.cpp) exactly — the TestTFRegionLattice.cpp pattern (that
 * suite covers the lattice/ownership predicates over the real topology; this
 * one covers the progress state machine those predicates feed). Update BOTH
 * when the rule changes. Constants mirrored: kCaptureTickSec = 1.0,
 * kDefenderDecayMult = 2.0, kEmptyDecayMult = 1.0.
 *
 * Preconditions handled before this math in the real loop (skyanchor /
 * captureSec <= 0 / no capture points => region skipped entirely) are out of
 * scope here and covered by TestTFRegionLattice.
 */
#include "TestFramework.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace
{
    enum Faction : int
    {
        None = 0,
        MRA = 1,
        AUC = 2,
        HLX = 3
    };

    constexpr float kCaptureTickSec = 1.0f;    // mirror: capture loop cadence
    constexpr float kDefenderDecayMult = 2.0f; // mirror: defenders alone bleed 2x
    constexpr float kEmptyDecayMult = 1.0f;    // mirror: nobody present bleeds 1x

    struct CapState
    {
        Faction owner = None;
        Faction capturing = None;
        float progress = 0.0f;
        bool contested = false;
    };

    constexpr uint32_t Mask(Faction f)
    {
        return 1u << static_cast<uint32_t>(f);
    }

    /// One capture tick for one region — mirrors the body of
    /// TFRegionSystem::TickCapture check for check. attackerLinked stands in
    /// for IsCapturable(region, f) (the lattice rule, tested elsewhere).
    /// Returns true when the region flipped this tick.
    bool StepCapture(CapState& st, uint32_t presentMask, bool attackerLinked, float captureSec,
                     float dt = kCaptureTickSec)
    {
        const int distinct = std::popcount(presentMask);
        st.contested = distinct >= 2;

        if (distinct == 1)
        {
            const auto f = static_cast<Faction>(std::countr_zero(presentMask));
            if (f == st.owner)
            {
                st.progress = std::max(0.0f, st.progress - kDefenderDecayMult * dt / captureSec);
                if (st.progress <= 0.0f)
                    st.capturing = None;
            }
            else if (attackerLinked)
            {
                if (st.capturing != f)
                {
                    st.capturing = f; // new besieger starts from zero
                    st.progress = 0.0f;
                }
                st.progress += dt / captureSec;
                if (st.progress >= 1.0f)
                {
                    // FlipOwner: owner change + full state reset.
                    st.owner = f;
                    st.capturing = None;
                    st.progress = 0.0f;
                    st.contested = false;
                    return true;
                }
            }
            // present but not lattice-linked: frozen, nothing advances
        }
        else if (distinct == 0 && st.progress > 0.0f)
        {
            st.progress = std::max(0.0f, st.progress - kEmptyDecayMult * dt / captureSec);
            if (st.progress <= 0.0f)
                st.capturing = None;
        }
        return false;
    }
} // namespace

TEST(TFCapture_LinkedAttackerAloneCapturesInCaptureSecTicks)
{
    CapState st;
    st.owner = AUC;
    // Power-of-two captureSec: dt/captureSec is exactly representable, so the
    // tick count below is deterministic (no float-accumulation off-by-one).
    const float captureSec = 32.0f;

    bool flipped = false;
    int ticks = 0;
    while (!flipped && ticks < 100)
    {
        flipped = StepCapture(st, Mask(MRA), /*linked*/ true, captureSec);
        ++ticks;
    }
    EXPECT_TRUE(flipped);
    EXPECT_EQ(ticks, 32); // exactly captureSec worth of 1 Hz ticks
    // Flip resets the whole point state.
    EXPECT_EQ(int{st.owner}, int{MRA});
    EXPECT_EQ(int{st.capturing}, int{None});
    EXPECT_NEAR(st.progress, 0.0f, 1.0e-6f);
    EXPECT_FALSE(st.contested);
}

TEST(TFCapture_UnlinkedAttackerIsFrozen)
{
    CapState st;
    st.owner = AUC;
    for (int i = 0; i < 50; ++i)
        EXPECT_FALSE(StepCapture(st, Mask(MRA), /*linked*/ false, 30.0f));
    EXPECT_NEAR(st.progress, 0.0f, 1.0e-6f);
    EXPECT_EQ(int{st.capturing}, int{None});
    EXPECT_FALSE(st.contested); // one faction present is not a contest
    EXPECT_EQ(int{st.owner}, int{AUC});
}

TEST(TFCapture_TwoFactionsContestAndFreezeProgress)
{
    CapState st;
    st.owner = AUC;

    // Build some attacker progress first.
    for (int i = 0; i < 10; ++i)
        StepCapture(st, Mask(MRA), true, 30.0f);
    const float before = st.progress;
    EXPECT_NEAR(before, 10.0f / 30.0f, 1.0e-4f);

    // Defender walks in: contested, progress frozen exactly where it was.
    for (int i = 0; i < 20; ++i)
        EXPECT_FALSE(StepCapture(st, Mask(MRA) | Mask(AUC), true, 30.0f));
    EXPECT_TRUE(st.contested);
    EXPECT_NEAR(st.progress, before, 1.0e-6f);
    EXPECT_EQ(int{st.capturing}, int{MRA});

    // Third faction piles on: still just contested.
    EXPECT_FALSE(StepCapture(st, Mask(MRA) | Mask(AUC) | Mask(HLX), true, 30.0f));
    EXPECT_TRUE(st.contested);
    EXPECT_NEAR(st.progress, before, 1.0e-6f);

    // Attackers leave: contest clears and the defender bleeds it back.
    StepCapture(st, Mask(AUC), true, 30.0f);
    EXPECT_FALSE(st.contested);
    EXPECT_NEAR(st.progress, before - 2.0f / 30.0f, 1.0e-4f);
}

TEST(TFCapture_DefenderBleedsTwiceAsFastAsEmpty)
{
    const float captureSec = 30.0f;

    CapState defended;
    defended.owner = AUC;
    CapState empty;
    empty.owner = AUC;

    // Same siege on both points: 12 ticks of linked MRA attack.
    for (int i = 0; i < 12; ++i)
    {
        StepCapture(defended, Mask(MRA), true, captureSec);
        StepCapture(empty, Mask(MRA), true, captureSec);
    }
    EXPECT_NEAR(defended.progress, 12.0f / 30.0f, 1.0e-4f);

    // 3 ticks of defenders-only vs 3 ticks of nobody.
    for (int i = 0; i < 3; ++i)
    {
        StepCapture(defended, Mask(AUC), true, captureSec);
        StepCapture(empty, 0u, true, captureSec);
    }
    EXPECT_NEAR(defended.progress, (12.0f - 2.0f * 3.0f) / 30.0f, 1.0e-4f);
    EXPECT_NEAR(empty.progress, (12.0f - 1.0f * 3.0f) / 30.0f, 1.0e-4f);
    EXPECT_GT(empty.progress, defended.progress);

    // Bleed to zero clears the besieger and never goes negative.
    for (int i = 0; i < 30; ++i)
        StepCapture(defended, Mask(AUC), true, captureSec);
    EXPECT_NEAR(defended.progress, 0.0f, 1.0e-6f);
    EXPECT_EQ(int{defended.capturing}, int{None});
    EXPECT_EQ(int{defended.owner}, int{AUC});
}

TEST(TFCapture_BesiegerSwitchResetsProgress)
{
    CapState st;
    st.owner = AUC;

    for (int i = 0; i < 15; ++i)
        StepCapture(st, Mask(MRA), true, 30.0f);
    EXPECT_EQ(int{st.capturing}, int{MRA});
    EXPECT_NEAR(st.progress, 0.5f, 1.0e-4f);

    // HLX takes over the point: MRA's half-bar does NOT carry across.
    StepCapture(st, Mask(HLX), true, 30.0f);
    EXPECT_EQ(int{st.capturing}, int{HLX});
    EXPECT_NEAR(st.progress, 1.0f / 30.0f, 1.0e-4f); // restarted, one tick in
}

TEST(TFCapture_EmptyPointAtZeroStaysInert)
{
    CapState st;
    st.owner = AUC;
    for (int i = 0; i < 10; ++i)
        EXPECT_FALSE(StepCapture(st, 0u, true, 30.0f));
    EXPECT_NEAR(st.progress, 0.0f, 1.0e-6f);
    EXPECT_EQ(int{st.capturing}, int{None});
    EXPECT_FALSE(st.contested);
}

TEST(TFCapture_NewOwnerDefendingFreshPointGainsNothing)
{
    CapState st;
    st.owner = AUC;

    // Full capture by MRA, then MRA stands on its new point: nothing accrues,
    // nothing decays below zero, no self-capture.
    bool flipped = false;
    for (int i = 0; i < 100 && !flipped; ++i)
        flipped = StepCapture(st, Mask(MRA), true, 10.0f);
    EXPECT_TRUE(flipped);
    EXPECT_EQ(int{st.owner}, int{MRA});
    for (int i = 0; i < 20; ++i)
        EXPECT_FALSE(StepCapture(st, Mask(MRA), true, 10.0f));
    EXPECT_EQ(int{st.owner}, int{MRA});
    EXPECT_NEAR(st.progress, 0.0f, 1.0e-6f);
    EXPECT_EQ(int{st.capturing}, int{None});
}
