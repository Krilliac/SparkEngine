/**
 * @file TFColossusSystem.h
 * @brief Colossus exosuit purchase + suit stats.
 *
 * OWNERSHIP: this header + TFColossusSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W3: flux purchase at terminals, suit = pawn variant with autocannon, no regen shield.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFColossusSystem {
  public:
    TFColossusSystem();
    ~TFColossusSystem();

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
