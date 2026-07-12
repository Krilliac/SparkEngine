/**
 * @file TFServerValidation.h
 * @brief Server-authoritative anti-cheat sanity layer (W13 anti-cheat lane).
 *
 * The netcode is already server-authoritative end to end (TFServerSim owns the
 * movement tick, TFWeaponSystem::ValidateFire already gates fire rate with a
 * per-weapon-id token bucket). This module hardens the SEAMS around that:
 *
 *  - MOVEMENT: TFServerSim::TickMovement calls ValidateMovementTick() once per
 *    player per authoritative tick, comparing the tick's net displacement
 *    against the max a legitimate client could produce (class speed * dt *
 *    fudge) and clamping the position back in place on violation. This is a
 *    backstop against the input-queue "catch-up" path: TickMovement replays
 *    up to kMaxInputsPerTick queued inputs per real tick, each stepped with a
 *    full fixedDeltaTime -- a client that floods TF_ClientInput packets
 *    faster than the server drains them (> kMaxInputsPerTick * tickHz per
 *    second) can otherwise sustain up to kMaxInputsPerTick x the intended
 *    per-tick displacement indefinitely, since each replayed input's dt
 *    represents backlog being caught up, not real elapsed wall time. Vehicles
 *    (seated pawns never call this -- TickMovement's seated branch `continue`s
 *    before reaching it) and ability speed modifiers (Striker jets / AegisWall
 *    slow -- the caller passes the ability-modified cap) are naturally
 *    accounted for. Redeploy and the tf_tp admin teleport call
 *    NoteExemptTeleport() as explicit defense-in-depth documentation of the
 *    whitelist (in practice TeleportPawn() always lands strictly between two
 *    TickMovement iterations for that player, so the flag is rarely actually
 *    consumed, but it guards against a future refactor changing that).
 *  - FIRE RATE: TFWeaponSystem::ValidateFire (Game/TFWeaponServer.cpp) is the
 *    real per-weapon-id RoF token-bucket gate -- verified solid (burst
 *    tolerance 2, per-weapon-id bucket persistence, approx mag simulation)
 *    and left as-is. It mirrors its rejections here (RecordFireRateReject)
 *    purely for the unified tf_cheat_stats view; this module adds NO second
 *    rate gate of its own -- an independently-tuned second limiter would risk
 *    double-rejecting legitimate bursts the real bucket already tolerates.
 *  - POSITION SANITY: TFServerSim::HandleFireEvent calls CheckFireOrigin()
 *    before a fire event ever reaches TFWeaponSystem, comparing the client's
 *    claimed muzzle position (TF_FireEvent::origin*) against the server's own
 *    trusted pawn position (pawn feet + eye height) and dropping the packet
 *    outright on a gross divergence. ServerHandleFire already never TRUSTS
 *    the claimed origin for damage/hit resolution (it only falls back to it
 *    when the pawn registry reports the zeroed stub position -- this check
 *    special-cases that same condition so it never false-rejects the
 *    legitimate fallback), so this closes the seam where a client lying about
 *    its position would otherwise go completely unnoticed.
 *
 * All counters are DETECTION + CLAMP/REJECT only (no bans this wave -- see
 * tf_cheat_stats, Console/TFCommands.cpp). False-positive risk is kept low by
 * design: movement clamps gently pull the position back in place (never a
 * hard reject/kick) with a generous fudge factor, and the fire-origin reject
 * only fires on gross, no-legitimate-cause divergence.
 */
#pragma once

#include "Core/TFTypes.h"

#include <unordered_map>

namespace Terrafront
{

    /// Per-player violation counters (tf_cheat_stats).
    struct TFViolationStats
    {
        uint32_t movementClamps = 0;    ///< tick displacement exceeded the plausible cap and was pulled back
        uint32_t movementSpikes = 0;    ///< subset of movementClamps that were a gross (>= kSpikeRatio) overshoot
        uint32_t fireRateRejects = 0;   ///< TFWeaponSystem::ValidateFire rejections (mirrored counter)
        uint32_t fireOriginRejects = 0; ///< TF_FireEvent claimed origin diverged from the trusted pawn position
    };

    /// Process-singleton (same lifetime/access pattern as TFImpactFx::Get()).
    /// Server-authority-only in practice: only TFServerSim (IsAuthority()
    /// callers) touches it, but the class itself does not gate on authority --
    /// callers are responsible for that, same as every other server-side
    /// validation in this module.
    class TFServerValidation
    {
      public:
        static TFServerValidation& Get();

        /// Called once per player per authoritative tick, AFTER the movement
        /// model + collision resolve have produced this tick's final position,
        /// BEFORE it is written to the replicated Transform. Clamps `pos` back
        /// toward `prevPos` in place when the tick's displacement exceeds the
        /// plausible max (maxHorizSpeed * dt * fudge horizontally; a fixed
        /// generous constant vertically -- jumps/falls/jet-thrust are already
        /// bounded by the shared movement model, this is a backstop, not the
        /// primary limiter). `now` paces the throttled spike log.
        void ValidateMovementTick(PlayerId player, const float prevPos[3], float pos[3], float maxHorizSpeed,
                                  float dt, double now);

        /// Redeploy / tf_tp / any future explicit server-authoritative
        /// reposition: call immediately before writing the new position so the
        /// NEXT ValidateMovementTick() call for this player is skipped once.
        void NoteExemptTeleport(PlayerId player);

        /// TF_FireEvent claimed origin vs the server's trusted pawn position.
        /// Returns false (and counts the violation) when `claimed` diverges
        /// from `trusted` by more than `maxDivergenceM` -- caller should drop
        /// the packet without forwarding it to TFWeaponSystem.
        bool CheckFireOrigin(PlayerId player, const float claimed[3], const float trusted[3], float maxDivergenceM);

        /// Mirrors a TFWeaponSystem::ValidateFire rejection into the unified
        /// per-player counters (see file header: no second gate here).
        void RecordFireRateReject(PlayerId player);

        /// Session teardown hygiene: drop this player's counters so a recycled
        /// PlayerId doesn't inherit a stale violation history (same pattern as
        /// TFServerSim::CleanupPlayerSession's other ClearPlayer calls).
        void ClearPlayer(PlayerId player);

        const std::unordered_map<PlayerId, TFViolationStats>& Stats() const { return m_stats; }

      private:
        TFServerValidation() = default;

        std::unordered_map<PlayerId, TFViolationStats> m_stats;
        std::unordered_map<PlayerId, bool> m_exemptOnce;
        std::unordered_map<PlayerId, double> m_lastSpikeLog;
    };

} // namespace Terrafront
