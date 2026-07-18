/**
 * @file TFWeaponServer.cpp
 * @brief Server half of TERRAFRONT weapons, fire-entry part: server-side
 *        loadout/unlock enforcement, fire validation (RoF token bucket +
 *        approx mag), and the validated-fire dispatch into hitscan pellets or
 *        server projectiles. Split parts: TFWeaponServerFx.cpp (0x54F4/0x54F5
 *        fx broadcasts), TFWeaponServerHitscan.cpp (lag-compensated hitscan),
 *        TFWeaponServerProjectile.cpp (projectile sim + splash); shared
 *        damage-kind constants in TFWeaponServerInternal.h. Client half in
 *        TFWeaponSystem.cpp.
 */
#include "Game/TFWeaponSystem.h"

#include "Game/TFAbilitySystem.h" // class-abilities lane (W9): Lockdown RoF bonus
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h" // W6 progression: per-weapon shot/hit stats
#include "Game/TFVehicleSystem.h"     // W3 shared-edit: vehicle hit tests + seat weapons
#include "Game/TFServerValidation.h"  // W13 anti-cheat lane: mirror ValidateFire rejections into tf_cheat_stats
#include "Game/TFWeaponMath.h"
#include "Game/TFWeaponServerInternal.h"
#include "Net/TFServerSim.h"

#include <algorithm>

namespace Terrafront
{

    using namespace WeaponServerDetail;

    namespace
    {
        constexpr float kMaxHitscanRangeM = 400.0f;
        constexpr double kFixedRttSec = 0.100; // TF-W2: per-client RTT from NetworkManager
    } // namespace

