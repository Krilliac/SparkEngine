/**
 * @file TFScoreboard.h
 * @brief Per-faction K/D/score/regions tab screen.
 *
 * OWNERSHIP: this header + TFScoreboard.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W4: tab-hold screen, per-faction columns, region tally.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFScoreboard {
  public:
    TFScoreboard();
    ~TFScoreboard();

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
