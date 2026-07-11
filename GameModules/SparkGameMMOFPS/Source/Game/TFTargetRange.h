/**
 * @file TFTargetRange.h
 * @brief Sanctuary firing range: 6 target dummies at 20/40/60 m that react to
 *        the LOCAL player's shots with a hit flash, a floating damage number
 *        and a per-dummy DPS readout on a holo sign.
 *
 * OWNERSHIP: sanctuary-v2 lane (W10) — this header + TFTargetRange.cpp.
 * Owned and driven by TFTravelSystem (TFRegionSystem::m_decor precedent), so
 * no contended Main.cpp wiring is needed.
 *
 * ## COSMETIC ONLY — zero server/net involvement
 * The sanctuary is a no-damage safe zone (TFSanctuaryZone.h), so these
 * dummies are PURE CLIENT PRESENTATION: Transform+MeshRenderer entities (no
 * physics, no health, not replicated, invisible to the server). Hit
 * detection is a local ray-vs-capsule test against the LOCAL player's own
 * fire ray, delivered through TFWeaponSystem::SetLocalShotHook — the same
 * post-spread ray ClientTriggerFire sends in TF_FireEvent. Displayed damage
 * is a client-side ESTIMATE (weapons.json damage x pellets with linear
 * falloff); the server's lag-compensated pipeline is never consulted and
 * never notified. Other players/bots shooting the dummies shows nothing, by
 * design (training feedback for YOUR loadout only).
 *
 * Dummies are neutral-gray class body meshes on the NE plateau edge, two
 * lanes x three ranges, behind a crate firing line. Readouts draw as
 * world-anchored ImGui foreground text (projection mirrors
 * TFWorldSetup::ComputeViewProj's first-person branch).
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstdint>
#include <vector>

namespace Terrafront
{

    struct WeaponDef; // Data/TFDataTables.h

    class TFTargetRange
    {
      public:
        TFTargetRange();
        ~TFTargetRange();

        bool Initialize(TFGameContext& ctx);
        /// Spawn-once poll, weapon-hook install, flash/floater aging. Called
        /// by TFTravelSystem::Update every frame (cheap no-op server-side).
        void Update(float deltaTime);
        void Shutdown();
        /// World-anchored hit numbers + per-dummy DPS signs (ImGui foreground).
        void RenderUI();

        uint32_t SpawnedCount() const { return static_cast<uint32_t>(m_entities.size()); }

      private:
        struct Dummy
        {
            uint32_t body{0};       ///< gray class-mesh entity (0 = none)
            float x{0.0f}, z{0.0f}; ///< feet position (Y = TerrainHeightAt)
            float y{0.0f};
            float rangeM{0.0f}; ///< nominal lane range (sign label)
            // presentation state
            float flash{0.0f};        ///< 0..1 emissive hit flash, decays in Update
            float lastDmg{0.0f};      ///< last single-shot estimate (sign line 1)
            float windowDmg{0.0f};    ///< damage inside the rolling DPS window
            double windowStart{0.0};  ///< m_clock when the current window opened
            double lastHitAt{-1.0e9}; ///< m_clock of the last hit (window/idle reset)
        };

        struct Floater
        {
            float pos[3];
            float amount;
            float age{0.0f};
        };

        void SpawnAll();
        void InstallHook();
        /// TFWeaponSystem local-shot hook target (cosmetic-only, see header).
        void OnLocalShot(const float origin[3], const float dir[3], const WeaponDef& def);
        /// Client-side damage estimate: damage x pellets with linear falloff.
        static float EstimateDamage(const WeaponDef& def, float distM);

        TFGameContext* m_ctx{nullptr};
        bool m_initialized{false};
        bool m_spawned{false};
        bool m_hooked{false};

        double m_clock{0.0}; ///< client-local seconds (DPS windows, floaters)

        std::vector<uint32_t> m_entities; ///< every range entity, for Shutdown
        std::vector<Dummy> m_dummies;     ///< size 6 once spawned
        std::vector<Floater> m_floaters;  ///< live floating damage numbers

        uint32_t m_hits{0};  ///< lifetime dummy hits (debug)
        uint32_t m_shots{0}; ///< lifetime hook invocations in sanctuary (debug)
        bool m_debugCmd{false};
    };

} // namespace Terrafront