    double TFWeaponSystem::ServerNow() const
    {
        return (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_serverClock;
    }

    // SECURITY: mirrors the client's RefreshLocalLoadout() slot resolution
    // (primary/secondary/tool/melee) but is evaluated server-side against the
    // server-trusted pawn.cls/pawn.faction, so a client cannot claim a weapon
    // outside what its class is actually issued. Checks every entry in
    // primarySlots (not just front()) so a future multi-primary class stays
    // correct without another server-side change.
    bool TFWeaponSystem::IsWeaponInLoadout(WeaponId fireWeapon, const PawnInfo& pawn) const
    {
        if (!m_ctx || !m_ctx->data)
            return false;

        const ClassDef* cls = m_ctx->data->GetClass(pawn.cls);
        if (!cls)
            return false;

        for (const std::string& slotKey : cls->primarySlots)
        {
            if (FindWeaponForSlotKey(slotKey, pawn.faction) == fireWeapon)
                return true;
        }
        if (FindWeaponForSlotKey(cls->secondarySlot, pawn.faction) == fireWeapon)
            return true;
        if (FindToolWeapon(cls->toolKey) == fireWeapon)
            return true;
        if (FindWeaponForSlotKey("melee", pawn.faction) == fireWeapon)
            return true;
        return false;
    }

    // ---------------------------------------------------------------------------
    // Frozen API: server fire entry
    // ---------------------------------------------------------------------------

    void TFWeaponSystem::ServerHandleFire(PlayerId shooter, const TF_FireEvent& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players || !m_ctx->data ||
            !m_ctx->data->IsLoaded())
            return;

        PawnInfo pawn;
        if (!m_ctx->players->GetPawnByPlayer(shooter, pawn) || !pawn.alive)
            return;

        // W3 shared-edit (vehicles agent): a seated shooter fires the SEAT weapon
        // through this exact validation path — the riding pawn is the shooter
        // surrogate (its transform already sits on the vehicle, so the trusted
        // origin, lag comp and hit confirms all work unchanged). Unarmed seats
        // (passengers, drivers of unarmed vehicles) cannot fire at all.
        WeaponId fireWeapon = ev.weaponId;
        bool viaVehicleSeat = false;
        if (m_ctx->vehicles)
        {
            WeaponId seatWeapon = kInvalidWeapon;
            if (m_ctx->vehicles->GetSeatWeapon(shooter, seatWeapon))
            {
                if (seatWeapon == kInvalidWeapon)
                    return; // seated in an unarmed seat
                fireWeapon = seatWeapon;
                viaVehicleSeat = true;
            }
        }

        // SECURITY: an unseated shooter must be firing a weapon that's actually
        // part of their class loadout. Without this, a modified client can put
        // any WeaponId in TF_FireEvent (another class's weapon, a dev/OP-only
        // weapon, an out-of-range id) and have the server fire it with full
        // trust. Seated shooters are exempt: fireWeapon there is the
        // server-resolved seat weapon (GetSeatWeapon), not client input.
        if (!viaVehicleSeat && !IsWeaponInLoadout(fireWeapon, pawn))
            return;

        // W8 unlock enforcement (the deliberate non-wiring from W6): a
        // loadout-ELIGIBLE weapon must also be UNLOCKED for this player
        // (Persistence/TFUnlockTree.h via TFProgressionSystem). Weapons with no
        // tree entry are default kit and always pass -- IsWeaponUnlocked
        // returns true for them, and fails open while data tables are not
        // loaded (verified against TFUnlockTree::All(): only carbines, lmgs,
        // snipers, np_shotgun and np_launcher are gated). One deliberate
        // exemption: the class-DEFAULT primary (primarySlots.front()). Ghost's
        // default equip is a tree-gated sniper and Striker/Fabricator's is the
        // rank-3 carbine; RefreshLocalLoadout hands those to every fresh spawn
        // regardless of rank, so rejecting them would brick rank-1 players of
        // those classes. Every OTHER slot-pool weapon (e.g. np_shotgun on
        // Striker, the carbine in Ghost's second primary slot) is enforced.
        // Seated shooters stay exempt like the loadout check above: their
        // fireWeapon is the server-resolved seat weapon, not client input.
        if (!viaVehicleSeat && m_ctx->progression && !m_ctx->progression->IsWeaponUnlocked(shooter, fireWeapon))
        {
            const ClassDef* cls = m_ctx->data->GetClass(pawn.cls);
            const bool defaultPrimary = cls && !cls->primarySlots.empty() &&
                                        FindWeaponForSlotKey(cls->primarySlots.front(), pawn.faction) == fireWeapon;
            if (!defaultPrimary)
            {
                ++m_shotsRejected;
                return;
            }
        }

        const WeaponDef* base = m_ctx->data->GetWeapon(fireWeapon);
        if (!base || base->kind == "melee" || base->kind == "beam")
            return; // TF-W2: melee reach + tool beams take a different server path

        WeaponDef def = m_ctx->data->ResolveWeapon(fireWeapon, pawn.faction);
        // class-abilities lane (W9): Anchor Lockdown RoF bonus — MUST land
        // together with the client fire-interval edit (both-or-neither).
        if (m_ctx->abilities)
            def.rofRpm *= m_ctx->abilities->RoFMultiplierForPawn(pawn.entity);

        ShooterState& st = m_shooters[shooter];
        const double now = ServerNow();
        if (!ValidateFire(def, st, now))
        {
            ++m_shotsRejected;
            // W13 anti-cheat lane: mirror into the unified per-player counters
            // (tf_cheat_stats) — no second gate, this ValidateFire call above
            // is still the sole fire-rate authority (see TFServerValidation.h).
            TFServerValidation::Get().RecordFireRateReject(shooter);
            return;
        }
        ++m_shotsValidated;

        // W8 audio-polish: a validated fire event is the local client's only
        // in-process knowledge of OTHER shooters (bots + remote players on a
        // listen host), so the distant-gunfire layer hooks here. No-op on
        // dedicated servers and for the local shooter (checked inside).
        ClientOnRemoteFire(shooter, pawn, def);

        // W6 progression: one validated trigger pull == one shot (pellets are
        // still one shot; hits below are capped at one per fire event to match).
        if (m_ctx->progression)
            m_ctx->progression->ServerRecordShots(shooter, def.id, 1);

        // Server-trusted origin: the pawn's eye. Client dir is used but normalized.
        float origin[3] = {pawn.pos[0], pawn.pos[1] + 1.65f, pawn.pos[2]};
        // Registry-stub pawns report zeros; fall back to the client origin then.
        if (pawn.pos[0] == 0.0f && pawn.pos[1] == 0.0f && pawn.pos[2] == 0.0f)
        {
            origin[0] = ev.originX;
            origin[1] = ev.originY;
            origin[2] = ev.originZ;
        }
        float dir[3] = {ev.dirX, ev.dirY, ev.dirZ};
        if (!WeaponMath::Normalize3(dir))
            return;

        // W8 shared-edit (turret-aim/vehicles agent): the turret-controller seat
        // fires from the AIMED turret muzzle frame (server aim state, unit dir)
        // instead of the riding pawn's eye, so shots match the replicated turret
        // visual. Non-controller armed seats keep the pawn-eye path above.
        if (viaVehicleSeat)
            m_ctx->vehicles->GetSeatFireFrame(shooter, origin, dir);

        // W9 remote-fire-events: PURE clients have no in-process view of this
        // validated fire (ClientOnRemoteFire above only covers the listen
        // host), so broadcast a per-shot fx message from the FINAL fire frame
        // (post seat/turret resolution) to clients in range. Both hitscan and
        // projectile paths pass through here.
        ServerBroadcastRemoteFireFx(st, shooter, pawn.entity, def.id, origin, dir);

        if (def.projSpeed > 0.0f)
        {
            SpawnServerProjectile(shooter, pawn, def, origin, dir);
            return;
        }

        const double rewindTime = now - kFixedRttSec * 0.5;
        const int pellets = std::max(1, def.pellets);
        bool anyPelletHit = false;
        for (int i = 0; i < pellets; ++i)
        {
            float pdir[3] = {dir[0], dir[1], dir[2]};
            if (pellets > 1 || def.spreadHipDeg > 0.0f)
                WeaponMath::PerturbCone(pdir, def.spreadHipDeg * 0.5f, m_rng);
            if (FireHitscanRay(shooter, pawn, def, origin, pdir, kMaxHitscanRangeM, rewindTime, kDamageKindBullet))
                anyPelletHit = true;
        }
        // W6 progression: at most one hit per fire event (multi-pellet shotguns
        // count as a single hit so accuracy never exceeds 100%).
        if (anyPelletHit && m_ctx->progression)
            m_ctx->progression->ServerRecordHits(shooter, def.id, 1);
    }

