/**
 * @file TFSquadHUD.h
 * @brief PS2-style squad HUD (W11 squad-v2 lane): left-edge squadmate list
 *        (name, class tag, health bar, distance + direction chevron, leader
 *        star, dead/offline graying), the squad-waypoint world beacon (tall
 *        pulsing light pillar + base diamond + distance), and the leader-only
 *        waypoint request pings with promote/dismiss controls.
 *
 * OWNERSHIP: squad-v2 lane (this header + TFSquadHUD.cpp / TFSquadHUDBeacon.cpp
 * / TFSquadHUDInternal.h, alongside the waypoint + query sections of
 * Game/TFSquadSystem.h/.cpp).
 *
 * No wire traffic of its own — every input is client-mirrored state:
 *  - roster/waypoint/requests ... TFSquadSystem (GetLocalSquadView,
 *                                 GetLocalWaypoint, PendingWaypointRequests)
 *  - pawns ..................... TFPlayerSystem::GetPawnByPlayer (replicated
 *                                 pos / class / alive / health / shield)
 *  - names ..................... TFSocialSystem::NameOfPlayer with the
 *                                 scoreboard "HOST"/"P<n>" fallback; a member
 *                                 with neither roster name nor pawn is OFFLINE
 *  - max vitals ................ classes.json ClassDef (nameplates pattern)
 *
 * The beacon projects through the SAME matrices the frame was rendered with
 * (TFWorldSetup::ComputeViewProj first-person branch) — see the MUST-MATCH
 * note in the .cpp (TFNameplates precedent) before touching either side.
 *
 * Suppressed while any fullscreen UI owns the screen (map / spawn screen /
 * scoreboard). The list renders even while the local pawn is dead (squad
 * intel feeds the spawn choice); the beacon needs the first-person camera and
 * skips dead frames.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <cstdint>

namespace Terrafront
{

    // Tuning (UI-only; nothing here affects movement, collision, or damage).
    constexpr float kTFSquadHudBeaconHeightM = 42.0f; ///< light-pillar height above terrain
    constexpr float kTFSquadHudBeaconBaseM = 2.2f;    ///< base diamond anchor above terrain
    constexpr float kTFSquadHudListAnchorY01 = 0.28f; ///< list top as a viewport-height fraction

    class TFSquadHUD
    {
      public:
        TFSquadHUD();
        ~TFSquadHUD();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI();

      private:
        // ImGui internals (stubbed out when !SPARK_HAS_IMGUI, TFHUD pattern).
        void DrawSquadList();      ///< left-edge member rows + leader controls
        void DrawWaypointBeacon(); ///< world light pillar (foreground drawlist)

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        float m_clock{0.0f}; ///< pulse animation only (not sim time)

        // Debug counters (RenderDebugUI).
        uint32_t m_rowsDrawn{0};
        bool m_beaconDrawn{false};
    };

} // namespace Terrafront
