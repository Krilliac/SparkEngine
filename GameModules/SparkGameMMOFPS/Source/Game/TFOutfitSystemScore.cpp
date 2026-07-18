/**
 * @file TFOutfitSystemScore.cpp
 * @brief TFOutfitSystem W12 competition score: aggregation off the existing
 *        EvPlayerKilled/EvXPAwarded event surfaces, the ISO-week rollover and
 *        the TF_OutfitLeaderboard snapshot builder. Split from
 *        TFOutfitSystem.cpp; the shared helpers live in
 *        TFOutfitSystemInternal.h.
 */
#include "Game/TFOutfitSystem.h"

#include "Game/TFOutfitSystemInternal.h"
#include "Game/TFPlayerSystem.h"      // FactionOf — the kill-credit team filter
#include "Game/TFProgressionSystem.h" // kXPReasonCapture* (score hooks)
#include "Utils/LogMacros.h"
#include "World/TFAlertSystem.h" // kXPReasonAlert (alert-win score hook)

#include <algorithm>

namespace Terrafront
{

    using namespace OutfitDetail;

    // ---------------------------------------------------------------------------
    // Server: competition score aggregation + leaderboard (W12)
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::RolloverIfNeeded()
    {
        if (!m_store.IsOpen())
            return;
        const uint32_t wk = TFOutfitISOWeekKey(NowMs());
        if (wk == m_lastWeekKey)
            return; // still inside the week RolloverWeek last stamped
        const size_t stamped = m_store.RolloverWeek(wk);
        if (stamped > 0)
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit weekly scores rolled to ISO week %u (%zu outfits)",
                           wk, stamped);
        m_lastWeekKey = wk;
    }

    void TFOutfitSystem::ServerAddOutfitScore(PlayerId player, uint32_t points)
    {
        if (player == kInvalidPlayer || points == 0)
            return;
        const BoundChar* bc = BoundCharOf(player);
        if (!bc)
            return; // bots / pre-onboarding sessions never score
        if (!EnsureStoreOpen())
            return;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return; // unaffiliated player
        if (m_store.AddScore(rec->id, points, TFOutfitISOWeekKey(NowMs())))
            ++m_scoreEvents;
    }

    void TFOutfitSystem::OnPlayerKilledScore(const EvPlayerKilled& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        // Same credit filter as TFMedalSystem/TFProgressionSystem: no score for
        // environment deaths, suicides, or team kills.
        if (ev.killer == kInvalidPlayer || ev.killer == ev.victim)
            return;
        if (m_ctx->players)
        {
            const FactionId killerF = m_ctx->players->FactionOf(ev.killer);
            const FactionId victimF = m_ctx->players->FactionOf(ev.victim);
            if (killerF != FactionId::None && killerF == victimF)
                return;
        }
        ServerAddOutfitScore(ev.killer, kTFOutfitScorePerKill);
    }

    void TFOutfitSystem::OnXPAwardedScore(const EvXPAwarded& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        // Capture participation (the canonical reason codes 4/5/6 —
        // TFMedalSystem/TFAlertSystem precedent) and alert wins (13 — the
        // per-winning-participant payout TFAlertSystem routes through
        // ServerAwardXP). Everything else is ignored.
        if (ev.reason == kXPReasonCaptureFacility || ev.reason == kXPReasonCaptureFort ||
            ev.reason == kXPReasonCaptureOutpost)
            ServerAddOutfitScore(ev.player, kTFOutfitScorePerCapture);
        else if (ev.reason == kXPReasonAlert)
            ServerAddOutfitScore(ev.player, kTFOutfitScorePerAlertWin);
    }

    void TFOutfitSystem::ServerSendLeaderboard(PlayerId requester)
    {
        RolloverIfNeeded(); // the weekly column is always current-week truth

        // Rank every outfit: weekly desc, all-time desc, then name (stable,
        // deterministic order for equal scores).
        std::vector<const TFOutfitRecord*> ranked;
        ranked.reserve(m_store.OutfitCount());
        for (const TFOutfitRecord& o : m_store.All())
            ranked.push_back(&o);
        std::sort(ranked.begin(), ranked.end(),
                  [](const TFOutfitRecord* a, const TFOutfitRecord* b)
                  {
                      if (a->weeklyScore != b->weeklyScore)
                          return a->weeklyScore > b->weeklyScore;
                      if (a->allTimeScore != b->allTimeScore)
                          return a->allTimeScore > b->allTimeScore;
                      return a->name < b->name;
                  });

        uint32_t yourOutfitId = 0;
        if (const BoundChar* bc = BoundCharOf(requester))
            if (const TFOutfitRecord* mine = m_store.FindByCharacter(bc->charId))
                yourOutfitId = mine->id;

        TF_OutfitLeaderboard lb{};
        lb.totalOutfits = static_cast<uint16_t>(std::min<size_t>(ranked.size(), 0xFFFF));
        lb.weekKey = TFOutfitISOWeekKey(NowMs());
        lb.yourIndex = 0xFF;

        auto fillRow = [](TF_OutfitLbRow& row, const TFOutfitRecord& rec, size_t rank)
        {
            row.outfitId = rec.id;
            row.rank = static_cast<uint32_t>(rank);
            row.weekly = rec.weeklyScore;
            row.allTime = rec.allTimeScore;
            CopyField(row.name, sizeof(row.name), rec.name);
            CopyField(row.tag, sizeof(row.tag), rec.tag);
        };

        const size_t topN = std::min<size_t>(ranked.size(), kTFOutfitLbTopN);
        for (size_t i = 0; i < topN; ++i)
        {
            fillRow(lb.rows[lb.count], *ranked[i], i + 1);
            if (ranked[i]->id == yourOutfitId)
                lb.yourIndex = lb.count;
            ++lb.count;
        }
        // Requester's outfit outside the top N: append its row at its TRUE rank
        // so the panel can highlight it either way.
        if (yourOutfitId != 0 && lb.yourIndex == 0xFF)
        {
            for (size_t i = topN; i < ranked.size(); ++i)
            {
                if (ranked[i]->id != yourOutfitId)
                    continue;
                fillRow(lb.rows[lb.count], *ranked[i], i + 1);
                lb.yourIndex = lb.count;
                ++lb.count;
                break;
            }
        }

        SendWireTo(requester, kTFMsgOutfitLeaderboard, &lb, sizeof(lb));
    }

} // namespace Terrafront