    // ---------------------------------------------------------------------------
    // Validation
    // ---------------------------------------------------------------------------

    bool TFWeaponSystem::ValidateFire(const WeaponDef& def, ShooterState& st, double now)
    {
        const double perSec = std::max(1.0, static_cast<double>(def.rofRpm) / 60.0);

        // SECURITY: the RoF token bucket + approx mag live PER WEAPON ID here
        // (try_emplace only seeds a fresh bucket the FIRST time this shooter is
        // ever seen firing this weapon id). Previously this state was a single
        // slot keyed off "last weapon fired", reset to a full 2.0 tokens every
        // time the weapon id changed -- alternating between two weapon ids each
        // shot refilled the bucket every shot for unlimited fire rate. Persisting
        // per weapon id closes that: switching back to a weapon reuses its own
        // gradually-refilled bucket, never a fresh one.
        auto [it, inserted] = st.perWeapon.try_emplace(def.id);
        WeaponFireState& ws = it->second;
        if (inserted)
        {
            ws.lastRefill = now;
            ws.mag = def.magSize;
        }

        ws.tokens = std::min(2.0f, ws.tokens + static_cast<float>((now - ws.lastRefill) * perSec));
        ws.lastRefill = now;

        if (ws.tokens < 1.0f)
            return false;
        ws.tokens -= 1.0f;

        // Approximate server mag: refills after reloadSec of silence.
        if (ws.mag <= 0)
        {
            if (now - ws.magEmptyTime < def.reloadSec)
                return false;
            ws.mag = def.magSize;
        }
        if (--ws.mag <= 0)
            ws.magEmptyTime = now;

        ws.lastShotTime = now;
        return true;
    }

} // namespace Terrafront
