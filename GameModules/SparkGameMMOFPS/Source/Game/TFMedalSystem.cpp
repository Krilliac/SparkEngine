/**
 * @file TFMedalSystem.cpp
 * @brief TFMedalSystem core: the medal definition table + icon paths, system
 *        lifecycle (init/update/shutdown), server score-row bookkeeping and
 *        the TFScoreboard cross-system surface. Server-side detection and
 *        wire-out live in TFMedalSystemServer.cpp; the client mirror, toast
 *        queue and rendering in TFMedalSystemClient.cpp. See TFMedalSystem.h
 *        for the full design note.
 */
#include "Game/TFMedalSystem.h"

#include "Game/TFPlayerSystem.h"
#include "Net/TFServerSim.h"
#include "UI/TFScoreboard.h"

#include "Utils/LogMacros.h"

#include <cstdio>

namespace Terrafront
{

    namespace
    {

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

} // namespace Terrafront
