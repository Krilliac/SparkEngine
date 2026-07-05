/**
 * @file TFRegionSystem.h
 * @brief Hex regions, conduit lattice, capture points, Dominion.
 *
 * OWNERSHIP: this header + TFRegionSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W2: regions.json topology; capture tick logic (server); TF_RegionState/TF_CaptureTick; territory persistence; Dominion detection.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFRegionSystem {
  public:
    TFRegionSystem();
    ~TFRegionSystem();

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
