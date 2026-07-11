/**
 * @file TestTFAbilityWire.cpp
 * @brief TERRAFRONT W9 class-abilities lane: frozen wire layout for the
 *        0x5458 block, phase vocabulary, movement-modifier defaults, and the
 *        shared jet-thrust math both sides of the prediction mirror run.
 *
 * Module sources are NOT compiled into SparkTests for this suite
 * (TestTFRedeployRules.cpp pattern): everything under test is header-only —
 * TFAbilitySystem.h wire PODs/constants and TFMovementModel.h's TFMoveStep.
 * The jet-thrust integration test matters because TFServerSim::StepPlayer and
 * TFClientNet::SimulateMove both call TFApplyJetThrust after the shared move
 * step; if its math stopped beating kTFGravity the ability would dead-stick
 * identically on both sides and no reconciliation error would ever flag it.
 */
#include "TestFramework.h"

#include "Game/TFAbilitySystem.h"
#include "Game/TFMovementModel.h"

#include <cstddef>
#include <cstdint>

using namespace Terrafront;

// ============================================================================
// Wire layout freeze (0x5458 block, class-abilities lane)
// ============================================================================

static_assert(kTFMsgAbilityRequest == 0x5458, "reserved id frozen");
static_assert(kTFMsgAbilityState == 0x5459, "reserved id frozen");

static_assert(sizeof(TF_AbilityRequest) == 4, "wire layout frozen");
static_assert(offsetof(TF_AbilityRequest, on) == 0, "wire layout frozen");

static_assert(sizeof(TF_AbilityState) == 20, "wire layout frozen");
static_assert(offsetof(TF_AbilityState, pawnEntity) == 0, "wire layout frozen");
static_assert(offsetof(TF_AbilityState, player) == 4, "wire layout frozen");
static_assert(offsetof(TF_AbilityState, abilityClass) == 8, "wire layout frozen");
static_assert(offsetof(TF_AbilityState, phase) == 9, "wire layout frozen");
static_assert(offsetof(TF_AbilityState, remainingSec) == 12, "wire layout frozen");
static_assert(offsetof(TF_AbilityState, fuel01) == 16, "wire layout frozen");

// Phase values ride the wire as uint8_t — renumbering breaks live clients.
static_assert(static_cast<uint8_t>(TFAbilityPhase::Ready) == 0, "wire vocabulary frozen");
static_assert(static_cast<uint8_t>(TFAbilityPhase::Active) == 1, "wire vocabulary frozen");
static_assert(static_cast<uint8_t>(TFAbilityPhase::Cooldown) == 2, "wire vocabulary frozen");

// ============================================================================
// Effect constants (desc-level numbers from classes.json ability text)
// ============================================================================

TEST(TFAbility_EffectConstantsMatchClassesJsonDescriptions)
{
    // Triage Surge: "150 HP over 5s, 8m" -> 30 hp/s for the 5 s duration.
    EXPECT_NEAR(kTFSurgeHealPerSec * 5.0f, 150.0f, 1e-4f);
    EXPECT_NEAR(kTFSurgeRadiusM, 8.0f, 1e-6f);
    // Bulwark Field: "+450 absorb while active, -20% move speed".
    EXPECT_NEAR(kTFAegisAbsorbHp, 450.0f, 1e-6f);
    EXPECT_NEAR(kTFAegisSpeedMult, 0.8f, 1e-6f);
    // Anchor Lockdown: "+40% RoF".
    EXPECT_NEAR(kTFLockdownRoFMult, 1.4f, 1e-6f);
    // Jets need a sane ignition floor strictly inside (0, 1).
    EXPECT_GT(kTFJetMinActivateFuel01, 0.0f);
    EXPECT_LT(kTFJetMinActivateFuel01, 1.0f);
}

TEST(TFAbility_MoveModsDefaultIsIdentity)
{
    const TFAbilityMoveMods mods;
    EXPECT_NEAR(mods.speedMult, 1.0f, 1e-6f);
    EXPECT_FALSE(mods.jetThrust);
}

// ============================================================================
// Jet thrust math (shared server/client prediction mirror)
// ============================================================================

TEST(TFAbility_JetThrustLiftsAndUncaps)
{
    TFMoveState s;
    s.grounded = true;
    const float dt = 1.0f / 60.0f;

    TFApplyJetThrust(s, dt);
    EXPECT_GT(s.vel[1], 0.0f); // net positive even though the step subtracted gravity
    EXPECT_FALSE(s.grounded);  // thrust always breaks ground contact
}

TEST(TFAbility_JetThrustRiseSpeedIsCapped)
{
    TFMoveState s;
    s.grounded = false;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 600; ++i) // 10 s of continuous thrust
        TFApplyJetThrust(s, dt);
    EXPECT_LE(s.vel[1], kTFJetMaxRiseMps + 1e-4f);
    EXPECT_GT(s.vel[1], 0.0f);
}

TEST(TFAbility_JetThrustBeatsGravityThroughTheSharedMoveStep)
{
    // One second of TFMoveStep (which applies kTFGravity internally) with the
    // thrust applied after each step — exactly the order of the mirrored
    // TFServerSim::StepPlayer / TFClientNet::SimulateMove wiring snippets.
    TFMoveState s;
    s.pos[1] = 0.0f;
    s.grounded = true;
    TFMoveInput in; // no move input, no jump: thrust alone must lift
    const float dt = 1.0f / 60.0f;
    auto flat = [](float, float) { return 0.0f; };

    for (int i = 0; i < 60; ++i)
    {
        TFMoveStep(s, in, 5.2f, 7.2f, dt, flat);
        TFApplyJetThrust(s, dt);
    }
    EXPECT_GT(s.pos[1], 1.0f); // well off the ground after 1 s
    EXPECT_FALSE(s.grounded);
}

TEST(TFAbility_RootedSpeedMultZeroesTheMoveStep)
{
    // Lockdown roots by multiplying both speed caps to 0 (and the wiring
    // snippet suppresses jump). Full forward input must go nowhere on flat
    // ground once friction eats the (clamped-to-zero) horizontal velocity.
    TFMoveState s;
    s.grounded = true;
    TFMoveInput in;
    in.moveY = 1.0f;
    in.sprint = true;
    const float dt = 1.0f / 60.0f;
    auto flat = [](float, float) { return 0.0f; };
    const float mult = 0.0f; // MoveModsForPawn speedMult while lockdown is active

    for (int i = 0; i < 60; ++i)
        TFMoveStep(s, in, 5.2f * mult, 7.2f * mult, dt, flat);

    const float h2 = s.pos[0] * s.pos[0] + s.pos[2] * s.pos[2];
    EXPECT_LT(h2, 0.01f); // < 10 cm of total drift over a full second
}
