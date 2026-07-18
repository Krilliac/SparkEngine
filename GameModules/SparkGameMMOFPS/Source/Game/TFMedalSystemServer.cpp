/**
 * @file TFMedalSystemServer.cpp
 * @brief TFMedalSystem server half: medal detection from the bus events
 *        (killstreak/multikill/Savior/Avenger + capture participation), the
 *        Award -> ServerAwardXP payout, dirty-row broadcast, and the
 *        NetworkManager wire-out with the late-joiner/leaver GetClients()
 *        diff poll. Split from TFMedalSystem.cpp.
 */
#include "Game/TFMedalSystem.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFSquadSystem.h"

#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstring>

namespace Terrafront
{

    namespace
    {

        // Killstreak thresholds -> medal (order matters: checked on exact hit).
        constexpr uint32_t kStreakRampage = 5;
        constexpr uint32_t kStreakDominating = 10;
        constexpr uint32_t kStreakUnstoppable = 15;

        constexpr size_t kMaxDamagersPerVictim = 8; ///< recent-attacker ring cap

    } // namespace

    // ---------------------------------------------------------------------------
    // Server: event handlers
    // ---------------------------------------------------------------------------

    void TFMedalSystem::OnPlayerKilled(const EvPlayerKilled& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;

        const double now = NowSec();
        FactionId killerF = FactionId::None;
        FactionId victimF = FactionId::None;
        if (m_ctx->players)
        {
            killerF = m_ctx->players->FactionOf(ev.killer);
            victimF = m_ctx->players->FactionOf(ev.victim);
        }

        // Victim side: death tally, streak/chain reset, Avenger bookkeeping.
        if (ev.victim != kInvalidPlayer)
        {
            ServerRec& vr = Ensure(ev.victim);
            ++vr.row.deaths;
            vr.streak = 0;
            vr.chain = 0;
            vr.dirty = true;
            m_recentDamage.erase(ev.victim); // their attackers are moot now
            if (ev.killer != kInvalidPlayer && ev.killer != ev.victim &&
                (killerF == FactionId::None || killerF != victimF))
                m_lastKiller[ev.victim] = LastKiller{ev.killer, now};
        }

        // Killer credit: same filter as TFProgressionSystem/TFScoreboard —
        // no credit for environment, suicide, or team kills.
        if (ev.killer == kInvalidPlayer || ev.killer == ev.victim)
            return;
        if (killerF != FactionId::None && killerF == victimF)
            return;

        ServerRec& kr = Ensure(ev.killer);
        ++kr.row.kills;
        ++kr.streak;
        kr.chain = (now - kr.lastKillAt) <= kTFMultikillWindowSec ? kr.chain + 1 : 1;
        kr.lastKillAt = now;

        // Multikills (exactly on the threshold; a 5th chained kill stays Quad).
        if (kr.chain == 2)
            Award(ev.killer, kr, TFMedalId::DoubleKill);
        else if (kr.chain == 3)
            Award(ev.killer, kr, TFMedalId::TripleKill);
        else if (kr.chain == 4)
            Award(ev.killer, kr, TFMedalId::QuadKill);

        // Killstreaks.
        if (kr.streak == kStreakRampage)
            Award(ev.killer, kr, TFMedalId::Rampage);
        else if (kr.streak == kStreakDominating)
            Award(ev.killer, kr, TFMedalId::Dominating);
        else if (kr.streak == kStreakUnstoppable)
            Award(ev.killer, kr, TFMedalId::Unstoppable);

        CheckAvenger(ev.killer, ev.victim, kr, now);
        CheckSavior(ev.killer, ev.victim, kr, now);

        RecomputeScore(kr);
        kr.dirty = true;
    }

