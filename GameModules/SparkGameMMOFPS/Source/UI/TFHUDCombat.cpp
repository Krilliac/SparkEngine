/**
 * @file TFHUDCombat.cpp
 * @brief TFHUD combat-half state (W6): killfeed feed-in with headshot marker
 *        and EvPlayerKilled pairing, death panel capture (killer/weapon/
 *        distance), world-anchored damage-ping bookkeeping with the
 *        octant-flash dedup, and the entity/player position lookups.
 *
 * Split from TFHUD.cpp per the repo file-size rule (TFPlayerSystemClient.cpp
 * pattern — same class, feature-owned translation units). TFHUD.cpp keeps the
 * lifecycle, feed-in setters and pawn-state gather; the overlay drawing
 * (vitals/weapon/crosshair/compass) lives in TFHUDDraw.cpp, the chat window
 * in TFHUDChat.cpp, and the combat-half drawing (killfeed rows, damage
 * octants/pings, respawn overlay, death actions, minimap delegate) in
 * TFHUDCombatDraw.cpp.
 *
 * Data-visibility notes (what a pure client actually knows):
 *  - Pawns: replicated for every player (TFReplication RemotePawn store ->
 *    TFPlayerSystem client records), so ForEachAlivePawn/GetPawnBy* serve
 *    owner/faction/pos on all roles. Enemy pawns are deliberately NOT drawn
 *    on the minimap (friendly/squad intel only).
 *  - Deployables: TFDeployableSystem's client mirror (0x54FC-0x54FE).
 *  - Vehicles: TFVehicleSystem serves the same authoritative/mirror split
 *    (m_ctx->vehicles is already published — no wiring needed).
 *  - Squad membership: TFSquadSystem::SquadOf only resolves the LOCAL player
 *    on pure clients (the roster mirror is private), so squadmates render as
 *    plain friendlies there; on authority roles the full registry resolves.
 *    A public roster accessor is requested in the wave wiring notes.
 */
#include "UI/TFHUD.h"

