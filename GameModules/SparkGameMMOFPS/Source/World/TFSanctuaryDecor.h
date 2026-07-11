/**
 * @file TFSanctuaryDecor.h
 * @brief Client-side Sanctuary Haven decor (sanc kit stamp) + the class
 *        terminal prop/interaction near the spawn pads.
 *
 * OWNERSHIP: sanctuary-v2 lane (W10) — this header + TFSanctuaryDecor.cpp.
 * Owned and driven by TFTravelSystem (constructed in its Initialize, updated
 * from its Update, drawn from its RenderUI, shut down with it) following the
 * TFRegionSystem::m_decor precedent, so no contended Main.cpp wiring is
 * needed.
 *
 * Viewer-only decor (TFRegionDecor / TFVehicleTerminal precedent): spawned
 * ONCE when the engine ECS world + TFWorldSetup are live, gated on
 * HasLocalPlayer so the dedicated server never builds it. The layout is a
 * fixed constexpr table in the .cpp — deterministic on every client by
 * construction. sanctuary_haven.scene is deliberately NOT edited (lane rule):
 * everything here is spawned like TFRegionDecor spawns region decor.
 *
 * COLLISION HONESTY: these entities are Transform+MeshRenderer ONLY — no
 * physics bodies (TFWorldCollision builds statics from the .scene files
 * alone), so players walk straight through them. The sanctuary is a safe
 * zone (no damage, no redeploy — TFSanctuaryZone.h), so walk-through props
 * are cosmetic-only by definition this wave; noted for a future
 * decor-to-collision pass alongside TFRegionDecor's.
 *
 * CLASS TERMINAL: one sanc_terminal.obj near the spawn pads. Walking within
 * kClassTerminalUseM and pressing [E] opens the EXISTING loadout/class UI via
 * TFSpawnScreen::OpenClassTerminal() (class pre-select for the next deploy —
 * see the note on that method; there is no server-side live re-class, so this
 * reuses the deploy panel rather than rebuilding a class picker).
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstdint>
#include <vector>

namespace Terrafront
{

    class TFSanctuaryDecor
    {
      public:
        TFSanctuaryDecor();
        ~TFSanctuaryDecor();

        bool Initialize(TFGameContext& ctx);
        /// Spawn-once poll + class-terminal [E] interaction. Called by
        /// TFTravelSystem::Update every frame (cheap no-op server-side).
        void Update(float deltaTime);
        void Shutdown();
        /// "[E] Class terminal" walk-up prompt (ImGui foreground; travel-
        /// terminal prompt pattern). Called by TFTravelSystem::RenderUI.
        void RenderUI();

        uint32_t SpawnedCount() const { return static_cast<uint32_t>(m_entities.size()); }

      private:
        void SpawnAll();                       ///< one deterministic stamp of the fixed layout table
        bool LocalPawn(float outPos[3]) const; ///< alive local pawn position

        TFGameContext* m_ctx{nullptr};
        bool m_initialized{false};
        bool m_spawned{false};

        std::vector<uint32_t> m_entities; ///< every decor entity, for Shutdown

        // class-terminal interaction state (travel-terminal UX pattern)
        bool m_nearClassTerm{false};
        float m_interactDebounce{0.0f};

        bool m_debugCmd{false}; ///< tf_sanctuary_debug registered by this instance
    };

} // namespace Terrafront