    void TFMedalSystem::CheckAvenger(PlayerId killer, PlayerId victim, ServerRec& kr, double now)
    {
        auto it = m_lastKiller.find(killer);
        if (it == m_lastKiller.end())
            return;
        if (it->second.killer == victim && (now - it->second.at) <= kTFAvengerWindowSec)
        {
            m_lastKiller.erase(it); // one revenge per death
            Award(killer, kr, TFMedalId::Avenger);
        }
    }

    void TFMedalSystem::CheckSavior(PlayerId killer, PlayerId victim, ServerRec& kr, double now)
    {
        if (!m_ctx || !m_ctx->squads)
            return;
        const SquadId sq = m_ctx->squads->SquadOf(killer);
        if (sq == kInvalidSquad)
            return;

        // Did the fallen enemy recently damage one of the killer's squadmates?
        for (const auto& [damagedPlayer, damagers] : m_recentDamage)
        {
            if (damagedPlayer == killer)
                continue; // damaging ME is revenge territory, not a save
            if (m_ctx->squads->SquadOf(damagedPlayer) != sq)
                continue;
            for (const Damager& d : damagers)
            {
                if (d.attacker == victim && (now - d.at) <= kTFSaviorWindowSec)
                {
                    Award(killer, kr, TFMedalId::Savior);
                    return; // one Savior per kill, even for multiple squadmates
                }
            }
        }
    }

    void TFMedalSystem::OnPlayerDamaged(const EvPlayerDamaged& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players)
            return;
        if (ev.attacker == 0 || ev.victim == ev.attacker)
            return; // environment / self

        PawnInfo victimPi{}, attackerPi{};
        if (!m_ctx->players->GetPawnByEntity(ev.victim, victimPi) ||
            !m_ctx->players->GetPawnByEntity(ev.attacker, attackerPi))
            return;
        if (victimPi.owner == kInvalidPlayer || attackerPi.owner == kInvalidPlayer ||
            victimPi.owner == attackerPi.owner)
            return;
        if (victimPi.faction != FactionId::None && victimPi.faction == attackerPi.faction)
            return; // friendly fire never seeds a Savior

