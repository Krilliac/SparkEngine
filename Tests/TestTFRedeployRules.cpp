/**
 * @file TestTFRedeployRules.cpp
 * @brief TERRAFRONT W7 redeploy rules: frozen wire layout + reason vocabulary
 *        for the 0x5430 block, the shared timing constants, and the denial
 *        precedence of the client/server predicate.
 *
 * Module sources are NOT compiled into SparkTests for this suite, so the
 * predicate's decision order is reimplemented standalone, mirroring
 * TFRedeployRules::CanRedeploy (TFRedeployRules.cpp) exactly — the
 * TestTFRegionLattice.cpp pattern. The check order IS the contract: both
 * sides show ReasonText(reason) straight off the wire, so a reorder would
 * silently change user-facing denial text (e.g. a dead seated player must
 * hear "you are down", not "exit your vehicle").
 *
 * Header-only includes: TFRedeployProtocol.h (wire PODs + reason ids) and
 * TFRedeployRules.h (inline timing constants). No engine systems booted.
 */
#include "TestFramework.h"

#include "Game/TFRedeployRules.h"
#include "Net/TFRedeployProtocol.h"

#include <cstddef>
#include <cstdint>

using namespace Terrafront;

// ============================================================================
// Wire layout freeze (0x5430 block, ui-map-keys lane)
// ============================================================================

static_assert(kTFMsg_RedeployRequest == 0x5430, "reserved id frozen");
static_assert(kTFMsg_RedeployReply == 0x5431, "reserved id frozen");

static_assert(sizeof(TF_RedeployRequest) == 4, "wire layout frozen");
static_assert(offsetof(TF_RedeployRequest, regionId) == 0, "wire layout frozen");

static_assert(sizeof(TF_RedeployReply) == 20, "wire layout frozen");
static_assert(offsetof(TF_RedeployReply, accepted) == 0, "wire layout frozen");
static_assert(offsetof(TF_RedeployReply, reason) == 1, "wire layout frozen");
static_assert(offsetof(TF_RedeployReply, regionId) == 2, "wire layout frozen");
static_assert(offsetof(TF_RedeployReply, posX) == 4, "wire layout frozen");
static_assert(offsetof(TF_RedeployReply, posY) == 8, "wire layout frozen");
static_assert(offsetof(TF_RedeployReply, posZ) == 12, "wire layout frozen");
static_assert(offsetof(TF_RedeployReply, cooldownSec) == 16, "wire layout frozen");

TEST(TFRedeploy_ReasonVocabularyFrozen)
{
    // Reason bytes travel on the wire and index client denial text: the
    // numeric values may never be renumbered, only appended after 6.
    EXPECT_EQ(int{kTFRedeployOk}, 0);
    EXPECT_EQ(int{kTFRedeployBadRegion}, 1);
    EXPECT_EQ(int{kTFRedeployCooldown}, 2);
    EXPECT_EQ(int{kTFRedeployContested}, 3);
    EXPECT_EQ(int{kTFRedeployInVehicle}, 4);
    EXPECT_EQ(int{kTFRedeployNotAlive}, 5);
    EXPECT_EQ(int{kTFRedeployBlocked}, 6);
}

TEST(TFRedeploy_TimingConstantsSane)
{
    // Client countdown must be shorter than the server's accept interval, or
    // a spam-clicker would be denied by cooldown on every second legal use.
    EXPECT_NEAR(TFRedeployRules::kTFRedeployCountdownSec, 5.0f, 1.0e-6f);
    EXPECT_NEAR(TFRedeployRules::kTFRedeployIntervalSec, 15.0f, 1.0e-6f);
    EXPECT_LT(TFRedeployRules::kTFRedeployCountdownSec, TFRedeployRules::kTFRedeployIntervalSec);
}

// ============================================================================
// Predicate precedence (standalone mirror of TFRedeployRules::CanRedeploy)
// ============================================================================

