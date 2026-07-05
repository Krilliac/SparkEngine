/**
 * @file TFProgressionSystem.h
 * @brief XP events, ranks 1-30, flux income, persistence.
 *
 * OWNERSHIP: this header + TFProgressionSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W2: XP table from data, TF_XPEvent, flux tick from held regions, save via persistence pattern.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFProgressionSystem {
  public:
    TFProgressionSystem();
    ~TFProgressionSystem();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

  private:
    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
};

} // namespace Terrafront
