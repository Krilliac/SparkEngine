/**
 * @file TFMedalSystem.cpp
 * @brief Medal detection (killstreak/multikill/Savior/Avenger), server score
 *        rows, TF_MedalAward / TF_ScoreUpdate wire traffic, and the local
 *        medal toast overlay. See TFMedalSystem.h for the full design note.
 */
#include "Game/TFMedalSystem.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFSquadSystem.h"
#include "Net/TFServerSim.h"
#include "UI/TFScoreboard.h"

#include "Graphics/GraphicsEngine.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
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
        constexpr float kToastSec = 4.0f;           ///< medal toast lifetime
        constexpr float kToastFadeSec = 1.0f;       ///< fade-out tail

        // Medal definition table — indexed by TFMedalId. Icon families map onto
        // the shipped Assets/Textures/MMOFPS/ui/128/medal_<family>_<1|2|3>.png
        // set (verified present: hex/star/shield/ring x 1-3 = the 12 icons):
        //   hex    = multikills  (fixed tier 1/2/3 = Double/Triple/Quad)
        //   star   = killstreaks (fixed tier 1/2/3 = Rampage/Dominating/Unstoppable)
        //   shield = Savior      (tier escalates with session count)
        //   ring   = Avenger     (tier escalates with session count)
        constexpr TFMedalDef kMedals[static_cast<size_t>(TFMedalId::COUNT)] = {
            {"double_kill", "Double Kill", 50, "hex", 1}, {"triple_kill", "Triple Kill", 100, "hex", 2},
            {"quad_kill", "Quad Kill", 150, "hex", 3},    {"rampage", "Rampage", 100, "star", 1},
            {"dominating", "Dominating", 250, "star", 2}, {"unstoppable", "Unstoppable", 500, "star", 3},
            {"savior", "Savior", 75, "shield", 0},        {"avenger", "Avenger", 50, "ring", 0},
        };
        constexpr TFMedalDef kUnknownMedal = {"unknown", "Medal", 0, "hex", 1};

    } // namespace

    const TFMedalDef& TFMedalDefOf(uint8_t medalId)
    {
        if (medalId >= static_cast<uint8_t>(TFMedalId::COUNT))
            return kUnknownMedal;
        return kMedals[medalId];
    }

    std::string TFMedalIconPath(uint8_t medalId, uint16_t sessionCount)
    {
        const TFMedalDef& def = TFMedalDefOf(medalId);
        int tier = def.iconTier;
        if (tier == 0) // session-count tiered (Savior/Avenger)
            tier = sessionCount >= 50 ? 3 : sessionCount >= 10 ? 2 : 1;
        char path[96];
        std::snprintf(path, sizeof(path), "Assets/Textures/MMOFPS/ui/128/medal_%s_%d.png", def.iconFamily, tier);
        return path;
    }

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    TFMedalSystem::TFMedalSystem() = default;
    TFMedalSystem::~TFMedalSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFMedalSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });
        events.Subscribe<EvPlayerDamaged>([this](const EvPlayerDamaged& ev) { OnPlayerDamaged(ev); });
        events.Subscribe<EvXPAwarded>([this](const EvXPAwarded& ev) { OnXPAwarded(ev); });

        // Lane self-wiring (TFClientNet::SetChatUI pattern): context pointers are
        // published before any Initialize, so ctx.scoreboard is already valid
        // regardless of boot order — no TFTypes.h/TFHUD edits needed.
        if (ctx.scoreboard)
            ctx.scoreboard->SetMedalSystem(this);

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFMedalSystem initialized");
        return true;
    }

    void TFMedalSystem::Shutdown()
    {
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
        m_knownClients.clear();
#endif
        if (m_ctx && m_ctx->scoreboard)
            m_ctx->scoreboard->SetMedalSystem(nullptr);
        m_server.clear();
        m_recentDamage.clear();
        m_lastKiller.clear();
        m_mirror.clear();
        m_toasts.clear();
        m_initialized = false;
    }

    void TFMedalSystem::Update(float deltaTime)
    {
        m_clock += deltaTime;
        if (!m_initialized || !m_ctx)
            return;

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle (TFSquadSystem pattern: registered
        // after link-up so the real handler wins the per-type slot).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            m_mirror.clear();
            m_toasts.clear();
        }

        if (m_ctx->IsAuthority())
            PollJoinsLeaves();
#endif

        // Dirty-row flush at kTFScoreFlushHz (authority only).
        if (m_ctx->IsAuthority())
        {
            m_flushAccum += deltaTime;
            if (m_flushAccum >= 1.0f / kTFScoreFlushHz)
            {
                m_flushAccum = 0.0f;
                FlushDirtyRows();
            }
        }

        // Toast lifetimes (local player overlay).
        for (Toast& t : m_toasts)
            t.ttl -= deltaTime;
        while (!m_toasts.empty() && m_toasts.front().ttl <= 0.0f)
            m_toasts.pop_front();
    }

    void TFMedalSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime; // event-driven; nothing to do on the fixed step
    }

    double TFMedalSystem::NowSec() const
    {
        if (m_ctx && m_ctx->IsAuthority() && m_ctx->serverSim)
            return m_ctx->serverSim->ServerTime();
        return m_clock;
    }

    // ---------------------------------------------------------------------------
    // Server: row bookkeeping
    // ---------------------------------------------------------------------------

    TFMedalSystem::ServerRec& TFMedalSystem::Ensure(PlayerId player)
    {
        ServerRec& rec = m_server[player];
        RefreshIdentity(player, rec);
        return rec;
    }

    void TFMedalSystem::RefreshIdentity(PlayerId player, ServerRec& rec)
    {
        if (!m_ctx || !m_ctx->players)
            return;
        const FactionId f = m_ctx->players->FactionOf(player);
        if (f != FactionId::None)
            rec.row.faction = f;
        PawnInfo pi{};
        if (m_ctx->players->GetPawnByPlayer(player, pi))
            rec.row.cls = pi.cls;
    }

    void TFMedalSystem::RecomputeScore(ServerRec& rec)
    {
        rec.row.score =
            rec.row.kills * kTFScorePerKill + rec.row.captures * kTFScorePerCapture + rec.row.medals * kTFScorePerMedal;
    }

    void TFMedalSystem::ClearPlayer(PlayerId player)
    {
        m_server.erase(player);
        m_recentDamage.erase(player);
        m_lastKiller.erase(player);
    }

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

    // ---------------------------------------------------------------------------
    // Cross-system surface (TFScoreboard)
    // ---------------------------------------------------------------------------

    bool TFMedalSystem::GetScoreRow(PlayerId player, TFScoreRow& out) const
    {
        if (m_ctx && m_ctx->IsAuthority())
        {
            auto it = m_server.find(player);
            if (it == m_server.end())
                return false;
            out = it->second.row;
            return true;
        }
        auto it = m_mirror.find(player);
        if (it == m_mirror.end())
            return false;
        out = it->second;
        return true;
    }

    void TFMedalSystem::ForEachScoreRow(const std::function<void(PlayerId, const TFScoreRow&)>& fn) const
    {
        if (m_ctx && m_ctx->IsAuthority())
        {
            for (const auto& [player, rec] : m_server)
                fn(player, rec.row);
            return;
        }
        for (const auto& [player, row] : m_mirror)
            fn(player, row);
    }

    uint16_t TFMedalSystem::MedalCountOf(PlayerId player, TFMedalId medal) const
    {
        auto it = m_server.find(player);
        if (it == m_server.end() || medal >= TFMedalId::COUNT)
            return 0;
        return it->second.medalCounts[static_cast<size_t>(medal)];
    }

    // ---------------------------------------------------------------------------
    // Client: mirror + toasts
    // ---------------------------------------------------------------------------

    void TFMedalSystem::ClientHandleScore(const TF_ScoreUpdate& st)
    {
        TFScoreRow& row = m_mirror[st.player];
        row.score = st.score;
        row.kills = st.kills;
        row.deaths = st.deaths;
        row.captures = st.captures;
        row.medals = st.medals;
        row.faction =
            st.faction < static_cast<uint8_t>(FactionId::COUNT) ? static_cast<FactionId>(st.faction) : FactionId::None;
        row.cls = st.cls < static_cast<uint8_t>(ClassId::COUNT) ? static_cast<ClassId>(st.cls) : ClassId::COUNT;
    }

    void TFMedalSystem::ClientHandleMedal(const TF_MedalAward& award)
    {
        if (!m_ctx || !m_ctx->HasLocalPlayer() || award.player != m_ctx->localPlayer)
            return; // owner-only toast
        Toast t{};
        t.medal = award.medal;
        t.sessionCount = award.sessionCount;
        t.ttl = kToastSec;
        m_toasts.push_back(t);
        while (m_toasts.size() > 4) // never stack a wall of toasts
            m_toasts.pop_front();
    }

