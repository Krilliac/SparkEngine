/**
 * @file TestTFServerValidation.cpp
 * @brief TERRAFRONT server-authoritative anti-cheat sanity layer (W13):
 *        ValidateMovementTick's horizontal/vertical clamp math + spike
 *        counting, NoteExemptTeleport's one-shot skip, CheckFireOrigin's
 *        divergence reject, AllowInput's 60Hz(+fudge) token bucket, and
 *        ClearPlayer's per-counter session-teardown hygiene.
 *
 * Game/TFServerValidation.cpp is compiled into SparkTests explicitly (the
 * TFDatabase.cpp / TFOutfitStore.cpp pattern — see Tests/CMakeLists.txt): it
 * is deliberately minimal-dependency (Core/TFTypes.h + Utils/LogMacros.h
 * only), no engine/rendering deps, so it links standalone.
 *
 * TFServerValidation::Get() is a process singleton (same lifetime/access
 * pattern as TFSecondaryMotion::Get(), exercised the same way in
 * TestTFSecondaryMotion.cpp's registry test) — every test below uses a
 * PlayerId unique to it and calls ClearPlayer() up front so tests never
 * inherit stale counters/token-bucket state from a previous test.
 */
#include "TestFramework.h"

#include "Game/TFServerValidation.h"

using namespace Terrafront;

namespace
{
    const TFViolationStats& StatsFor(PlayerId player)
    {
        static const TFViolationStats kZero{};
        const auto& all = TFServerValidation::Get().Stats();
        const auto it = all.find(player);
        return it != all.end() ? it->second : kZero;
    }
} // namespace

TEST(TFServerValidation_MovementTick_HorizontalClampPullsPositionBack)
{
    constexpr PlayerId kPlayer = 90001;
    TFServerValidation::Get().ClearPlayer(kPlayer);

    const float prevPos[3] = {0.0f, 0.0f, 0.0f};
    float pos[3] = {0.2f, 0.0f, 0.0f};
    const float maxHorizSpeed = 5.0f;
    const float dt = 1.0f / 60.0f;
    // horizCap = maxHorizSpeed * dt * kHorizFudge(1.5) = 5 * (1/60) * 1.5 = 0.125
    const float horizCap = maxHorizSpeed * dt * 1.5f;

    TFServerValidation::Get().ValidateMovementTick(kPlayer, prevPos, pos, maxHorizSpeed, dt, 0.0);

    // Clamped back to exactly the plausible cap, direction preserved.
    EXPECT_NEAR(pos[0], horizCap, 1.0e-4f);
    EXPECT_NEAR(pos[1], 0.0f, 1.0e-6f);
    EXPECT_NEAR(pos[2], 0.0f, 1.0e-6f);

    // ratio = 0.2 / 0.125 = 1.6 -- clamped, but below kSpikeRatio(2.5).
    EXPECT_EQ(StatsFor(kPlayer).movementClamps, uint32_t{1});
    EXPECT_EQ(StatsFor(kPlayer).movementSpikes, uint32_t{0});
}

TEST(TFServerValidation_MovementTick_GrossOverconsumptionCountsAsSpike)
{
    constexpr PlayerId kPlayer = 90002;
    TFServerValidation::Get().ClearPlayer(kPlayer);

    const float prevPos[3] = {0.0f, 0.0f, 0.0f};
    // horizCap for these params is 0.125; 5x that is a gross (>= 2.5x) overshoot.
    float pos[3] = {0.625f, 0.0f, 0.0f};
    const float maxHorizSpeed = 5.0f;
    const float dt = 1.0f / 60.0f;

    TFServerValidation::Get().ValidateMovementTick(kPlayer, prevPos, pos, maxHorizSpeed, dt, 0.0);

    EXPECT_EQ(StatsFor(kPlayer).movementClamps, uint32_t{1});
    EXPECT_EQ(StatsFor(kPlayer).movementSpikes, uint32_t{1});

    // A second, merely-mild clamp on the same player accumulates the clamp
    // counter but must NOT add another spike.
    float pos2[3] = {0.2f, 0.0f, 0.0f};
    TFServerValidation::Get().ValidateMovementTick(kPlayer, prevPos, pos2, maxHorizSpeed, dt, 1.0);
    EXPECT_EQ(StatsFor(kPlayer).movementClamps, uint32_t{2});
    EXPECT_EQ(StatsFor(kPlayer).movementSpikes, uint32_t{1});
}

