/**
 * @file TFSpawnScreen.h
 * @brief Death -> spawn point chooser (skyanchor/base/Aegis/squad).
 *
 * OWNERSHIP: this header + TFSpawnScreen.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W2: list valid spawn options (server-filtered), class picker, sends TF_SpawnRequest.
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

  private:
    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
};

} // namespace Terrafront
