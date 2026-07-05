/**
 * @file TFColossusSystem.h
 * @brief Colossus exosuit purchase + suit stats (W3).
 *
 * OWNERSHIP: this header + TFColossusSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W3 implementation (server-authoritative):
 *  - ServerPurchaseColossus(player): validated flux purchase at a friendly
 *    vehicle terminal (25 m). On success the player's pawn is swapped IN PLACE
 *    to ClassId::Colossus — a kill-free despawn + ServerSpawnPawn at the same
 *    position/facing. See the .cpp header comment for the full interaction
 *    with respawn timers and the other systems.
 *  - Suit rules come from data + existing systems, not code here:
 *      stats/weapon : classes.json id "Colossus" (2200 hp, 0 shield,
 *                     noRegen=true, primary slot colossus_autocannon ->
 *                     weapons.json "col_cannon") via TFPlayerSystem's
 *                     data-driven spawn path (verified: ServerSpawnPawn
 *                     honors arbitrary ClassId, no TFPlayerSystem edit needed)
 *      no regen     : TFDamageSystem honors ClassDef::noRegen
 *      slow / weak sprint : TFServerSim moves pawns with the class's
 *                     runSpeed/sprintSpeed and hard-caps at sprintSpeed —
 *                     Colossus sprint (5.6 m/s) is below other classes' RUN
 *                     speed, which IS the sprint clamp (data-enforced)
 *      death        : normal death path; the player respawns through the
 *                     spawn screen as whatever class they pick (Colossus is
 *                     terminal-only and rejected by spawn requests)
 *  - Anchor Lockdown ability: TF-W4.
 *
 * The orchestrator wires a tf_colossus console command to
 * ServerPurchaseColossus (TFCommands.cpp is off-limits to this agent).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

struct PawnInfo; // Game/TFPlayerSystem.h

/// Result of a ServerPurchaseColossus attempt (also the tf_colossus reply).
enum class TFColossusResult : uint8_t {
    Ok = 0,
    NotAuthority,       ///< called on a pure client
    DataMissing,        ///< data tables not loaded / no Colossus class row
    NoPawn,             ///< player has no pawn or is dead
    AlreadyColossus,    ///< pawn is already a Colossus
    NoTerminal,         ///< no friendly vehicle terminal within 25 m
    InsufficientFlux,   ///< wallet below ClassDef::fluxCost
    SpawnFailed,        ///< pawn swap failed (should not happen on authority)
};

/// Max distance from a friendly region's vehicle terminal to buy a suit.
constexpr float kTFColossusTerminalRangeM = 25.0f;

class TFColossusSystem {
  public:
    TFColossusSystem();
    ~TFColossusSystem();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

    // --- W3 public surface (orchestrator wires console/net entry points) ---

    /// Server: validate + execute a Colossus purchase for `player`.
    /// Requirements: alive pawn, not already a Colossus, within 25 m of a
    /// vehicle terminal in a region owned by the player's faction, and
    /// ClassDef::fluxCost affordable (spent via ctx.progression->ServerSpendFlux).
    TFColossusResult ServerPurchaseColossus(PlayerId player);

    /// Human-readable result text (console command replies / logs).
    static const char* ResultText(TFColossusResult r);

    uint32_t PurchaseCount() const { return m_purchases; }

  private:
    bool NearFriendlyVehicleTerminal(const PawnInfo& pawn) const;

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};

    uint32_t m_purchases{0};
    uint32_t m_rejects{0};
    TFColossusResult m_lastResult{TFColossusResult::Ok};
    PlayerId         m_lastPlayer{kInvalidPlayer};
};

} // namespace Terrafront
