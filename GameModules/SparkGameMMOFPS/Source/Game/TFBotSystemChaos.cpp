/**
 * @file TFBotSystemChaos.cpp
 * @brief TFBotSystem chaos exercise mode (bots-chaos lane): run start/stop,
 *        teleport-scatter, randomized objective re-rolls, deployable/vehicle
 *        purchase exercise and the W9 deterministic pilot slots. Split from
 *        TFBotSystem.cpp; the shared tuning constants and shims live in
 *        TFBotSystemInternal.h.
 */
#include "Game/TFBotSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFBotSystemInternal.h"
#include "Game/TFChaosHarness.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Net/TFServerSim.h"
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Terrafront
{

    using namespace BotDetail;

    // ---------------------------------------------------------------------------
    // Chaos exercise mode (bots-chaos lane, 2026-07-10)
    // ---------------------------------------------------------------------------

    void TFBotSystem::ServerStartChaos(uint32_t botCount, float seconds)
    {
        if (!m_initialized || !m_ctx)
            return;
        if (!m_ctx->IsAuthority())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] ServerStartChaos ignored — not the authority");
            return;
        }

        if (botCount == 0)
        {
            m_chaosActive = false;
            m_chaosEndsAt = 0.0;
            ServerSetBotCount(0);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] chaos stopped, bots despawned");
            return;
        }

        ServerSetBotCount(std::min(botCount, kTFMaxBots));
        const double now = Now();
        m_chaosActive = true;
        m_chaosEndsAt = seconds > 0.0f ? now + static_cast<double>(seconds) : 0.0;
        m_chaosDeployTries = 0;
        m_chaosVehicleTries = 0;
        m_chaosVehiclePurchases = 0;
        for (Bot& b : m_bots)
        {
            b.chaosScatterPending = true; // scatter on the first alive think
            // Hold the first drop point a while (fight/capture there) before
            // the randomized re-rolls kick in; stagger per slot.
            b.chaosRerollAt = now + 15.0 + static_cast<double>(b.id - kTFBotIdBase) * 0.7;
            b.chaosUtilityAt = now + 5.0 + static_cast<double>(m_rng() % 60) * 0.1;
            b.pilotBuyAt = 0.0;
        }

        // W9 pilots: bankroll + rank the designated pilots through the REAL
        // progression paths (grant/award/unlock — the same seams tf_giveflux
        // and gameplay use) so the Vulture/Drifter purchase gates actually
        // open, then let the validated terminal purchase do the rest. Every
        // bot gets Drifter money so incidental terminal purchases can succeed
        // too. The 750-flux wallet cap forces the grant/unlock interleave.
        if (m_ctx->progression)
        {
            for (Bot& b : m_bots)
            {
                m_ctx->progression->ServerGrantFlux(b.id, kBotChaosFluxGrant);
                if (!ChaosIsPilot(b))
                    continue;
                if ((b.id - kTFBotIdBase) == kChaosVulturePilotSlot)
                {
                    m_ctx->progression->ServerAwardXP(b.id, kPilotXPGrant, kXPReasonKill); // rank >= 15
                    m_ctx->progression->ServerGrantFlux(b.id, kFluxWalletCap);
                    m_ctx->progression->ServerTryUnlock(b.id, "veh_ravager"); // Vulture prereq
                    m_ctx->progression->ServerTryUnlock(b.id, "veh_vulture");
                }
                m_ctx->progression->ServerGrantFlux(b.id, kFluxWalletCap); // spawn money
            }
        }

        if (m_chaos)
        {
            m_chaos->OnChaosStart(BotCount(), seconds);
            m_chaos->SetAbilitySeamPresent(AbilitySeamPresent(m_ctx));
        }
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] CHAOS: %u bots for %.0f s (0 = open-ended)", BotCount(),
                       static_cast<double>(seconds));
    }

    std::string TFBotSystem::ValidationReport()
    {
        if (!m_chaos)
            return "[TF-VALIDATE] RESULT: FAIL harness=missing";
        return m_chaos->BuildReport();
    }

    RegionId TFBotSystem::ChaosArenaRegion() const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return kInvalidRegion;
        const ContinentDef& cont = m_ctx->data->GetContinent();
        const RegionDef* first = nullptr;
        for (const RegionDef& r : cont.regions)
        {
            if (r.tier == "skyanchor")
                continue;
            if (!first)
                first = &r;
            if (r.tier == "facility")
                return r.id; // biggest brawl space wins
        }
        return first ? first->id : kInvalidRegion;
    }

    void TFBotSystem::ChaosScatter(Bot& bot, double now)
    {
        if (!m_ctx->serverSim || !m_ctx->world || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const ContinentDef& cont = m_ctx->data->GetContinent();
        if (cont.regions.empty())
            return;

        // W9 pilots: drop at the OWN faction skyanchor's vehicle terminal so the
        // purchase gate (friendly terminal within 25 m) deterministically opens.
        const uint32_t slot = bot.id - kTFBotIdBase;
        if (ChaosIsPilot(bot))
        {
            for (const RegionDef& r : cont.regions)
            {
                if (r.tier != "skyanchor" || r.homeFaction != bot.faction || !r.vehicleTerminal.has_value())
                    continue;
                const float tx = (*r.vehicleTerminal)[0];
                const float tz = (*r.vehicleTerminal)[1];
                const float ang = static_cast<float>(m_rng() % 6283u) * 0.001f;
                const float rad = 2.0f + static_cast<float>(m_rng() % 60u) * 0.1f; // 2-8 m off the pad
                const float x = tx + std::sin(ang) * rad;
                const float z = tz + std::cos(ang) * rad;
                const float y = m_ctx->world->TerrainHeightAt(x, z) + 0.5f;
                m_ctx->serverSim->TeleportPawn(bot.id, x, y, z);
                bot.objectiveRegion = r.id;
                bot.objectiveX = tx; // stay on the pad until the purchase lands
                bot.objectiveZ = tz;
                bot.stuckRefPos[0] = x;
                bot.stuckRefPos[1] = y;
                bot.stuckRefPos[2] = z;
                bot.stuckSince = now;
                bot.jumping = false;
                ResetAvoidance(bot, x, z, now); // W12: fresh nav window at the new drop
                bot.unstickCount = 0;
                bot.pilotBuyAt = now + 1.0; // let the teleport settle one tick
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] chaos pilot: bot 0x%08X %s -> terminal r%u (%.0f, %.0f)",
                               bot.id, FactionTag(bot.faction), static_cast<unsigned>(r.id), x, z);
                return;
            }
            // No terminal for this faction (data changed): fall through to the
            // normal scatter below.
        }

        // Even slots -> the shared multi-faction arena (guaranteed contact and
        // contested points). Odd slots -> a random region this bot's faction can
        // actually capture (guaranteed single-attacker progress somewhere).
        const RegionDef* dest = nullptr;
        if ((slot % 2u) == 0u)
        {
            const RegionId arena = ChaosArenaRegion();
            for (const RegionDef& r : cont.regions)
            {
                if (r.id == arena)
                {
                    dest = &r;
                    break;
                }
            }
        }
        if (!dest)
        {
            std::vector<const RegionDef*> candidates;
            candidates.reserve(cont.regions.size());
            for (const RegionDef& r : cont.regions)
            {
                if (r.tier == "skyanchor")
                    continue;
                if (QueryRegionCapturable(m_ctx->regions, r.id, bot.faction, /*fallback*/ true))
                    candidates.push_back(&r);
            }
            if (!candidates.empty())
                dest = candidates[m_rng() % candidates.size()];
        }
        if (!dest)
            return;

        // Drop near a capture point (region center as fallback), on a 4-18 m
        // ring so stacked bots spread out and have to walk THROUGH the plateau
        // structures (collision exercise) to converge on the point.
        float cpX = dest->centerX, cpZ = dest->centerZ;
        if (!dest->capturePoints.empty())
        {
            const auto& cp = dest->capturePoints[m_rng() % dest->capturePoints.size()];
            cpX = cp[0];
            cpZ = cp[1];
        }
        const float ang = static_cast<float>(m_rng() % 6283u) * 0.001f;
        const float rad = 4.0f + static_cast<float>(m_rng() % 140u) * 0.1f;
        const float x = cpX + std::sin(ang) * rad;
        const float z = cpZ + std::cos(ang) * rad;
        const float y = m_ctx->world->TerrainHeightAt(x, z) + 0.5f;
        m_ctx->serverSim->TeleportPawn(bot.id, x, y, z);

        bot.objectiveRegion = dest->id;
        bot.objectiveX = cpX; // converge on the point itself, not the ring drop
        bot.objectiveZ = cpZ;
        bot.stuckRefPos[0] = x;
        bot.stuckRefPos[1] = y;
        bot.stuckRefPos[2] = z;
        bot.stuckSince = now;
        bot.jumping = false;
        ResetAvoidance(bot, x, z, now); // W12: fresh nav window at the new drop
        bot.unstickCount = 0;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] chaos scatter: bot 0x%08X %s -> region %u (%.0f, %.0f)", bot.id,
                       FactionTag(bot.faction), static_cast<unsigned>(dest->id), x, z);
    }

    void TFBotSystem::ChaosMaybeReroll(Bot& bot, double now)
    {
        // W9 pilots keep their scripted objectives (terminal, then far region):
        // a re-roll mid-plan would strand the purchase or shorten the ride.
        if (ChaosIsPilot(bot))
            return;
        if (now < bot.chaosRerollAt)
            return;
        bot.chaosRerollAt = now + 6.0 + static_cast<double>(m_rng() % 100u) * 0.1; // 6-16 s
        if ((m_rng() & 1u) != 0u)
            return; // keep the lattice-scored objective half the time
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const ContinentDef& cont = m_ctx->data->GetContinent();
        std::vector<const RegionDef*> pool;
        pool.reserve(cont.regions.size());
        for (const RegionDef& r : cont.regions)
        {
            if (r.tier != "skyanchor")
                pool.push_back(&r);
        }
        if (pool.empty())
            return;
        const RegionDef& r = *pool[m_rng() % pool.size()];
        bot.objectiveRegion = r.id;
        bot.objectiveX = r.centerX;
        bot.objectiveZ = r.centerZ;
        if (!r.capturePoints.empty())
        {
            const auto& cp = r.capturePoints[m_rng() % r.capturePoints.size()];
            bot.objectiveX = cp[0];
            bot.objectiveZ = cp[1];
        }
    }

    void TFBotSystem::ChaosTryUtility(Bot& bot, double now)
    {
        if (now < bot.chaosUtilityAt)
            return;
        bot.chaosUtilityAt = now + 8.0 + static_cast<double>(m_rng() % 80u) * 0.1; // 8-16 s

        // Deployables through the real validated entry point (class-gated;
        // refusals — slope/spacing/hostile-region/limit — are free exercise).
        if (m_ctx->deployables)
        {
            if (bot.cls == ClassId::Fabricator)
            {
                const DeployableKind kind =
                    (m_rng() & 1u) != 0u ? DeployableKind::FabTurret : DeployableKind::FabAmmoPack;
                m_ctx->deployables->ServerTryPlaceDeployable(bot.id, kind);
                ++m_chaosDeployTries;
            }
            else if (bot.cls == ClassId::Medtech)
            {
                m_ctx->deployables->ServerTryPlaceDeployable(bot.id, DeployableKind::MedBeacon);
                ++m_chaosDeployTries;
            }
        }

        // Vehicle purchase through the real validated terminal path (needs a
        // friendly terminal in reach + unlock + flux; usually refused — that IS
        // the exercise; success hands nearby bots wheels for TryUseVehicle).
        if (m_ctx->vehicles && (m_rng() % 3u) == 0u)
        {
            if (m_ctx->vehicles->ServerPurchaseVehicle(bot.id, static_cast<VehicleId>(1u + (m_rng() % 3u))))
                ++m_chaosVehiclePurchases;
            ++m_chaosVehicleTries;
        }
    }

    // ---------------------------------------------------------------------------
    // Chaos pilots (W9 bots-v2): deterministic purchase + drive/fly exercise
    // ---------------------------------------------------------------------------

    bool TFBotSystem::ChaosIsPilot(const Bot& bot) const
    {
        const uint32_t slot = bot.id - kTFBotIdBase;
        return slot == kChaosVulturePilotSlot || slot == kChaosDriverPilotSlot;
    }

    void TFBotSystem::SetFarObjective(Bot& bot, const float selfPos[3])
    {
        bot.objectiveRegion = kInvalidRegion;
        bot.objectiveX = bot.objectiveZ = 2048.0f; // map-center fallback
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const ContinentDef& cont = m_ctx->data->GetContinent();
        const RegionDef* best = nullptr;
        float bestD2 = -1.0f;
        for (const RegionDef& r : cont.regions)
        {
            if (r.tier == "skyanchor")
                continue;
            const float dx = r.centerX - selfPos[0];
            const float dz = r.centerZ - selfPos[2];
            const float d2 = dx * dx + dz * dz;
            if (d2 > bestD2)
            {
                bestD2 = d2;
                best = &r;
            }
        }
        if (!best)
            return;
        bot.objectiveRegion = best->id;
        bot.objectiveX = best->centerX;
        bot.objectiveZ = best->centerZ;
    }

    void TFBotSystem::ChaosPilotTryPurchase(Bot& bot, const PawnInfo& self, double now)
    {
        bot.pilotBuyAt = now + kPilotBuyRetrySec;
        if (!m_ctx->vehicles || m_ctx->vehicles->IsSeated(bot.id))
            return;

        // A free friendly ride already parked nearby (bought last try, or the
        // pilot walked back to it): just make the march long so TryUseVehicle
        // boards it on this same think.
        bool haveRide = false;
        m_ctx->vehicles->ForEachVehicle(
            [&](const TFVehicleInfo& v)
            {
                if (haveRide || v.faction != bot.faction || v.hp <= 0.0f || v.deployed)
                    return;
                if (v.seatCount == 0 || v.seats[0] != kInvalidPlayer)
                    return;
                const float dx = v.pos[0] - self.pos[0];
                const float dz = v.pos[2] - self.pos[2];
                haveRide = dx * dx + dz * dz < kVehScanRadiusM * kVehScanRadiusM;
            });
        if (haveRide)
        {
            SetFarObjective(bot, self.pos);
            bot.vehicleRetryAt = now; // board on this very think, not after cooldown
            return;
        }

        const uint32_t slot = bot.id - kTFBotIdBase;
        const VehicleId want = slot == kChaosVulturePilotSlot ? VehicleId::Vulture : VehicleId::Drifter;
        ++m_chaosVehicleTries;
        if (m_ctx->vehicles->ServerPurchaseVehicle(bot.id, want))
        {
            ++m_chaosVehiclePurchases;
            SetFarObjective(bot, self.pos);
            bot.vehicleRetryAt = now; // walk straight to the fresh hull
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] chaos pilot 0x%08X bought vehicle %u -> far objective r%d",
                           bot.id, static_cast<unsigned>(want),
                           bot.objectiveRegion == kInvalidRegion ? -1 : static_cast<int>(bot.objectiveRegion));
        }
        else if (want == VehicleId::Vulture)
        {
            // Vulture refused (wallet drained by respawns / unlock rejected):
            // fall back to the ungated Drifter so the run still drives.
            ++m_chaosVehicleTries;
            if (m_ctx->vehicles->ServerPurchaseVehicle(bot.id, VehicleId::Drifter))
            {
                ++m_chaosVehiclePurchases;
                SetFarObjective(bot, self.pos);
                bot.vehicleRetryAt = now;
            }
        }
    }

} // namespace Terrafront
