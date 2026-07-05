/**
 * @file TFMapScreen.h
 * @brief Continent hex map: ownership, lattice, capture progress, deploy.
 *
 * OWNERSHIP: this header + TFMapScreen.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W2 implementation:
 *  - Fullscreen ImGui overlay (toggle key M, Escape closes) rendering the 13
 *    Cindral Wastes regions as a pointy-top axial hex grid from
 *    RegionDef.hexQ/hexR, auto-fitted to the viewport.
 *  - Fill = owner faction color (TFRegionSystem::OwnerOf), contested regions
 *    pulse, capture progress renders as a ring in the capturing faction's
 *    color, conduit lines connect neighbors, skyanchors get a home marker.
 *  - Legend with per-faction region counts (TFRegionSystem::RegionsHeld).
 *  - Click a region while DEAD and TFRegionSystem::CanSpawnAt says yes ->
 *    sends TF_SpawnRequest{spawnKind=1} (class from TFSpawnScreen::
 *    SelectedClass) and closes; while ALIVE a click stores a deploy hint
 *    (DeployHintRegion — W3 redeploy consumes it).
 *  - Hover tooltip: name / tier / owner / flux per tick / capture state.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFMapScreen {
  public:
    TFMapScreen();
    ~TFMapScreen();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();
    void RenderUI();

    // --- W2 public surface (consumed by TFSpawnScreen / console commands) ---
    void Open();
    void Close();
    void Toggle();
    bool IsOpen() const { return m_open; }

    /// Region clicked while alive ("rally here"); kInvalidRegion when unset.
    /// W3 redeploy consumes this; W2 renders it as a highlighted hex.
    RegionId DeployHintRegion() const { return m_deployHint; }
    void     ClearDeployHint() { m_deployHint = kInvalidRegion; }

  private:
    bool LocalPawnAlive() const;
    void SendRegionSpawnRequest(RegionId region);

    // Drawing internals live in the .cpp (no ImGui types leak into headers);
    // per-frame hex layout is cached in plain floats.
    void DrawMapContents();     ///< everything between the overlay Begin/End

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};

    bool     m_open{false};
    RegionId m_deployHint{kInvalidRegion};
    RegionId m_hovered{kInvalidRegion};   ///< refreshed every rendered frame
    float    m_time{0.0f};                ///< contested-pulse clock

    // Per-frame layout cache (screen-space): hex size + axial origin.
    float m_hexSize{44.0f};
    float m_originX{0.0f};
    float m_originY{0.0f};
};

} // namespace Terrafront
