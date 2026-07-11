/**
 * @file TestTFDeathRecapWire.cpp
 * @brief TERRAFRONT W11 death-recap lane: frozen wire layout for the 0x5474
 *        block and the TFDamageLog rolling-ring semantics (window filter,
 *        oldest-first ordering, overwrite-on-overflow, per-attacker sums).
 *
 * Module sources are NOT compiled into SparkTests for this suite
 * (TestTFRedeployRules/TestTFAbilityWire pattern): everything under test is
 * header-only — Game/TFDamageSystem.h wire PODs/constants and the inline
 * TFDamageLog ring. The ring matters because TFDamageSystem::SendDeathRecap
 * reads the victim's timeline AND the killer's "damage dealt to killer" sum
 * from it; a wrong window/order silently produces a plausible-looking but
 * false recap that no compile error would ever catch.
 */
#include "TestFramework.h"

#include "Game/TFDamageSystem.h"

#include <cstddef>
#include <cstdint>

using namespace Terrafront;

// ============================================================================
// Wire layout freeze (0x5474 block, death-recap lane)
// ============================================================================

static_assert(kTFMsgDeathRecap == 0x5474, "reserved id frozen");

static_assert(sizeof(TF_DeathRecapEntry) == 12, "wire layout frozen");
static_assert(offsetof(TF_DeathRecapEntry, attackerPlayer) == 0, "wire layout frozen");
static_assert(offsetof(TF_DeathRecapEntry, weaponId) == 4, "wire layout frozen");
static_assert(offsetof(TF_DeathRecapEntry, damage) == 6, "wire layout frozen");
static_assert(offsetof(TF_DeathRecapEntry, ageDeciSec) == 8, "wire layout frozen");
static_assert(offsetof(TF_DeathRecapEntry, headshot) == 10, "wire layout frozen");
static_assert(offsetof(TF_DeathRecapEntry, damageKind) == 11, "wire layout frozen");

static_assert(kTFDeathRecapMaxEntries == 8, "ring/wire cap frozen");
static_assert(sizeof(TF_DeathRecap) == 112, "wire layout frozen");
static_assert(offsetof(TF_DeathRecap, entryCount) == 0, "wire layout frozen");
static_assert(offsetof(TF_DeathRecap, killerClass) == 1, "wire layout frozen");
static_assert(offsetof(TF_DeathRecap, killerFaction) == 2, "wire layout frozen");
static_assert(offsetof(TF_DeathRecap, hitsOnKiller) == 3, "wire layout frozen");
static_assert(offsetof(TF_DeathRecap, killerPlayer) == 4, "wire layout frozen");
static_assert(offsetof(TF_DeathRecap, killerHealth) == 8, "wire layout frozen");
static_assert(offsetof(TF_DeathRecap, killerShield) == 10, "wire layout frozen");
static_assert(offsetof(TF_DeathRecap, damageToKiller) == 12, "wire layout frozen");
static_assert(offsetof(TF_DeathRecap, distanceDm) == 14, "wire layout frozen");
static_assert(offsetof(TF_DeathRecap, entries) == 16, "wire layout frozen");

// ============================================================================
// TFDamageLog ring semantics
// ============================================================================

namespace
{
    TFDamageLogHit Hit(PlayerId attacker, float amount, double at, WeaponId weapon = 3, bool headshot = false)
    {
        TFDamageLogHit h;
        h.attackerPlayer = attacker;
        h.weapon = weapon;
        h.amount = amount;
        h.kind = 0;
        h.headshot = headshot;
        h.at = at;
        return h;
    }
} // namespace

TEST(TFDeathRecap_CollectReturnsOldestFirst)
{
    TFDamageLog log;
    log.Push(Hit(1, 10.0f, 1.0));
    log.Push(Hit(2, 20.0f, 2.0));
    log.Push(Hit(3, 30.0f, 3.0));

    TFDamageLogHit rows[kTFDeathRecapMaxEntries];
    const uint32_t n = log.Collect(3.0, kTFDeathRecapWindowSec, rows);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(rows[0].attackerPlayer, 1u);
    EXPECT_EQ(rows[1].attackerPlayer, 2u);
    EXPECT_EQ(rows[2].attackerPlayer, 3u);
    EXPECT_NEAR(rows[2].amount, 30.0f, 1e-6f);
}

TEST(TFDeathRecap_OverflowKeepsTheNewestEight)
{
    TFDamageLog log;
    for (uint32_t i = 0; i < 10; ++i) // attackers 0..9, one per second
        log.Push(Hit(i, 1.0f * static_cast<float>(i), static_cast<double>(i)));

    TFDamageLogHit rows[kTFDeathRecapMaxEntries];
    const uint32_t n = log.Collect(9.0, kTFDeathRecapWindowSec, rows);
    EXPECT_EQ(n, 8u);
    EXPECT_EQ(rows[0].attackerPlayer, 2u); // 0 and 1 were overwritten
    EXPECT_EQ(rows[7].attackerPlayer, 9u);
}

TEST(TFDeathRecap_WindowFiltersStaleHits)
{
    TFDamageLog log;
    log.Push(Hit(1, 10.0f, 0.0));  // 20 s old at collect time — outside 8 s
    log.Push(Hit(2, 20.0f, 15.0)); // 5 s old — inside
    log.Push(Hit(3, 30.0f, 19.5)); // 0.5 s old — inside

    TFDamageLogHit rows[kTFDeathRecapMaxEntries];
    const uint32_t n = log.Collect(20.0, kTFDeathRecapWindowSec, rows);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(rows[0].attackerPlayer, 2u);
    EXPECT_EQ(rows[1].attackerPlayer, 3u);
}

TEST(TFDeathRecap_SumFromCountsOnlyThatAttackerInsideTheWindow)
{
    TFDamageLog log;
    log.Push(Hit(7, 40.0f, 0.0));  // attacker 7 but stale at now=20
    log.Push(Hit(7, 25.0f, 14.0)); // counts
    log.Push(Hit(9, 99.0f, 15.0)); // other attacker
    log.Push(Hit(7, 25.0f, 19.0)); // counts

    float dmg = -1.0f;
    uint32_t hits = 99;
    log.SumFrom(7, 20.0, kTFDeathRecapWindowSec, dmg, hits);
    EXPECT_NEAR(dmg, 50.0f, 1e-6f);
    EXPECT_EQ(hits, 2u);

    // Environment hits never attribute to a player.
    log.SumFrom(kInvalidPlayer, 20.0, kTFDeathRecapWindowSec, dmg, hits);
    EXPECT_NEAR(dmg, 0.0f, 1e-6f);
    EXPECT_EQ(hits, 0u);
}

TEST(TFDeathRecap_EmptyLogCollectsNothing)
{
    const TFDamageLog log;
    TFDamageLogHit rows[kTFDeathRecapMaxEntries];
    EXPECT_EQ(log.Collect(100.0, kTFDeathRecapWindowSec, rows), 0u);

    float dmg = -1.0f;
    uint32_t hits = 99;
    log.SumFrom(1, 100.0, kTFDeathRecapWindowSec, dmg, hits);
    EXPECT_NEAR(dmg, 0.0f, 1e-6f);
    EXPECT_EQ(hits, 0u);
}
