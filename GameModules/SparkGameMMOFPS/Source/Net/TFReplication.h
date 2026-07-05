/**
 * @file TFReplication.h
 * @brief EntityReplicator wiring; TF component <-> replication field mapping.
 *
 * OWNERSHIP: this header + TFReplication.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W1: register replicated TF components as ReplicationFields; create/update packet flow; interest via ConnectionScopeFilter.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFReplication {
  public:
    TFReplication();
    ~TFReplication();

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
