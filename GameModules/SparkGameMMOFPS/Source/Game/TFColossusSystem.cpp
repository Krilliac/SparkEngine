/**
 * @file TFColossusSystem.cpp
 * @brief Colossus exosuit purchase (W3) — flux-gated in-place pawn swap.
 *
 * Swap mechanics & respawn-timer interaction (documented per wave contract):
 *  - The purchase is a KILL-FREE swap: no EvPlayerKilled fires, so no death
 *    XP/killfeed, no 8 s respawn timer, and TFServerSim's m_deathTime is
 *    untouched. TFPlayerSystem::ServerSpawnPawn destroys the old pawn entity
 *    and creates the new one; because the buyer is alive, no respawn timer
 *    was pending, and ServerSpawnPawn's nextRespawnAt reset is a no-op.
 *  - Downstream systems pick the swap up through the normal spawn pipeline:
 *      EvPlayerSpawned      -> TFDamageSystem seeds the 2200/0 noRegen pool,
 *                              TFServerSim reseeds MoveState (cls=Colossus,
 *                              position/yaw re-read from the pawn Transform we
 *                              just spawned at the OLD pawn's pos/facing),
 *                              TFReplication broadcasts the new pawn Create.
 *      old entity destroyed -> TFReplication's stale-entity sweep broadcasts
 *                              the Destroy; clients rebuild the pawn visual.
 *    The old pawn's damage pool is dropped explicitly via
 *    TFDamageSystem::ServerForgetPawn (kill-free despawns skip the usual
 *    kill-path erase).
 *  - Dying as a Colossus is a normal death: standard 8 s timer, then the
 *    spawn screen respawns the player as whichever (non-Colossus) class they
 *    pick — TFPlayerSystem already rejects Colossus in TF_SpawnRequest.
 */
#include "Game/TFColossusSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "World/TFRegionSystem.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <cmath>

namespace Terrafront {

TFColossusSystem::TFColossusSystem() = default;
TFColossusSystem::~TFColossusSystem() { if (m_initialized) Shutdown(); }

bool TFColossusSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFColossusSystem initialized");
    return true;
}

// Purchases are request-driven; per-frame suit rules are enforced by data +
// the owning systems (see header). Nothing to tick.
void TFColossusSystem::Update(float deltaTime) { (void)deltaTime; }

// NOTE: Main.cpp (frozen) does not route OnFixedUpdate to this system; all
// logic is request-driven anyway. Kept as a contract no-op.
void TFColossusSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFColossusSystem::Shutdown()
{
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Purchase
// ---------------------------------------------------------------------------

const char* TFColossusSystem::ResultText(TFColossusResult r)
{
    switch (r)
    {
        case TFColossusResult::Ok:               return "Colossus deployed";
        case TFColossusResult::NotAuthority:     return "server only";
        case TFColossusResult::DataMissing:      return "data tables not loaded";
        case TFColossusResult::NoPawn:           return "you must be alive";
        case TFColossusResult::AlreadyColossus:  return "already in a Colossus";
        case TFColossusResult::NoTerminal:       return "no friendly vehicle terminal in range";
        case TFColossusResult::InsufficientFlux: return "insufficient flux";
        case TFColossusResult::SpawnFailed:      return "suit fabrication failed";
    }
    return "?";
}

bool TFColossusSystem::NearFriendlyVehicleTerminal(const PawnInfo& pawn) const
{
    if (!m_ctx->data || !m_ctx->data->IsLoaded() || !m_ctx->regions)
        return false;

    const float r2 = kTFColossusTerminalRangeM * kTFColossusTerminalRangeM;
    for (const RegionDef& region : m_ctx->data->GetContinent().regions)
    {
        if (!region.vehicleTerminal)
            continue;
        // "Friendly" terminal == the terminal's region is owned by the buyer's
        // faction (skyanchor home regions count — OwnerOf returns homeFaction).
        if (m_ctx->regions->OwnerOf(region.id) != pawn.faction)
            continue;
        const float dx = pawn.pos[0] - (*region.vehicleTerminal)[0];
        const float dz = pawn.pos[2] - (*region.vehicleTerminal)[1];
        if (dx * dx + dz * dz <= r2)
            return true;
    }
    return false;
}

TFColossusResult TFColossusSystem::ServerPurchaseColossus(PlayerId player)
{
    TFColossusResult result = TFColossusResult::Ok;

    if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players ||
        !m_ctx->progression)
        result = TFColossusResult::NotAuthority;

    const ClassDef* suit = nullptr;
    if (result == TFColossusResult::Ok)
    {
        suit = (m_ctx->data && m_ctx->data->IsLoaded())
                   ? m_ctx->data->GetClass(ClassId::Colossus) : nullptr;
        if (!suit)
            result = TFColossusResult::DataMissing;
    }

    PawnInfo pawn{};
    if (result == TFColossusResult::Ok)
    {
        if (!m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive)
            result = TFColossusResult::NoPawn;
        else if (pawn.cls == ClassId::Colossus)
            result = TFColossusResult::AlreadyColossus;
        else if (!NearFriendlyVehicleTerminal(pawn))
            result = TFColossusResult::NoTerminal;
    }

    // Spend LAST so every other rejection leaves the wallet untouched.
    if (result == TFColossusResult::Ok &&
        !m_ctx->progression->ServerSpendFlux(player, static_cast<uint32_t>(suit->fluxCost)))
        result = TFColossusResult::InsufficientFlux;

    if (result == TFColossusResult::Ok)
    {
        // Kill-free swap: forget the old pawn's damage pool (despawn without a
        // death skips the kill-path erase), then respawn in place preserving
        // position + facing. ServerSpawnPawn destroys the old entity itself.
        const float pos[3] = {pawn.pos[0], pawn.pos[1], pawn.pos[2]};
        if (m_ctx->damage)
            m_ctx->damage->ServerForgetPawn(pawn.entity);

        const EntityId suitPawn =
            m_ctx->players->ServerSpawnPawn(player, pawn.faction, ClassId::Colossus,
                                            pos, pawn.yaw);
        if (suitPawn == 0)
        {
            // Authority spawn cannot realistically fail (checked above); flux
            // is already spent — log loudly rather than silently minting flux.
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] Colossus swap failed for player %u AFTER flux spend (%d)",
                            player, suit->fluxCost);
            result = TFColossusResult::SpawnFailed;
        }
        else
        {
            ++m_purchases;
            SPARK_LOG_INFO(Spark::LogCategory::Game,
                           "[TF] player %u purchased Colossus (%d flux) -> pawn %u at (%.0f %.0f %.0f)",
                           player, suit->fluxCost, suitPawn, pos[0], pos[1], pos[2]);
        }
    }

    if (result != TFColossusResult::Ok)
        ++m_rejects;
    m_lastResult = result;
    m_lastPlayer = player;
    return result;
}

// ---------------------------------------------------------------------------
// Debug UI
// ---------------------------------------------------------------------------

void TFColossusSystem::RenderDebugUI()
{
#ifdef ENABLE_EDITOR
    if (!ImGui::CollapsingHeader("TF Colossus"))
        return;
    ImGui::Text("purchases : %u   rejects: %u", m_purchases, m_rejects);
    if (m_lastPlayer != kInvalidPlayer)
        ImGui::Text("last      : p%u -> %s", m_lastPlayer, ResultText(m_lastResult));
#endif
}

} // namespace Terrafront
