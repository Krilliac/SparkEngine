/**
 * @file TFMapScreen.h
 * @brief Continent hex map: ownership, lattice, deploy/redeploy.
 *
 * OWNERSHIP: this header + TFMapScreen.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W2: hex render from regions.json, ownership colors, click-to-deploy integration with spawn flow.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFMapScreen {
  public:
    TFMapScreen();
    ~TFMapScreen();

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