#include "Data/TFDataTables.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFOutfitSystem.h" // Outfits lane: death-panel killer tag
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Net/TFClientNet.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace Terrafront
{

    namespace
    {

        constexpr float kKillfeedTTL = 8.0f; // seconds, fade over last 2
        constexpr size_t kKillfeedMax = 6;
        constexpr float kPendingKillTTL = 0.30f;    // EvPlayerKilled <-> PushKillfeed pairing window
        constexpr float kOctantSuppressSec = 0.05f; // world ping already covered this hit
        constexpr float kPingMergeDot = 0.94f;      // ~20 deg: refresh instead of new ping

        /// Local mirror of the TFClientNetHandlers PlayerLabel convention — keeps the
        /// death panel consistent with the names TFClientNet feeds the killfeed.
        void HudPlayerLabel(PlayerId id, PlayerId localId, char out[16])
        {
            if (id == kInvalidPlayer)
                std::snprintf(out, 16, "-");
            else if (id == localId)
                std::snprintf(out, 16, "YOU");
            else if (id == kTFLocalHostPlayer)
                std::snprintf(out, 16, "HOST");
            else
                std::snprintf(out, 16, "p%u", id);
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Killfeed feed-in (frozen 5-arg surface + W6 extended overload)
    // ---------------------------------------------------------------------------

    void TFHUD::PushKillfeed(const char* killer, const char* weapon, const char* victim, FactionId killerF,
                             FactionId victimF)
    {
        // Legacy call sites (TFClientNet) carry no ids/headshot; the extended
        // overload pairs the entry with a pending EvPlayerKilled when possible.
        PushKillfeed(killer, weapon, victim, killerF, victimF, false, kInvalidPlayer, kInvalidPlayer);
    }

    void TFHUD::PushKillfeed(const char* killer, const char* weapon, const char* victim, FactionId killerF,
                             FactionId victimF, bool headshot, PlayerId killerPlayer, PlayerId victimPlayer)
    {
        const PlayerId local = m_ctx ? m_ctx->localPlayer : kInvalidPlayer;

        KillfeedEntry e;
        e.killer = killer ? killer : "?";
        e.weapon = weapon ? weapon : "?";
        e.victim = victim ? victim : "?";
        e.killerF = killerF;
        e.victimF = victimF;
        e.headshot = headshot;
        e.annotated = (killerPlayer != kInvalidPlayer || victimPlayer != kInvalidPlayer);
        e.ttl = kKillfeedTTL;

        e.involvesLocal = (local != kInvalidPlayer && (killerPlayer == local || victimPlayer == local));
        // Name-convention fallback for un-upgraded 5-arg call sites (the labels
        // TFClientNet builds render the local player as "YOU").
        if (!e.involvesLocal)
            e.involvesLocal = (e.killer == "YOU" || e.victim == "YOU");

        // EvPlayerKilled fired BEFORE this push (bus subscriber order is not
        // guaranteed on authority roles): adopt the stashed details.
        if (!e.annotated && m_pendingKill.valid && m_pendingKill.ttl > 0.0f)
        {
            e.headshot = m_pendingKill.headshot;
            e.annotated = true;
            if (local != kInvalidPlayer && (m_pendingKill.killer == local || m_pendingKill.victim == local))
                e.involvesLocal = true;
            if (victimPlayer == kInvalidPlayer)
                victimPlayer = m_pendingKill.victim;
            if (killerPlayer == kInvalidPlayer)
                killerPlayer = m_pendingKill.killer;
            m_pendingKill = PendingKill{};
        }

        // Death panel capture for pure clients (no EvPlayerKilled bus there):
        // ids when the call site was upgraded, "YOU" label as the fallback.
        const bool localDied = (local != kInvalidPlayer && victimPlayer == local) || e.victim == "YOU";
        if (localDied)
        {
            m_death.valid = true;
            m_death.headshot = e.headshot;
            m_death.killerName = e.killer;
            m_death.weaponName = e.weapon;
            m_death.distanceM = -1.0f;
            float kp[3];
            if (killerPlayer != kInvalidPlayer && ResolvePlayerPosition(killerPlayer, kp))
            {
                const float dx = kp[0] - m_lastAlivePos[0];
                const float dy = kp[1] - m_lastAlivePos[1];
                const float dz = kp[2] - m_lastAlivePos[2];
                m_death.distanceM = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }

        m_killfeed.push_front(std::move(e));
        while (m_killfeed.size() > kKillfeedMax)
            m_killfeed.pop_back();
    }

    // ---------------------------------------------------------------------------
    // Bus handlers (authority roles; pure clients never see these events fire
    // with full ids — see the pure-client capture path in PushKillfeed above)
    // ---------------------------------------------------------------------------

    void TFHUD::OnPlayerKilledBus(const EvPlayerKilled& ev)
    {
        const PlayerId local = m_ctx ? m_ctx->localPlayer : kInvalidPlayer;

        if (local != kInvalidPlayer && ev.victim == local)
            RecordDeath(ev.killer, ev.weapon, ev.headshot);

        // Annotate the matching killfeed entry if PushKillfeed already ran for
        // this kill; otherwise stash the details for the imminent push.
        for (KillfeedEntry& e : m_killfeed)
        {
            if (e.annotated || e.ttl < kKillfeedTTL - kPendingKillTTL)
                continue; // older entry — belongs to a previous kill
            e.headshot = ev.headshot;
            e.annotated = true;
            if (local != kInvalidPlayer && (ev.killer == local || ev.victim == local))
                e.involvesLocal = true;
            return;
        }
        m_pendingKill.valid = true;
        m_pendingKill.headshot = ev.headshot;
        m_pendingKill.killer = ev.killer;
        m_pendingKill.victim = ev.victim;
        m_pendingKill.ttl = kPendingKillTTL;
    }

    void TFHUD::OnPlayerDamagedBus(const EvPlayerDamaged& ev)
    {
        if (!m_ctx || !m_ctx->players)
            return;
        PlayerId local = m_ctx->localPlayer;
        if (local == kInvalidPlayer && m_ctx->clientNet)
            local = m_ctx->clientNet->LocalPlayerId();
        if (local == kInvalidPlayer)
            return;

        PawnInfo me{};
        if (!m_ctx->players->GetPawnByPlayer(local, me))
            return;
        if (ev.victim != me.entity || ev.attacker == 0 || ev.attacker == me.entity)
            return; // not our hit, environmental, or self-inflicted

        float attackerPos[3];
        if (ResolveEntityPosition(ev.attacker, attackerPos))
            AddWorldPing(attackerPos, me.pos);
        // Unresolvable attacker (already despawned): the octant flash from
        // ShowDamageFrom stays as the fallback indicator.
    }

    void TFHUD::RecordDeath(PlayerId killer, WeaponId weapon, bool headshot)
    {
        const PlayerId local = m_ctx ? m_ctx->localPlayer : kInvalidPlayer;

        m_death.valid = true;
        m_death.headshot = headshot;

        char name[16];
        HudPlayerLabel(killer, local, name);
        // Outfits lane: prepend the killer's outfit tag when known.
        char tagged[26];
        OutfitTaggedLabel(m_ctx && m_ctx->outfits ? m_ctx->outfits->GetOutfitTag(killer) : "", name, tagged,
                          sizeof(tagged));
        m_death.killerName = tagged;

        m_death.weaponName = "-";
        if (m_ctx && m_ctx->data && m_ctx->data->IsLoaded())
        {
            if (const WeaponDef* wd = m_ctx->data->GetWeapon(weapon))
                if (!wd->name.empty())
                    m_death.weaponName = wd->name;
        }

        m_death.distanceM = -1.0f;
        float kp[3];
        if (ResolvePlayerPosition(killer, kp))
        {
            const float dx = kp[0] - m_lastAlivePos[0];
            const float dy = kp[1] - m_lastAlivePos[1];
            const float dz = kp[2] - m_lastAlivePos[2];
            m_death.distanceM = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    // ---------------------------------------------------------------------------
    // World-anchored damage pings
    // ---------------------------------------------------------------------------

    void TFHUD::AddWorldPing(const float attackerPos[3], const float myPos[3])
    {
        float dx = attackerPos[0] - myPos[0];
        float dz = attackerPos[2] - myPos[2];
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.5f)
            return; // point blank / same spot: direction is meaningless
        dx /= len;
        dz /= len;

        // Same-hit dedup vs the octant flash: ShowDamageFrom may run before or
        // after this handler (synchronous bus, unspecified subscriber order).
        m_octantSuppress = kOctantSuppressSec; // flash arriving after us
        if (m_lastOctant >= 0 && m_lastOctantAge <= kOctantSuppressSec)
        {
            m_octant[m_lastOctant] = 0.0f; // flash arrived before us
            m_lastOctant = -1;
        }

        // Sustained fire from one direction refreshes instead of stacking.
        for (DamagePing& p : m_pings)
        {
            if (p.intensity > 0.0f && p.dirX * dx + p.dirZ * dz >= kPingMergeDot)
            {
                p.dirX = dx;
                p.dirZ = dz;
                p.intensity = 1.0f;
                return;
            }
        }
        DamagePing* slot = &m_pings[0];
        for (DamagePing& p : m_pings)
            if (p.intensity < slot->intensity)
                slot = &p;
        slot->dirX = dx;
        slot->dirZ = dz;
        slot->intensity = 1.0f;
    }

    // ---------------------------------------------------------------------------
    // Position lookups (net entity / player id -> world position)
    // ---------------------------------------------------------------------------

    bool TFHUD::ResolvePlayerPosition(PlayerId player, float out[3]) const
    {
        if (!m_ctx || !m_ctx->players || player == kInvalidPlayer)
            return false;
        PawnInfo p{};
        if (!m_ctx->players->GetPawnByPlayer(player, p))
            return false;
        out[0] = p.pos[0];
        out[1] = p.pos[1];
        out[2] = p.pos[2];
        return true;
    }

    bool TFHUD::ResolveEntityPosition(EntityId entity, float out[3]) const
    {
        if (!m_ctx || entity == 0)
            return false;
        if (m_ctx->players)
        {
            PawnInfo p{};
            if (m_ctx->players->GetPawnByEntity(entity, p))
            {
                out[0] = p.pos[0];
                out[1] = p.pos[1];
                out[2] = p.pos[2];
                return true;
            }
        }
        if (m_ctx->deployables)
        {
            TFDeployableView d{};
            if (m_ctx->deployables->GetDeployable(entity, d))
            {
                out[0] = d.pos[0];
                out[1] = d.pos[1];
                out[2] = d.pos[2];
                return true;
            }
        }
        if (m_ctx->vehicles)
        {
            TFVehicleInfo v{};
            if (m_ctx->vehicles->GetVehicleInfo(entity, v))
            {
                out[0] = v.pos[0];
                out[1] = v.pos[1];
                out[2] = v.pos[2];
                return true;
            }
        }
        return false;
    }

} // namespace Terrafront
