/**
 * @file TFRegionDecor.h
 * @brief Client-side per-tier region decor: stamps the P2 building kit around
 *        every region center from Assets/MMOFPS/Data/decor.json templates
 *        (outpost/fort/facility/skyanchor), plus a seeded prop scatter.
 *
 * OWNERSHIP: world-decor lane (W9) — this header + TFRegionDecor.cpp +
 * Assets/MMOFPS/Data/decor.json. Driven by TFRegionSystem (constructed,
 * updated inside its HasLocalPlayer block, shut down with it) so no Main.cpp
 * wiring is needed.
 *
 * Viewer-only decor (TFRegionSystem::UpdateCaptureVisuals /
 * TFVehicleTerminal precedent): spawned ONCE when the ECS world + terrain +
 * data tables are live, gated on HasLocalPlayer so the dedicated server never
 * builds it. Deterministic on every client — fixed template offsets plus a
 * prop scatter seeded from RegionId only.
 *
 * COLLISION HONESTY: these entities are Transform+MeshRenderer ONLY. They get
 * NO physics bodies — TFWorldCollision builds statics from the .scene file
 * alone, so players/vehicles walk straight through decor this wave. Templates
 * keep >= clearanceM (8 m) from every capture point, spawn position and
 * vehicle terminal, and the code re-checks that per piece per region (a piece
 * violating clearance for a given region is skipped, deterministically, and
 * counted in tf_decor_debug). Follow-up: decor-to-collision registration.
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Terrafront
{

    struct RegionDef; // Data/TFDataTables.h

    class TFRegionDecor
    {
      public:
        TFRegionDecor();
        ~TFRegionDecor();

        bool Initialize(TFGameContext& ctx);
        /// Spawn-once poll; called by TFRegionSystem::Update inside its
        /// HasLocalPlayer block. Cheap no-op after the stamp succeeds.
        void Update();
        void Shutdown();

        uint32_t SpawnedCount() const { return static_cast<uint32_t>(m_entities.size()); }

      private:
        struct DecorPiece
        {
            std::string model;        ///< OBJ path (Assets/Models/MMOFPS/...)
            std::string material;     ///< empty = faction/neutral structure material
            float offX = 0.0f;        ///< meters east of region center
            float offZ = 0.0f;        ///< meters north of region center
            float yawDeg = 0.0f;      ///< Transform.rotation is DEGREES
            bool terrainAlign = true; ///< false = region-center height (level runs)
            bool castShadows = true;
            float emissive = 0.0f;
        };

        struct ScatterSpec
        {
            std::vector<std::string> models;
            int countMin = 0, countMax = 0;
            float radiusMin = 0.0f, radiusMax = 0.0f;
        };

        struct TierTemplate
        {
            std::vector<DecorPiece> pieces;
            ScatterSpec scatter;
        };

        bool LoadTemplates(); ///< decor.json -> m_templates (fail-soft: no decor)
        void SpawnAll();      ///< one deterministic stamp over all regions
        /// >= m_clearanceM (XZ) from every capturePoint/spawn/vehicleTerminal.
        bool ClearOfGameplay(const RegionDef& r, float x, float z) const;

        TFGameContext* m_ctx{nullptr};
        bool m_initialized{false};
        bool m_spawned{false};
        bool m_loadTried{false};
        bool m_loaded{false};

        float m_clearanceM{8.0f};
        std::unordered_map<std::string, TierTemplate> m_templates; ///< by tier name

        std::vector<uint32_t> m_entities; ///< every decor entity, for Shutdown
        uint32_t m_skippedClearance{0};   ///< pieces dropped by the gameplay-clearance check
        uint32_t m_skippedBudget{0};      ///< pieces dropped by the 20-per-region cap
        bool m_debugCmd{false};           ///< tf_decor_debug registered by this instance
    };

} // namespace Terrafront
