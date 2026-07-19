/**
 * @file TFDeployableSystemServer.cpp
 * @brief TFDeployableSystem server placement + damage/destroy paths:
 *        ServerTryPlaceDeployable (class gate, per-player limits, drop point,
 *        W6 validation before any state changes), direct + splash damage entry
 *        points and the destroy/replicate teardown. Split from
 *        TFDeployableSystem.cpp; the shared internals live in
 *        TFDeployableSystemInternal.h.
 */
#include "Game/TFDeployableSystem.h"

#include "Game/TFDeployableSystemInternal.h"
#include "Game/TFDeployableTypes.h"
#include "Game/TFPlayerSystem.h"
#include "World/TFWorldSetup.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace Terrafront
{

    using namespace DeploySysDetail;

    namespace
    {

        constexpr float kPlaceAheadM = 2.0f; // drop point in front of the pawn
        constexpr float kPlaceLiftM = 0.05f; // epsilon above terrain

        // Synthetic network ids when no ECS world exists (headless unit tests);
        // distinct base from TFPlayerSystem's 1000000 pawn range.
        EntityId g_nextSyntheticEntity = 2000000;

    } // namespace

    // ---------------------------------------------------------------------------
    // Placement (server)
    // ---------------------------------------------------------------------------

    const char* TFDeployableSystem::ResultText(TFDeployResult r)
    {
        switch (r)
        {
        case TFDeployResult::Ok:
            return "deployed";
        case TFDeployResult::NotAuthority:
            return "server only";
        case TFDeployResult::DataMissing:
            return "data tables not loaded";
        case TFDeployResult::NoPawn:
            return "you must be alive";
        case TFDeployResult::WrongClass:
            return "your class cannot place that";
        case TFDeployResult::BadKind:
            return "unknown deployable kind";
        case TFDeployResult::SpawnFailed:
            return "placement failed";
        case TFDeployResult::TooSteep:
            return "ground too steep";
        case TFDeployResult::TooClose:
            return "too close to another deployable";
        case TFDeployResult::HostileRegion:
            return "cannot deploy in enemy territory";
        case TFDeployResult::Blocked:
            return "placement blocked by an obstacle";
        case TFDeployResult::LimitReached:
            return "deployable limit reached";
        }
        return "?";
    }

    TFDeployResult TFDeployableSystem::ServerTryPlaceDeployable(PlayerId player, DeployableKind kind)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players)
            return TFDeployResult::NotAuthority;
        const TFDeployableSpec* spec = TFDeploySpecOf(kind);
        if (!spec)
            return TFDeployResult::BadKind;

        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive)
            return TFDeployResult::NoPawn;

        // Class gate is spec-table-driven (W3's Fabricator/Medtech split, kept).
        if (static_cast<uint8_t>(pawn.cls) != spec->requiredClass)
            return TFDeployResult::WrongClass;

        // Per-player limits: at the PER-KIND cap the oldest of that kind is
        // replaced (the W3 one-per-kind rule, generalized); below it the TOTAL
        // active cap refuses outright.
        EntityId victim = 0;
        {
            auto& byKind = m_ownerIndex[player];
            const auto& sameKind = byKind[static_cast<uint8_t>(kind)];
            if (!sameKind.empty() && sameKind.size() >= static_cast<size_t>(spec->maxActive))
            {
                victim = sameKind.front(); // oldest first (placement order)
            }
            else
            {
                size_t total = 0;
                for (const auto& kv : byKind)
                    total += kv.second.size();
                if (total >= kTFDeployMaxActivePerPlayer)
                    return TFDeployResult::LimitReached;
            }
        }

        // Drop point: 2 m ahead on the pawn's facing (yaw convention: fwd = sin/cos,
        // see TFPlayerSystem::FindSkyanchorSpawn), clamped onto the terrain.
        float pos[3];
        pos[0] = pawn.pos[0] + std::sin(pawn.yaw) * kPlaceAheadM;
        pos[2] = pawn.pos[2] + std::cos(pawn.yaw) * kPlaceAheadM;
        pos[1] = (m_ctx->world ? m_ctx->world->TerrainHeightAt(pos[0], pos[2]) : pawn.pos[1]) + kPlaceLiftM;

        // W6: validate BEFORE any state changes (the to-be-replaced victim is
        // excluded from spacing — the new placement supersedes it).
        if (const TFDeployResult vr = ValidatePlacement(*spec, pawn.faction, pos, victim); vr != TFDeployResult::Ok)
        {
            ++m_refusedPlacements;
            return vr;
        }

        if (victim != 0)
            ServerDestroyDeployable(victim, "replaced");

        TFDeployableView view{};
        view.kind = kind;
        view.owner = player;
        view.faction = pawn.faction;
        view.yaw = pawn.yaw;
        view.maxHealth = spec->health;
        view.health = view.maxHealth;
        view.life = spec->lifeSec;
        view.pos[0] = pos[0];
        view.pos[1] = pos[1];
        view.pos[2] = pos[2];

        Rec rec{};
        rec.local = CreateDeployableEntity(view);
        view.entity = (rec.local != 0) ? rec.local : g_nextSyntheticEntity++;
        rec.view = view;
        rec.nextShotAt = m_clock;
        rec.nextPulseAt = m_clock + kAmmoPackPulseSec;

        m_deployables[view.entity] = rec;
        if (view.kind == kDeployShieldWall)
            CreateWallBody(m_deployables[view.entity]); // authority path; Jolt null-checked inside
        // Re-acquire the index — ServerDestroyDeployable above may have erased
        // emptied kind/owner entries and invalidated earlier references.
        m_ownerIndex[player][static_cast<uint8_t>(kind)].push_back(view.entity);
        ++m_placed;

