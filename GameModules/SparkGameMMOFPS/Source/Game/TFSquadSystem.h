/**
 * @file TFSquadSystem.h
 * @brief Squads of 6: invite/accept/leave, squad spawn, waypoint.
 *
 * OWNERSHIP: this header + TFSquadSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W4: TF_SquadMsg ops, squad-leader spawn rule (30s cd).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFSquadSystem {
  public:
    TFSquadSystem();
    ~TFSquadSystem();

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
