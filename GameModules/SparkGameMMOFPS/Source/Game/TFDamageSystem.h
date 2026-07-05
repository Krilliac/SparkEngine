/**
 * @file TFDamageSystem.h
 * @brief Health/shields, TTK model, friendly fire, kill credit.
 *
 * OWNERSHIP: this header + TFDamageSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W1: 500hp+500shield regen model; TF_DamageEvent/TF_KillEvent; FF 50% + grief kick.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFDamageSystem {
  public:
    TFDamageSystem();
    ~TFDamageSystem();

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