        const double now = NowSec();
        std::vector<Damager>& ring = m_recentDamage[victimPi.owner];
        std::erase_if(ring, [now](const Damager& d) { return (now - d.at) > kTFSaviorWindowSec; });
        // Refresh an existing entry for this attacker instead of duplicating.
        for (Damager& d : ring)
        {
            if (d.attacker == attackerPi.owner)
            {
                d.at = now;
                return;
            }
        }
        if (ring.size() >= kMaxDamagersPerVictim)
            ring.erase(ring.begin());
        ring.push_back(Damager{attackerPi.owner, now});
    }

    void TFMedalSystem::OnXPAwarded(const EvXPAwarded& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        // Capture participation only — the canonical capture reason codes
        // (TFDirectiveSystem precedent). Everything else — including our own
        // kXPReasonMedal payouts, which re-enter here synchronously via
        // ServerAwardXP -> EvXPAwarded — is ignored. Keep this filter FIRST.
        if (ev.reason != kXPReasonCaptureFacility && ev.reason != kXPReasonCaptureFort &&
            ev.reason != kXPReasonCaptureOutpost)
            return;
        ServerRec& rec = Ensure(ev.player);
        ++rec.row.captures;
        RecomputeScore(rec);
        rec.dirty = true;
    }

    // ---------------------------------------------------------------------------
    // Server: award + wire out
    // ---------------------------------------------------------------------------

    void TFMedalSystem::Award(PlayerId player, ServerRec& rec, TFMedalId medal)
    {
        const size_t idx = static_cast<size_t>(medal);
        if (rec.medalCounts[idx] < 0xFFFFu)
            ++rec.medalCounts[idx];
        ++rec.row.medals;
        ++m_medalsAwarded;

        const TFMedalDef& def = TFMedalDefOf(static_cast<uint8_t>(medal));
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] medal '%s' -> player %u (x%u)", def.key, player,
                       rec.medalCounts[idx]);

        // XP through the real progression hook (directives precedent) — this
        // also pushes the standard TF_XPEvent to the owning client.
        if (m_ctx && m_ctx->progression && def.xp > 0)
            m_ctx->progression->ServerAwardXP(player, def.xp, kXPReasonMedal);

        TF_MedalAward award{};
        award.player = player;
        award.medal = static_cast<uint8_t>(medal);
        award.sessionCount = rec.medalCounts[idx];
        SendMedalToOwner(player, award);
    }

    void TFMedalSystem::SendMedalToOwner(PlayerId player, const TF_MedalAward& award)
    {
        // Listen host / standalone: the local player is not a network client —
        // feed the toast queue directly (TFSquadSystem::SendEchoTo pattern).
        if (m_ctx && m_ctx->HasLocalPlayer() && player == m_ctx->localPlayer)
        {
            ClientHandleMedal(award);
            return;
        }
#ifdef ENABLE_NETWORKING
        SendWire(player, kTFMsgMedalAward, &award, sizeof(award));
#endif
    }

    void TFMedalSystem::FlushDirtyRows()
    {
        for (auto& [player, rec] : m_server)
        {
            if (!rec.dirty)
                continue;
            rec.dirty = false;
            RefreshIdentity(player, rec);
            BroadcastRow(player, rec);
        }
    }

    void TFMedalSystem::BroadcastRow(PlayerId player, const ServerRec& rec)
    {
        TF_ScoreUpdate st{};
        st.player = player;
        st.score = rec.row.score;
        st.kills = rec.row.kills;
        st.deaths = rec.row.deaths;
        st.captures = rec.row.captures;
        st.medals = rec.row.medals;
        st.faction = static_cast<uint8_t>(rec.row.faction);
        st.cls = static_cast<uint8_t>(rec.row.cls);
        ++m_rowsSent;

#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server)
        {
            for (const auto& [id, info] : nm.GetClients())
            {
                if (info.state == Spark::Net::ConnectionState::Connected)
                    SendWire(id, kTFMsgScoreUpdate, &st, sizeof(st));
            }
        }
#endif
        // The authority's own scoreboard reads m_server directly through
        // GetScoreRow/ForEachScoreRow — no local mirror write needed.
    }

#ifdef ENABLE_NETWORKING

    void TFMedalSystem::SendWire(PlayerId target, uint16_t msgId, const void* payload, size_t size)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(size);
        std::memcpy(msg.payload.data(), payload, size);
        nm.SendToClient(target, msg);
    }

    void TFMedalSystem::PollJoinsLeaves()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;

        // Late-joiner burst: a fresh client gets every known row once
        // (TFAbilitySystem / TFDeployableSystem GetClients() diff-poll pattern).
        for (const auto& [id, info] : nm.GetClients())
        {
            if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                continue;
            m_knownClients.insert(id);
            for (auto& [player, rec] : m_server)
            {
                RefreshIdentity(player, rec);
                TF_ScoreUpdate st{};
                st.player = player;
                st.score = rec.row.score;
                st.kills = rec.row.kills;
                st.deaths = rec.row.deaths;
                st.captures = rec.row.captures;
                st.medals = rec.row.medals;
                st.faction = static_cast<uint8_t>(rec.row.faction);
                st.cls = static_cast<uint8_t>(rec.row.cls);
                SendWire(id, kTFMsgScoreUpdate, &st, sizeof(st));
            }
        }

        // Leaver sweep doubles as the recycled-PlayerId hygiene (ClearPlayer) —
        // this lane needs no TFServerSim::CleanupPlayerSession edit. The listen
        // host's own kTFLocalHostPlayer id is never a socket client, so its row
        // survives for the whole session (correct: it never recycles).
        std::erase_if(m_knownClients,
                      [this, &nm](PlayerId id)
                      {
                          if (nm.GetClients().contains(id))
                              return false;
                          ClearPlayer(id);
                          return true;
                      });
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