TEST(TFServerValidation_MovementTick_VerticalClampBothDirections)
{
    constexpr PlayerId kPlayer = 90003;
    TFServerValidation::Get().ClearPlayer(kPlayer);

    const float prevPos[3] = {0.0f, 10.0f, 0.0f};

    // Upward: dy = 5.0 > kMaxVerticalDeltaPerTickM(3.0) -- clamped to +3.0.
    float posUp[3] = {0.0f, 15.0f, 0.0f};
    TFServerValidation::Get().ValidateMovementTick(kPlayer, prevPos, posUp, 5.0f, 1.0f / 60.0f, 0.0);
    EXPECT_NEAR(posUp[1], 13.0f, 1.0e-4f);
    EXPECT_EQ(StatsFor(kPlayer).movementClamps, uint32_t{1});
    // ratio = 5/3 = 1.67 -- below the spike threshold.
    EXPECT_EQ(StatsFor(kPlayer).movementSpikes, uint32_t{0});

    // Downward: dy = -5.0 -- clamped to -3.0.
    float posDown[3] = {0.0f, 5.0f, 0.0f};
    TFServerValidation::Get().ValidateMovementTick(kPlayer, prevPos, posDown, 5.0f, 1.0f / 60.0f, 0.0);
    EXPECT_NEAR(posDown[1], 7.0f, 1.0e-4f);
    EXPECT_EQ(StatsFor(kPlayer).movementClamps, uint32_t{2});
}

TEST(TFServerValidation_MovementTick_DegenerateTickIsSkippedUnmodified)
{
    constexpr PlayerId kPlayer = 90004;
    TFServerValidation::Get().ClearPlayer(kPlayer);

    const float prevPos[3] = {0.0f, 0.0f, 0.0f};

    // dt <= 0 (rooted / degenerate tick): position passes through untouched.
    float posA[3] = {100.0f, 0.0f, 0.0f};
    TFServerValidation::Get().ValidateMovementTick(kPlayer, prevPos, posA, 5.0f, 0.0f, 0.0);
    EXPECT_NEAR(posA[0], 100.0f, 1.0e-6f);
    EXPECT_EQ(StatsFor(kPlayer).movementClamps, uint32_t{0});

    // maxHorizSpeed <= 0 (ability speedMult 0): also skipped untouched.
    float posB[3] = {100.0f, 0.0f, 0.0f};
    TFServerValidation::Get().ValidateMovementTick(kPlayer, prevPos, posB, 0.0f, 1.0f / 60.0f, 0.0);
    EXPECT_NEAR(posB[0], 100.0f, 1.0e-6f);
    EXPECT_EQ(StatsFor(kPlayer).movementClamps, uint32_t{0});
}

TEST(TFServerValidation_NoteExemptTeleport_SkipsExactlyOneNextTick)
{
    constexpr PlayerId kPlayer = 90005;
    TFServerValidation::Get().ClearPlayer(kPlayer);

    const float prevPos[3] = {0.0f, 0.0f, 0.0f};
    const float maxHorizSpeed = 5.0f;
    const float dt = 1.0f / 60.0f;

    TFServerValidation::Get().NoteExemptTeleport(kPlayer);

    // First call after the exemption: a redeploy-sized jump must pass through
    // completely untouched and must not be counted.
    float pos[3] = {500.0f, 0.0f, -500.0f};
    TFServerValidation::Get().ValidateMovementTick(kPlayer, prevPos, pos, maxHorizSpeed, dt, 0.0);
    EXPECT_NEAR(pos[0], 500.0f, 1.0e-6f);
    EXPECT_NEAR(pos[2], -500.0f, 1.0e-6f);
    EXPECT_EQ(StatsFor(kPlayer).movementClamps, uint32_t{0});

    // The exemption is one-shot: the SAME displacement on the next tick is
    // validated normally and gets clamped.
    float pos2[3] = {500.0f, 0.0f, -500.0f};
    TFServerValidation::Get().ValidateMovementTick(kPlayer, prevPos, pos2, maxHorizSpeed, dt, 1.0);
    EXPECT_LT(pos2[0], 500.0f);
    EXPECT_EQ(StatsFor(kPlayer).movementClamps, uint32_t{1});
}

