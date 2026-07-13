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
 *  - INPUT RATE: TFServerSim::EnqueueInput calls AllowInput() before a
 *    TF_ClientInput packet is even pushed onto the per-player queue, charging
 *    it against a 60 Hz(+fudge) token bucket (same shape as
 *    TFWeaponSystem::ValidateFire's per-weapon-id RoF bucket, just keyed on
 *    the player instead of a weapon id). This is what actually closes the
 *    catch-up exploit the MOVEMENT bullet above describes: TickMovement's
 *    kMaxInputsPerTick replay is a bound on how much backlog is drained per
 *    tick, not on how fast the queue can be FILLED -- without this gate a
 *    client flooding TF_ClientInput packets faster than the server drains
 *    them keeps the queue permanently backlogged and sustains the
 *    kMaxInputsPerTick x displacement multiplier indefinitely (ValidateMovementTick's
 *    fudge factor is tuned to barely tolerate that catch-up path, not a
 *    sustained flood). Rejected inputs are simply dropped -- never queued --
 *    so the offending client just sees its own inputs stop being acked
 *    (server-auth reconciliation turns that into rubber-banding, never a
 *    desync). A legitimate client enqueues at most one input per its own
 *    frame and bots enqueue exactly once per fixed tick (TFBotSystem), so
 *    both stay comfortably under budget by construction.
 *  - KICK ESCALATION (W14): every counter above already survived its own
 *    deliberately-tolerant gate before it was counted (fudge factors, burst
 *    allowances, "gross divergence only"), so ViolationScore() sums them
 *    with per-counter weights (see the .cpp anonymous namespace) and
 *    ShouldKick() trips once the weighted total crosses kKickThreshold.
 *    movementClamps is deliberately EXCLUDED from the score -- only the
 *    movementSpikes subset (>= kSpikeRatio overshoot) counts, since mild
 *    clamps are expected occasionally even for legitimate clients (terrain,
 *    packet loss) and are already handled by the gentle pull-back alone.
 *    TFServerSim::EnforceAntiCheatKicks() polls ShouldKick() once per
 *    authoritative tick (after TickMovement() has returned, never from
 *    inside its per-player loop -- see that method's own comment for why),
 *    and for any player that trips it: calls NetworkManager::KickClient()
 *    then the same CleanupPlayerSession() teardown a real socket drop runs
 *    (which in turn calls ClearPlayer() below, so a recycled PlayerId never
 *    inherits a stale score and instantly re-trips the same kick).
 *
 * All counters below are DETECTION + CLAMP/REJECT as they're recorded; the
 * KICK ESCALATION bullet above is what finally acts on them (closing what
 * used to be a "no bans this wave" gap -- see tf_cheat_stats,
 * Console/TFCommands.cpp, for the live per-counter view an operator watches
 * before that threshold is reached). False-positive risk is kept low by
 * design: movement clamps gently pull the position back in place (never a
 * hard reject/kick) with a generous fudge factor, the fire-origin reject only
 * fires on gross, no-legitimate-cause divergence, and the kick threshold
 * itself requires accumulating several such violations (not a single one)
 * before it trips.
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
        uint32_t inputRateRejects = 0;  ///< TF_ClientInput dropped by AllowInput()'s 60Hz(+fudge) token bucket
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

        /// Call once per TF_ClientInput BEFORE it is pushed onto TFServerSim's
        /// per-player input queue (see file header: this is the seam that
        /// actually bounds sustained input flood rate, not just queue depth).
        /// Charges one token from a per-player 60Hz(+fudge) bucket; returns
        /// false (and counts the violation) when the bucket is empty --
        /// caller should drop the input without enqueueing it. `now` is
        /// TFServerSim::ServerTime(), same clock as ValidateMovementTick.
        bool AllowInput(PlayerId player, double now);

        /// Mirrors a TFWeaponSystem::ValidateFire rejection into the unified
        /// per-player counters (see file header: no second gate here).
        void RecordFireRateReject(PlayerId player);

        /// Weighted sum of `player`'s violation counters (see the .cpp
        /// anonymous namespace for the per-counter weights). movementClamps
        /// is NOT included -- only its movementSpikes subset is (file header:
        /// KICK ESCALATION). Zero for a player with no recorded violations.
        uint32_t ViolationScore(PlayerId player) const;

        /// True once ViolationScore(player) has crossed kKickThreshold (see
        /// .cpp). Pure query, no side effects -- callers (TFServerSim) own
        /// actually disconnecting the player and MUST follow up with
        /// ClearPlayer() (directly or via the existing CleanupPlayerSession
        /// teardown) so a recycled PlayerId doesn't inherit the score and
        /// instantly re-trip the same kick.
        bool ShouldKick(PlayerId player) const;

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

        // AllowInput's per-player token bucket (kept as parallel maps, same
        // shape as the rest of this class, rather than a nested struct).
        std::unordered_map<PlayerId, float> m_inputTokens;
        std::unordered_map<PlayerId, double> m_inputLastRefill;
        std::unordered_map<PlayerId, double> m_lastInputRejectLog;
    };

} // namespace Terrafront
