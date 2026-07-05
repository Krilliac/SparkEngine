/**
 * @file TFDeployableSystem.h
 * @brief Fabricator turret/ammo pack, Medtech beacon.
 *
 * OWNERSHIP: this header + TFDeployableSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W3: deployable lifecycle (TFDeployableComp), limits per player, TF interact placement.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFDeployableSystem {
  public:
    TFDeployableSystem();
    ~TFDeployableSystem();

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