TEST(TFServerValidation_CheckFireOrigin_AcceptsWithinDivergenceRejectsGross)
{
    constexpr PlayerId kPlayer = 90006;
    TFServerValidation::Get().ClearPlayer(kPlayer);

    const float trusted[3] = {10.0f, 2.0f, 10.0f};
    const float maxDivergenceM = 1.5f;

    // Within tolerance: accepted, nothing counted.
    const float claimedClose[3] = {10.5f, 2.0f, 10.5f};
    EXPECT_TRUE(TFServerValidation::Get().CheckFireOrigin(kPlayer, claimedClose, trusted, maxDivergenceM));
    EXPECT_EQ(StatsFor(kPlayer).fireOriginRejects, uint32_t{0});

    // Gross divergence: rejected and counted.
    const float claimedFar[3] = {50.0f, 2.0f, 10.0f};
    EXPECT_FALSE(TFServerValidation::Get().CheckFireOrigin(kPlayer, claimedFar, trusted, maxDivergenceM));
    EXPECT_EQ(StatsFor(kPlayer).fireOriginRejects, uint32_t{1});

    // A second rejection accumulates.
    EXPECT_FALSE(TFServerValidation::Get().CheckFireOrigin(kPlayer, claimedFar, trusted, maxDivergenceM));
    EXPECT_EQ(StatsFor(kPlayer).fireOriginRejects, uint32_t{2});

    // Exactly at the boundary (distance == maxDivergenceM) is still accepted
    // (CheckFireOrigin compares squared distance with <=).
    const float claimedAtEdge[3] = {10.0f + maxDivergenceM, 2.0f, 10.0f};
    EXPECT_TRUE(TFServerValidation::Get().CheckFireOrigin(kPlayer, claimedAtEdge, trusted, maxDivergenceM));
    EXPECT_EQ(StatsFor(kPlayer).fireOriginRejects, uint32_t{2});
}

TEST(TFServerValidation_RecordFireRateReject_MirrorsCounterPerPlayer)
{
    constexpr PlayerId kPlayer = 90007;
    constexpr PlayerId kOtherPlayer = 90008;
    TFServerValidation::Get().ClearPlayer(kPlayer);
    TFServerValidation::Get().ClearPlayer(kOtherPlayer);

    TFServerValidation::Get().RecordFireRateReject(kPlayer);
    TFServerValidation::Get().RecordFireRateReject(kPlayer);
    TFServerValidation::Get().RecordFireRateReject(kPlayer);
    EXPECT_EQ(StatsFor(kPlayer).fireRateRejects, uint32_t{3});

    // Per-player: an untouched player is unaffected.
    EXPECT_EQ(StatsFor(kOtherPlayer).fireRateRejects, uint32_t{0});
}

TEST(TFServerValidation_AllowInput_TokenBucketCapacityThenRefill)
{
    constexpr PlayerId kPlayer = 90009;
    TFServerValidation::Get().ClearPlayer(kPlayer);

    // Bucket capacity is 4 tokens; four calls at the SAME timestamp (no
    // refill between them) must all be allowed, draining it to empty.
    const double now = 100.0;
    EXPECT_TRUE(TFServerValidation::Get().AllowInput(kPlayer, now));
    EXPECT_TRUE(TFServerValidation::Get().AllowInput(kPlayer, now));
    EXPECT_TRUE(TFServerValidation::Get().AllowInput(kPlayer, now));
    EXPECT_TRUE(TFServerValidation::Get().AllowInput(kPlayer, now));

    // A 5th call at the same instant finds the bucket empty: dropped.
    EXPECT_FALSE(TFServerValidation::Get().AllowInput(kPlayer, now));
    EXPECT_EQ(StatsFor(kPlayer).inputRateRejects, uint32_t{1});

    // Advancing the clock refills at kServerTickHz(60) * kInputRateFudge(1.5)
    // = 90 tokens/sec; 20ms later there is ~1.8 tokens available again.
    EXPECT_TRUE(TFServerValidation::Get().AllowInput(kPlayer, now + 0.02));
    EXPECT_EQ(StatsFor(kPlayer).inputRateRejects, uint32_t{1}); // unchanged -- this call succeeded
}

