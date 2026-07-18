/**
 * @file TFAbilityTypes.h
 * @brief Ability wire protocol + tuning split out of TFAbilitySystem.h
 *        (umbrella): the reserved TFMsg id constants, the TFAbilityPhase /
 *        TFAbilityKind enums, packed + static_asserted wire structs, effect
 *        tuning constants, the per-pawn move-mod struct and the shared jet
 *        thrust helper.
 *
 * OWNERSHIP: class-abilities lane (with TFAbilitySystem.h + the
 * TFAbilitySystem*.cpp split parts).
 *
 * @see TFAbilitySystem.h
 */
#pragma once

#include "Game/TFMovementModel.h"

#include <cstdint>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire protocol — reserved TFMsg id block 0x5458-0x545F (class-abilities
    // lane). TFNetProtocol.h (contended) only gains a block comment via the
    // wave wiringNotes; these constants keep this lane compiling standalone
    // and MUST stay value-identical to any future enum entries.
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgAbilityRequest = 0x5458; // C->S  TF_AbilityRequest
    constexpr uint16_t kTFMsgAbilityState = 0x5459;   // S->C  TF_AbilityState (self detail + visible-pawn broadcast)
    // 0x545A-0x545F reserved for future ability traffic.

    /// Ability lifecycle phase (server truth; mirrored to clients).
    enum class TFAbilityPhase : uint8_t
    {
        Ready = 0,
        Active = 1,
        Cooldown = 2,
    };

#pragma pack(push, 1)

    struct TF_AbilityRequest
    {
        uint8_t on; // 1 = activate / toggle-on, 0 = deactivate (toggle & jets)
        uint8_t _pad[3];
    };
    static_assert(sizeof(TF_AbilityRequest) == 4, "wire layout frozen");

    /// Phase-transition notification. For the owning player this is the HUD
    /// truth (remainingSec/fuel01 re-seed the local countdown); for everyone
    /// else only pawnEntity + abilityClass + phase matter (veil ghosting).
    struct TF_AbilityState
    {
        uint32_t pawnEntity;  // subject pawn (network entity id; 0 = none)
        uint32_t player;      // owning player
        uint8_t abilityClass; // ClassId of the ability's class
        uint8_t phase;        // TFAbilityPhase
        uint8_t _pad[2];
        float remainingSec; // Active: time left (0 = indefinite); Cooldown: cd left
        float fuel01;       // jets fuel fraction 0..1 (1 for non-fuel abilities)
    };
    static_assert(sizeof(TF_AbilityState) == 20, "wire layout frozen");

#pragma pack(pop)

    // ---------------------------------------------------------------------------
    // Effect tuning (desc-level numbers from classes.json ability text; the
    // structured fields — duration/cooldown/regen/toggle — stay data-driven).
    // ---------------------------------------------------------------------------

    constexpr float kTFSurgeRadiusM = 8.0f;          ///< Triage Surge heal radius
    constexpr float kTFSurgeHealPerSec = 30.0f;      ///< 150 HP over the 5 s duration
    constexpr float kTFAegisAbsorbHp = 450.0f;       ///< Bulwark Field absorb pool
    constexpr float kTFAegisSpeedMult = 0.8f;        ///< Bulwark Field -20% move speed
    constexpr float kTFLockdownRoFMult = 1.4f;       ///< Anchor Lockdown +40% RoF (weapons seam)
    constexpr float kTFJetMinActivateFuel01 = 0.15f; ///< min fuel fraction to ignite jets
    constexpr float kTFJetUpAccelMps2 = 12.0f;       ///< net climb accel beyond gravity cancel
    constexpr float kTFJetMaxRiseMps = 9.0f;         ///< vertical rise speed cap under thrust

    /// Per-pawn movement modifiers the ability system feeds into THE shared
    /// movement step. Applied by the mirrored TFServerSim::StepPlayer /
    /// TFClientNet::SimulateMove snippets (wave wiringNotes, both-or-neither).
    struct TFAbilityMoveMods
    {
        float speedMult = 1.0f; ///< run/sprint max-speed multiplier (0 = rooted, no jump)
        bool jetThrust = false; ///< apply TFApplyJetThrust after the move step
    };

    /// Striker jet thrust — MUST run identically on server and client (called
    /// from both mirrored snippets, after TFMoveStep + collision resolve, so
    /// gravity applied inside the step is cancelled and the pawn climbs).
    inline void TFApplyJetThrust(TFMoveState& s, float dt)
    {
        s.vel[1] += (kTFGravity + kTFJetUpAccelMps2) * dt;
        if (s.vel[1] > kTFJetMaxRiseMps)
            s.vel[1] = kTFJetMaxRiseMps;
        s.grounded = false;
    }

    /// Behavior implemented for a classes.json ability key.
    enum class TFAbilityKind : uint8_t
    {
        None = 0,     ///< class has no ability / data unloaded
        Veil,         ///< "veil"      Ghost
        Jets,         ///< "jets"      Striker
        Surge,        ///< "surge"     Medtech
        Forge,        ///< "forge"     Fabricator
        AegisWall,    ///< "aegiswall" Bulwark
        Lockdown,     ///< "lockdown"  Colossus
        GenericTimed, ///< unknown key: timed activate/cooldown, no effect
    };

    struct ClassAbilityDef; // Data/TFDataTables.h

} // namespace Terrafront
