/**
 * @file TFVehicleTerminal.h
 * @brief Client-side vehicle terminal props: one physical terminal console at
 *        every region's `vehicleTerminal` point (regions.json), owner-tinted
 *        like the capture banners so the pad reads friendly/hostile at range.
 *
 * OWNERSHIP: ui-polish lane (W8) — this header + TFVehicleTerminal.cpp.
 *
 * Viewer-only decor (TFRegionSystem::UpdateCaptureVisuals precedent): the
 * gameplay half of terminals already exists and is NOT duplicated here —
 * TFVehicleSystem::ClientUpdateUX draws the "[E] Vehicle terminal" prompt and
 * opens the purchase menu within kTFVehTerminalPromptM of the same
 * regions.json point, and TFVehicleSystem::ServerPurchaseVehicle validates
 * range/ownership/flux server-side. Until this system existed the terminal
 * point was an invisible spot on the sand; now the prompt has a prop under it.
 * Adding a second E-handler here would double-toggle the shop on one key
 * press (both systems would see the same edge), so this system deliberately
 * spawns meshes only.
 *
 * Mesh: Assets/Models/MMOFPS/buildings/vehicle_terminal.obj (purpose-built
 * asset already in the tree; the brief's prop_ammo_printer.obj fallback is
 * kept for the small side-clutter piece next to the console).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <cstdint>
#include <vector>

namespace Terrafront
{

    class TFVehicleTerminal
    {
      public:
        TFVehicleTerminal();
        ~TFVehicleTerminal();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

      private:
        struct Pad
        {
            RegionId region{kInvalidRegion};
            uint32_t console{0}; ///< vehicle_terminal.obj entity (0 == none)
            uint32_t clutter{0}; ///< prop_ammo_printer.obj side piece (0 == none)
            int lastOwner{-2};   ///< FactionId as int; -2 forces the first retint
        };

        void SpawnPads();
        void RetintPads();
        void DestroyPads();

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        bool m_spawned{false};

        std::vector<Pad> m_pads;
        float m_retintAccum{0.0f};
    };

} // namespace Terrafront
