/**
 * @file TFHUD.h
 * @brief ImGui HUD: health/shield/ammo, crosshair, hitmarkers, killfeed, minimap.
 *
 * OWNERSHIP: this header + TFHUD.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W1 basics (health/ammo/crosshair/hitmarker); W4 killfeed/minimap polish.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFHUD {
  public:
    TFHUD();
    ~TFHUD();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();
    void RenderUI();

  private:
    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
};

} // namespace Terrafront
