/**
 * @file TFSpawnScreen.h
 * @brief Death -> spawn point chooser (faction splash, class picker, spawn list).
 *
 * OWNERSHIP: this header + TFSpawnScreen.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W2 implementation:
 *  - Auto-opens 2 s after EvLocalPlayerDied (HUD keeps its "YOU ARE DOWN"
 *    overlay for those 2 s); also opens once on boot/connect when no pawn
 *    exists (first deploy / faction pick). Closes when the local pawn goes
 *    alive (covers pure-client spawns, where EvPlayerSpawned never fires).
 *  - Faction splash (three faction cards -> TF_FactionSelect) when
 *    ctx.localFaction is None; otherwise class picker (classes.json, Colossus
 *    greyed out) + spawn point list (skyanchor always, plus every region
 *    TFRegionSystem::CanSpawnAt allows, with distance from the death spot).
 *  - DEPLOY sends TF_SpawnRequest{spawnKind 0|1} with the chosen class; the
 *    respawn countdown (seeded from EvLocalPlayerDied.respawnDelay) disables
 *    the button, plus a short debounce after each request.
 *  - HUD coordination: while open, TFHUD::SetRespawnState(false) is asserted
 *    every frame so the HUD death overlay never draws underneath; if the
 *    screen is closed while still dead, the remaining countdown is handed
 *    back to the HUD overlay.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFSpawnScreen {
  public:
    TFSpawnScreen();
    ~TFSpawnScreen();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();
    void RenderUI();

    // --- W2 cross-agent contract ---
    void Open();
    void Close();
    bool IsOpen() const { return m_open; }

    // --- W2 additions ---
    /// Class currently picked in the UI; TFMapScreen uses it for click-deploys.
    ClassId SelectedClass() const { return m_selClass; }

  private:
    bool LocalPawnAlive() const;
    void OnLocalPlayerDied(const EvLocalPlayerDied& ev);
    void SendSpawnRequest();
    void SendFactionSelect(FactionId f);

    // ImGui internals (stubbed out when !SPARK_HAS_IMGUI, TFHUD pattern).
    void DrawFactionSplash(float panelX, float panelY, float panelW, float panelH);
    void DrawDeployPanel(float panelX, float panelY, float panelW, float panelH);

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};

    bool  m_open{false};
    float m_pendingOpen{0.0f};    ///< countdown to auto-open after death (0 = idle)
    float m_bootOpenDelay{1.5f};  ///< one-shot first-deploy auto-open timer
    bool  m_bootOpened{false};

    // Selection
    ClassId  m_selClass{ClassId::Striker};
    uint8_t  m_selKind{0};        ///< TF_SpawnRequest.spawnKind: 0 skyanchor, 1 region, 2 aegis
    RegionId m_selRegion{kInvalidRegion};
    EntityId m_selAegis{0};       ///< W3 shared-edit (vehicles agent): deployed-Aegis entity

    // Death / respawn state (local mirror of the server timer)
    float m_deathPos[3]{0.0f, 0.0f, 0.0f};
    float m_respawnLeft{0.0f};
    float m_debounce{0.0f};       ///< re-enable delay after a DEPLOY request
};

} // namespace Terrafront