TEST(TFServerValidation_AllowInput_PerPlayerBucketsAreIndependent)
{
    constexpr PlayerId kPlayerA = 90010;
    constexpr PlayerId kPlayerB = 90011;
    TFServerValidation::Get().ClearPlayer(kPlayerA);
    TFServerValidation::Get().ClearPlayer(kPlayerB);

    const double now = 200.0;
    // Drain player A's bucket completely.
    for (int i = 0; i < 4; ++i)
        EXPECT_TRUE(TFServerValidation::Get().AllowInput(kPlayerA, now));
    EXPECT_FALSE(TFServerValidation::Get().AllowInput(kPlayerA, now));

    // Player B, never touched, still has a full bucket at the same instant.
    EXPECT_TRUE(TFServerValidation::Get().AllowInput(kPlayerB, now));
    EXPECT_EQ(StatsFor(kPlayerB).inputRateRejects, uint32_t{0});
}

TEST(TFServerValidation_ClearPlayer_ResetsCountersAndTokenBucket)
{
    constexpr PlayerId kPlayer = 90012;
    TFServerValidation::Get().ClearPlayer(kPlayer);

    // Generate a violation on every counter this class tracks.
    const float prevPos[3] = {0.0f, 0.0f, 0.0f};
    float pos[3] = {0.625f, 0.0f, 0.0f}; // gross overshoot: clamp + spike
    TFServerValidation::Get().ValidateMovementTick(kPlayer, prevPos, pos, 5.0f, 1.0f / 60.0f, 0.0);
    const float trusted[3] = {0.0f, 0.0f, 0.0f};
    const float claimedFar[3] = {50.0f, 0.0f, 0.0f};
    TFServerValidation::Get().CheckFireOrigin(kPlayer, claimedFar, trusted, 1.0f);
    TFServerValidation::Get().RecordFireRateReject(kPlayer);
    const double now = 300.0;
    for (int i = 0; i < 5; ++i) // drains the bucket and rejects the 5th
        TFServerValidation::Get().AllowInput(kPlayer, now);

    EXPECT_GT(StatsFor(kPlayer).movementClamps, uint32_t{0});
    EXPECT_GT(StatsFor(kPlayer).movementSpikes, uint32_t{0});
    EXPECT_GT(StatsFor(kPlayer).fireOriginRejects, uint32_t{0});
    EXPECT_GT(StatsFor(kPlayer).fireRateRejects, uint32_t{0});
    EXPECT_GT(StatsFor(kPlayer).inputRateRejects, uint32_t{0});

    TFServerValidation::Get().ClearPlayer(kPlayer);

    // A recycled PlayerId inherits no stale history: every counter is back
    // at zero (the stats entry itself is erased, not merely zeroed).
    EXPECT_EQ(TFServerValidation::Get().Stats().count(kPlayer), size_t{0});
    EXPECT_EQ(StatsFor(kPlayer).movementClamps, uint32_t{0});
    EXPECT_EQ(StatsFor(kPlayer).movementSpikes, uint32_t{0});
    EXPECT_EQ(StatsFor(kPlayer).fireOriginRejects, uint32_t{0});
    EXPECT_EQ(StatsFor(kPlayer).fireRateRejects, uint32_t{0});
    EXPECT_EQ(StatsFor(kPlayer).inputRateRejects, uint32_t{0});

    // The input-rate token bucket was reset too -- a fresh full bucket is
    // available at the very same timestamp the old one was drained at.
    EXPECT_TRUE(TFServerValidation::Get().AllowInput(kPlayer, now));
}
