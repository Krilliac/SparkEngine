/**
 * @file TFWorldSetup.h
 * @brief Scene/terrain load, WorldServer/AreaServer boot, origin-rebase driving.
 *
 * OWNERSHIP: this header + TFWorldSetup.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W1: load cindral_wastes.scene, drive WorldOriginSystem::Update each frame, boot net servers per NetRole.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFWorldSetup {
  public:
    TFWorldSetup();
    ~TFWorldSetup();

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