#ifdef ENABLE_NETWORKING

    bool TFMedalSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFMedalSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgScoreUpdate),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_ScoreUpdate))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_ScoreUpdate st{};
                               std::memcpy(&st, m.payload.data(), sizeof(st));
                               ClientHandleScore(st);
                           });
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgMedalAward),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_MedalAward))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_MedalAward award{};
                               std::memcpy(&award, m.payload.data(), sizeof(award));
                               ClientHandleMedal(award);
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] medal/score mirror handlers registered");
    }

    void TFMedalSystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgScoreUpdate),
                           [](const Spark::Net::NetworkMessage&) {});
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgMedalAward),
                           [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

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

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFMedalSystem::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || m_toasts.empty())
            return;

        GraphicsEngine* gfx = m_ctx->engine ? m_ctx->engine->GetGraphics() : nullptr;
        const bool canDrawIcons = gfx && gfx->GetDevice() && gfx->GetContext();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float iconSize = 44.0f;
        const float rowH = iconSize + 10.0f;
        const float width = 320.0f;
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - width) * 0.5f, vp->Pos.y + vp->Size.y * 0.16f));
        ImGui::SetNextWindowSize(ImVec2(width, rowH * static_cast<float>(m_toasts.size()) + 16.0f));
        ImGui::SetNextWindowBgAlpha(0.30f); // faint plate so toasts read over combat
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (!ImGui::Begin("##TFMedalToasts", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        for (const Toast& t : m_toasts)
        {
            const TFMedalDef& def = TFMedalDefOf(t.medal);
            const float alpha = t.ttl < kToastFadeSec ? std::max(0.0f, t.ttl / kToastFadeSec) : 1.0f;

            ImGui::BeginGroup();
            bool drewIcon = false;
            if (canDrawIcons)
            {
                if (ID3D11ShaderResourceView* srv = gfx->GetOrLoadTextureSRV(TFMedalIconPath(t.medal, t.sessionCount)))
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
                    ImGui::Image(static_cast<void*>(srv), ImVec2(iconSize, iconSize));
                    ImGui::PopStyleVar();
                    drewIcon = true;
                }
            }
            if (!drewIcon)
                ImGui::Dummy(ImVec2(iconSize, iconSize)); // headless / missing texture
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, alpha), "%s", def.name);
            if (t.sessionCount > 1)
                ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, alpha), "+%u XP   (x%u)", def.xp, t.sessionCount);
            else
                ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, alpha), "+%u XP", def.xp);
            ImGui::EndGroup();
            ImGui::EndGroup();
            ImGui::Spacing();
        }

        ImGui::End();
    }

    void TFMedalSystem::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Medals"))
            return;
        ImGui::Text("server rows : %zu", m_server.size());
        ImGui::Text("mirror rows : %zu", m_mirror.size());
        ImGui::Text("awarded     : %u", m_medalsAwarded);
        ImGui::Text("rows sent   : %u", m_rowsSent);
        ImGui::Text("bad packets : %u", m_badPackets);
        ImGui::Text("toasts      : %zu", m_toasts.size());
        if (m_ctx && m_ctx->IsAuthority())
        {
            for (const auto& [player, rec] : m_server)
                ImGui::Text("  P%u  score %u  K%u/D%u  cap %u  medals %u  streak %u", player, rec.row.score,
                            rec.row.kills, rec.row.deaths, rec.row.captures, rec.row.medals, rec.streak);
        }
    }

#else // !SPARK_HAS_IMGUI — headless: detection + wire only

    void TFMedalSystem::RenderUI() {}
    void TFMedalSystem::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
