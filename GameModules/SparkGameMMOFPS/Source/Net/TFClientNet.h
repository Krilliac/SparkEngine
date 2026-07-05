/**
 * @file TFClientNet.h
 * @brief Client connection, ClientPrediction wiring, interpolation buffers.
 *
 * OWNERSHIP: this header + TFClientNet.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W1: connect/handshake (TF_WorldWelcome); wire Spark::Net::ClientPrediction for local pawn; interpolation for remote entities.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFClientNet {
  public:
    TFClientNet();
    ~TFClientNet();

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
