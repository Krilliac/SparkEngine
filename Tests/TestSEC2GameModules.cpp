/**
 * @file TestSEC2GameModules.cpp
 * @brief SEC2 game-module security fixes: bounded console inputs, fog-of-war work bounds,
 *        owner-scoped network handlers across module hot reload, and server-attributed chat.
 *
 * Each SEC2GM_ test pins one fix from the SEC2-game-modules lane:
 * - ProgressionSystem::AwardXP saturates instead of reaching float-to-int or signed-overflow UB.
 * - WaveComposition bounds every wave to MAX_ENEMIES_PER_WAVE, heavies included.
 * - NetworkManager handler ownership: a hot-reload replacement keeps its handlers when the outgoing
 *   image tears down, and everything an image owns is removed before it is unmapped.
 * - MMO chat relays only routable channels and never forwards a client-chosen sender name.
 * - RTS fog of war clips vision to the grid and restored saves reject absurd vision ranges.
 *
 * The CTest registration SEC2GameModules pins the family size, which depends on the
 * ImGui (module sources) and networking features compiled into SparkTests.
 */

#include "TestFramework.h"

#include "../GameModules/SparkGameFPS/Source/Game/ProgressionSystem.h"

#include <climits>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// FPS progression (xp console command)
// ---------------------------------------------------------------------------

TEST(SEC2GM_ProgressionAwardSaturatesInsteadOfOverflowing)
{
    Spark::ProgressionSystem progression;
    progression.Initialize();
    int observedBase = -1;
    int observedModified = -1;
    progression.GetCallbacks().onXPAwarded = [&](int base, const std::string&, int modified)
    {
        observedBase = base;
        observedModified = modified;
    };

    // float(INT_MAX) rounds up to 2^31; the old cast back to int was undefined and produced INT_MIN,
    // leaving a negative XP total that was then persisted.
    progression.AwardXP(INT_MAX, "console");
    EXPECT_EQ(observedBase, Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(observedModified, Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::MAX_SINGLE_AWARD);
    EXPECT_EQ(progression.GetLevel(), progression.GetMaxLevel());

    // Awards stop at max level, so repetition cannot push the total toward INT_MAX either.
    progression.AwardXP(INT_MAX, "console");
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::MAX_SINGLE_AWARD);

    // Ordinary gameplay awards are unchanged.
    Spark::ProgressionSystem ordinary;
    ordinary.Initialize();
    ordinary.AwardXP(Spark::ProgressionSystem::XP_PER_KILL, "kill");
    EXPECT_EQ(ordinary.GetCurrentXP(), Spark::ProgressionSystem::XP_PER_KILL);
}

TEST(SEC2GM_ProgressionIgnoresNonPositiveAwards)
{
    Spark::ProgressionSystem progression;
    progression.Initialize();
    progression.AwardXP(Spark::ProgressionSystem::XP_PER_KILL, "kill");

    progression.AwardXP(0, "console");
    progression.AwardXP(-500, "console");
    progression.AwardXP(INT_MIN, "console");
    EXPECT_EQ(progression.GetCurrentXP(), Spark::ProgressionSystem::XP_PER_KILL);
    EXPECT_EQ(progression.GetLevel(), 1);
}
