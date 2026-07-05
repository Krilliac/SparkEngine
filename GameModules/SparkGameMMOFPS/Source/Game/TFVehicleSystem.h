/**
 * @file TFVehicleSystem.h
 * @brief Drifter/Aegis/Ravager: seats, driving, vehicle weapons.
 *
 * OWNERSHIP: this header + TFVehicleSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 * Plan: W3: vehicle spawn terminals (flux cost), seat management (TF_VehicleSeatOp), server-authoritative vehicle movement, Aegis deploy-spawn hook.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

namespace Terrafront {

class TFVehicleSystem {
  public:
    TFVehicleSystem();
    ~TFVehicleSystem();

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
