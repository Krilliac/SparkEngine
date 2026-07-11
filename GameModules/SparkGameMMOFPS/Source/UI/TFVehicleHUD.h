/**
 * @file TFVehicleHUD.h
 * @brief W10 vehicle HUD — the seated player's cockpit widget (bottom-center):
 *        vehicle name, hull HP bar, speed readout, seat diagram (occupied
 *        seats highlighted, your seat marked, roles from VehicleDef), Aegis
 *        deployed state and Vulture altitude — plus the per-seat crosshair
 *        (armed/turret seats get a turret reticle + weapon name, unarmed
 *        passengers a minimal dot).
 *
 * OWNERSHIP: vehicle-hud lane (W10) — this header + TFVehicleHUD.cpp. Owned
 * and rendered by TFVehicleSystem (TFVehicleClient.cpp lazily constructs one
 * and calls Render from the module's RenderDebugUI hook), so no Main.cpp
 * wiring is needed. Reads ONLY the public TFVehicleSystem surface
 * (GetSeatOf / GetVehicleInfo / IsSeated) + data tables — purely visual,
 * nothing here may touch movement/collision state (determinism contract).
 *
 * All ImGui bodies compile only under SPARK_HAS_IMGUI (module rule); the
 * headless stub keeps the symbol so link lines are identical either way.
 * Speed is estimated client-side from frame-to-frame hull positions (the
 * replicated vehicle state carries pose, not speed — the same estimate works
 * identically on listen hosts and pure clients).
 */
#pragma once

#include "Core/TFTypes.h"

namespace Terrafront
{

    class TFVehicleSystem;

    class TFVehicleHUD
    {
      public:
        /// Draw the seated cockpit widget + per-seat crosshair for the local
        /// player. Call once per frame from inside the ImGui frame (the
        /// TFVehicleSystem::RenderDebugUI hook). No-op when the local player is
        /// not seated, dead, or a fullscreen UI (map/spawn) is open.
        void Render(TFGameContext& ctx, TFVehicleSystem& vehicles);

      private:
        // frame-to-frame speed estimate (reset when the seated vehicle changes)
        EntityId m_lastVehicle{0};
        float m_lastPos[3]{0.0f, 0.0f, 0.0f};
        bool m_hasLast{false};
        float m_speedSmooth{0.0f};
    };

} // namespace Terrafront