namespace
{
    /// Flattened view of the sim facts CanRedeploy consults, in the order it
    /// consults them.
    struct RedeployFacts
    {
        bool validFactionAndRegion = true;   ///< faction != None && region != kInvalidRegion
        bool alive = true;                   ///< players->GetPawnByPlayer(...).alive
        bool seated = false;                 ///< vehicles->IsSeated(player)
        bool spawnable = true;               ///< regions->CanSpawnAt(region, faction)
        bool contested = false;              ///< regions->CaptureProgress(...) outContested
        uint8_t extraReason = kTFRedeployOk; ///< installed ExtraRule result (0 == allow)
    };

    /// Mirrors TFRedeployRules::CanRedeploy (TFRedeployRules.cpp) check for
    /// check, in order. Update BOTH when the rule changes.
    uint8_t MirrorCanRedeploy(const RedeployFacts& f)
    {
        if (!f.validFactionAndRegion)
            return kTFRedeployBadRegion;
        if (!f.alive)
            return kTFRedeployNotAlive;
        if (f.seated)
            return kTFRedeployInVehicle;
        if (!f.spawnable)
            return kTFRedeployBadRegion;
        if (f.contested)
            return kTFRedeployContested;
        if (f.extraReason != kTFRedeployOk)
            return f.extraReason;
        return kTFRedeployOk;
    }
} // namespace

TEST(TFRedeploy_HappyPathAllows)
{
    EXPECT_EQ(int{MirrorCanRedeploy(RedeployFacts{})}, int{kTFRedeployOk});
}

TEST(TFRedeploy_EachFactAloneDenies)
{
    RedeployFacts f;

    f = RedeployFacts{};
    f.validFactionAndRegion = false;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployBadRegion});

    f = RedeployFacts{};
    f.alive = false;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployNotAlive});

    f = RedeployFacts{};
    f.seated = true;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployInVehicle});

    f = RedeployFacts{};
    f.spawnable = false;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployBadRegion});

    f = RedeployFacts{};
    f.contested = true;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployContested});

    f = RedeployFacts{};
    f.extraReason = kTFRedeployBlocked; // sanctuary-style lane veto
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployBlocked});
}

TEST(TFRedeploy_DenialPrecedenceIsFrozen)
{
    // Every fact bad at once: the player-state checks outrank the region
    // checks, which outrank the extra rule. This precedence is what the user
    // reads on screen — keep it stable.
    RedeployFacts f;
    f.validFactionAndRegion = false;
    f.alive = false;
    f.seated = true;
    f.spawnable = false;
    f.contested = true;
    f.extraReason = kTFRedeployBlocked;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployBadRegion}); // arg sanity first

    f.validFactionAndRegion = true;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployNotAlive}); // dead beats seated

    f.alive = true;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployInVehicle}); // seated beats region

    f.seated = false;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployBadRegion}); // unspawnable beats contested

    f.spawnable = true;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployContested}); // contested beats extra rule

    f.contested = false;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployBlocked}); // extra rule last

    f.extraReason = kTFRedeployOk;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployOk});
}

TEST(TFRedeploy_ExtraRuleReasonPassesThroughVerbatim)
{
    // The hook's byte is forwarded untouched (TFRedeployRules.h contract), so
    // a lane may return any kTFRedeploy* vocabulary value, not only Blocked.
    RedeployFacts f;
    f.extraReason = kTFRedeployContested;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployContested});
    f.extraReason = kTFRedeployBlocked;
    EXPECT_EQ(int{MirrorCanRedeploy(f)}, int{kTFRedeployBlocked});
}

TEST(TFRedeploy_ServerCooldownIsNotPartOfThePredicate)
{
    // kTFRedeployCooldown exists in the wire vocabulary but is server-only
    // state (TFServerSim's per-player interval) — the shared predicate can
    // never produce it, so no RedeployFacts combination maps to it. This
    // documents that split: a client that showed "on cooldown" from the
    // pre-check would be lying about state it cannot know.
    const RedeployFacts allGood{};
    EXPECT_NE(int{MirrorCanRedeploy(allGood)}, int{kTFRedeployCooldown});
    RedeployFacts allBad;
    allBad.validFactionAndRegion = false;
    allBad.alive = false;
    allBad.seated = true;
    allBad.spawnable = false;
    allBad.contested = true;
    allBad.extraReason = kTFRedeployBlocked;
    EXPECT_NE(int{MirrorCanRedeploy(allBad)}, int{kTFRedeployCooldown});
}
