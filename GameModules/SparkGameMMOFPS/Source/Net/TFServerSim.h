/**
 * @file TFServerSim.h
 * @brief Authoritative fixed-tick simulation (movement validate, hits, capture).
 *
 * OWNERSHIP: this header + TFServerSim.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W1: 60Hz fixed tick; consume TF_ClientInput; movement validation (speed caps, terrain clamp); lag-compensated hitscan; owns server-side entity state.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFServerSim {
  public:
    TFServerSim();
    ~TFServerSim();

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
