/** @file TestTFReplicationRefresh.cpp @brief Actual sender-cache loss recovery and entity lifetime. */
#include "TestFramework.h"

#include "Net/TFRepProtocol.h"
#include "Net/TFReplicationRefresh.h"

#include <cstdint>
#include <limits>

namespace
{
    struct PawnState
    {
        Terrafront::QuantPos position;
        uint16_t health;
        bool operator==(const PawnState&) const = default;
    };

    PawnState StateAt(float x, uint16_t health = 450)
    {
        const float position[3] = {x, 24.0f, 3744.0f};
        return {Terrafront::QuantPos::From(position), health};
    }
} // namespace

TEST(TFReplicationRefresh_DroppedFinalUpdateRecoversWhilePawnStaysStationary)
{
    Terrafront::TFReplicationStateCache<PawnState> sender;
    constexpr Terrafront::EntityId entity = 42;
    const PawnState original = StateAt(296.0f);
    const PawnState moved = StateAt(320.0f, 400);
    PawnState receiver = original; // reliable Create established identity/state
    sender.Remember(entity, original, 0.0);
    unsigned int sends = 0;
    auto deliver = [&](double clock, const PawnState& state, bool drop)
    {
        if (sender.ShouldSend(entity, state, clock))
        {
            ++sends;
            if (!drop)
            {
                receiver = state;
            }
        }
    };

    // The final changed snapshot is lost. No later movement can incidentally
    // repair it: all subsequent ticks use this identical authoritative state.
    deliver(0.25, moved, true);
    EXPECT_TRUE(receiver == original);
    for (unsigned int tick = 6; tick < 25; ++tick)
    {
        deliver(static_cast<double>(tick) / Terrafront::kReplicationHz, moved, false);
        EXPECT_TRUE(receiver == original);
    }
    EXPECT_EQ(sends, 1u);
    deliver(1.25, moved, false);
    EXPECT_TRUE(receiver == moved);
    EXPECT_EQ(sends, 2u);
}

TEST(TFReplicationRefresh_QuietTrafficIsBoundedAndDirtyChangesSendImmediately)
{
    Terrafront::TFReplicationStateCache<PawnState> sender;
    constexpr Terrafront::EntityId entity = 43;
    const auto initial = StateAt(296.0f);
    sender.Remember(entity, initial, 0.0);
    unsigned int quietSends = 0;
    for (unsigned int tick = 1; tick <= 100; ++tick)
    {
        if (sender.ShouldSend(entity, initial, static_cast<double>(tick) / Terrafront::kReplicationHz))
        {
            ++quietSends;
        }
    }
    EXPECT_EQ(quietSends, 5u); // five seconds, not the full 100-tick update rate
    EXPECT_TRUE(sender.ShouldSend(entity, StateAt(300.0f), 5.01));
    EXPECT_TRUE(sender.ShouldSend(entity, StateAt(300.0f, 399), 5.02));
    EXPECT_FALSE(sender.ShouldSend(entity, StateAt(300.0f, 399), 5.03));
}

TEST(TFReplicationRefresh_IndependentEntitiesCannotPostponeEachOthersRecovery)
{
    Terrafront::TFReplicationStateCache<PawnState> sender;
    const auto stationary = StateAt(296.0f);
    sender.Remember(44, stationary, 0.0);
    sender.Remember(45, stationary, 0.5);
    EXPECT_TRUE(sender.ShouldSend(44, stationary, 1.0));
    EXPECT_FALSE(sender.ShouldSend(45, stationary, 1.0));
    EXPECT_TRUE(sender.ShouldSend(45, stationary, 1.5));
}

TEST(TFReplicationRefresh_DespawnErasesStateBeforeEntityIdReuse)
{
    Terrafront::TFReplicationStateCache<PawnState> sender;
    const auto state = StateAt(296.0f);
    sender.Remember(46, state, 10.0);
    sender.Remember(47, state, 10.0);
    sender.Erase(46);
    EXPECT_EQ(sender.Size(), std::size_t{1});
    EXPECT_FALSE(sender.Entries().contains(46));
    EXPECT_TRUE(sender.ShouldSend(46, state, 10.01));
    EXPECT_FALSE(sender.ShouldSend(47, state, 10.01));
}

TEST(TFReplicationRefresh_ServerDepartureClearsEveryEntityRefreshDeadline)
{
    Terrafront::TFReplicationStateCache<PawnState> sender;
    const auto state = StateAt(296.0f);
    sender.Remember(48, state, 20.0);
    sender.Remember(49, state, 20.0);
    sender.Clear(); // actual server-role departure and shutdown use this path
    EXPECT_TRUE(sender.Empty());
    EXPECT_EQ(sender.Size(), std::size_t{0});
    EXPECT_TRUE(sender.ShouldSend(48, state, 0.0));
    EXPECT_TRUE(sender.ShouldSend(49, state, 0.0));
}

TEST(TFReplicationRefresh_ClockRestartCannotSuppressStateBehindOldDeadline)
{
    Terrafront::TFReplicationStateCache<PawnState> sender;
    const auto state = StateAt(296.0f);
    sender.Remember(50, state, 100.0);
    EXPECT_TRUE(sender.ShouldSend(50, state, 0.0));
    EXPECT_FALSE(sender.ShouldSend(50, state, 0.5));
    EXPECT_TRUE(sender.ShouldSend(50, state, 1.0));
}

TEST(TFReplicationRefresh_NonFiniteClocksSendAndFiniteTimeRestoresRecovery)
{
    Terrafront::TFReplicationStateCache<PawnState> sender;
    const auto state = StateAt(296.0f);
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
    {
        sender.Remember(51, state, 10.0);
        EXPECT_TRUE(sender.ShouldSend(51, state, invalid));
        EXPECT_TRUE(sender.ShouldSend(51, state, 0.0));
        EXPECT_FALSE(sender.ShouldSend(51, state, 0.5));
        EXPECT_TRUE(sender.ShouldSend(51, state, 1.0));
    }
}
