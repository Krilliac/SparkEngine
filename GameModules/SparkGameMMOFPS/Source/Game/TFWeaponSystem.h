/**
 * @file TFWeaponSystem.h
 * @brief Data-driven weapons: hitscan + projectile, ADS/recoil/spread.
 *
 * OWNERSHIP: this header + TFWeaponSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W1: fire handling client+server, TF_FireEvent, ammo/reload; W4: full 21-weapon table.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFWeaponSystem {
  public:
    TFWeaponSystem();
    ~TFWeaponSystem();

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