#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            SendCreate(kInvalidPlayer, m_deployables[view.entity].view);
#endif

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] deployable %u (kind %u) placed by player %u at (%.0f %.0f %.0f)",
                       view.entity, static_cast<unsigned>(kind), player, view.pos[0], view.pos[1], view.pos[2]);

        // W8: server-side placement bus event (synchronous, authority-only by
        // construction -- this whole function early-outs on non-authority).
        // Fired LAST so subscribers observe the fully registered deployable
        // (m_deployables/m_ownerIndex updated, Create already replicated), and
        // so a re-entrant subscriber chain (e.g. TFDirectiveSystem tier payout
        // -> ServerAwardXP -> EvXPAwarded) runs after all state changes here.
        if (m_events)
            m_events->Fire(EvDeployablePlaced{view.entity, view.kind, view.owner, view.faction});

        return TFDeployResult::Ok;
    }

    // ---------------------------------------------------------------------------
    // Damage / destroy (server)
    // ---------------------------------------------------------------------------

    void TFDeployableSystem::ServerDamageDeployable(EntityId deployable, PlayerId attackerPlayer, float amount)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || amount <= 0.0f)
            return;
        auto it = m_deployables.find(deployable);
        if (it == m_deployables.end())
            return;
        Rec& rec = it->second;

        // Same-faction fire is ignored (cheap objects; W4 revisits grief policy).
        if (m_ctx->players && attackerPlayer != kInvalidPlayer &&
            m_ctx->players->FactionOf(attackerPlayer) == rec.view.faction)
            return;

        rec.view.health -= amount;
        if (rec.view.health > 0.0f)
        {
#ifdef ENABLE_NETWORKING
            if (ServerNetActive())
                SendUpdate(rec.view);
#endif
            return;
        }
        ++m_destroyedByDamage;
        ServerDestroyDeployable(deployable, "destroyed");
    }

    void TFDeployableSystem::ServerSplashDamageDeployables(const float at[3], float radiusM, float damage,
                                                           PlayerId attackerPlayer)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || radiusM <= 0.0f || damage <= 0.0f)
            return;

        // Collect first — ServerDamageDeployable may erase entries.
        std::vector<std::pair<EntityId, float>> hits;
        for (const auto& [entity, rec] : m_deployables)
        {
            const float d = std::sqrt(Dist2(rec.view.pos, at));
            if (d > radiusM)
                continue;
            const float dmg = damage * (1.0f - d / radiusM); // pawn splash falloff
            if (dmg > 1.0f)
                hits.emplace_back(entity, dmg);
        }
        for (const auto& [entity, dmg] : hits)
            ServerDamageDeployable(entity, attackerPlayer, dmg);
    }

    void TFDeployableSystem::ServerDestroyDeployable(EntityId entity, const char* why)
    {
        auto it = m_deployables.find(entity);
        if (it == m_deployables.end())
            return;
        Rec& rec = it->second;

#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            SendDestroy(entity);
#endif

        auto oit = m_ownerIndex.find(rec.view.owner);
        if (oit != m_ownerIndex.end())
        {
            auto kit = oit->second.find(static_cast<uint8_t>(rec.view.kind));
            if (kit != oit->second.end())
            {
                auto& list = kit->second;
                list.erase(std::remove(list.begin(), list.end(), entity), list.end());
                if (list.empty())
                    oit->second.erase(kit);
            }
            if (oit->second.empty())
                m_ownerIndex.erase(oit);
        }

        ReleaseWallBody(rec);
        DestroyLocalEntity(rec.local);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] deployable %u removed (%s)", entity, why);
        m_deployables.erase(it);
    }

} // namespace Terrafront
