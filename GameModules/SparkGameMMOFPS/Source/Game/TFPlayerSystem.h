/**
 * @file TFPlayerSystem.h
 * @brief Spawn/death/respawn, class loadouts, movement tuning.
 *
 * OWNERSHIP: this header + TFPlayerSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W1: spawn pipeline (TF_SpawnRequest/Reply), pawn entity creation with TF components; class stats from classes.json.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFPlayerSystem {
  public:
    TFPlayerSystem();
    ~TFPlayerSystem();

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
